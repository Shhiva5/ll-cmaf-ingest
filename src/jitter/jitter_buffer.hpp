// jitter_buffer.hpp
//
// Reorders incoming access units by presentation time and releases them to
// the muxer at a controlled, adaptively-delayed pace, absorbing network
// jitter without either (a) stalling indefinitely on a late or lost frame,
// or (b) releasing frames early enough that a slightly-late arrival causes
// a visible stall downstream.
//
// ---------------------------------------------------------------------------
// Two operating modes, selected at construction:
//
//   Mode::Fixed    - constant playout delay. A single parameter, useful as
//                    a baseline while validating the muxer and delivery
//                    layers independently of the jitter estimator.
//   Mode::Adaptive - RFC 3550 section 6.4.1-style interarrival jitter
//                    estimate drives the delay up and down.
//
// ---------------------------------------------------------------------------
// Adaptive delay algorithm:
//
//   For each arriving access unit, "transit time" is computed as
//   arrival_time - pts (both in the same monotonic clock domain once pts is
//   derived from the RTP timestamp plus an offset locked on the first
//   frame). The jitter estimate is the EWMA of the absolute change in
//   transit time between consecutive frames -- RFC 3550's definition,
//   applied to a jitter buffer delay decision rather than an RTCP report:
//
//       D(i) = (transit(i) - transit(i-1))
//       J(i) = J(i-1) + (|D(i)| - J(i-1)) / 16
//
//   Target playout delay = base_delay_ms + k * J, clamped to
//   [min_delay_ms, max_delay_ms].
//
//   To limit audible/visible churn, delay is permitted to increase
//   immediately (protecting against a fresh burst of jitter) but only
//   permitted to decrease gradually (a few ms per second), matching the
//   asymmetric-hysteresis approach used by WebRTC's NetEq and comparable
//   adaptive jitter buffers.
//
// ---------------------------------------------------------------------------
// Loss handling: if the next access unit in PTS order has not arrived by
// the time its playout deadline passes, the buffer does not block. It
// advances to the next available access unit. If that access unit is not
// an IDR (i.e. part of a GOP has been skipped), the muxer and downstream
// player will show corruption until the next IDR -- a deliberate
// simplification documented in docs/LIVE_DELIVERY.md; concealment
// (NACK/retransmit or frame repeat) is documented there as an extension
// path, not implemented here.

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <optional>
#include <algorithm>
#include <cmath>

#include "../common/types.hpp"

namespace llcmaf {

class JitterBuffer {
public:
    enum class Mode { Fixed, Adaptive };

    struct Config {
        Mode mode = Mode::Adaptive;
        int64_t fixed_delay_ms = 100;     // used when mode == Fixed
        int64_t base_delay_ms = 40;       // adaptive: floor added to jitter term
        double  jitter_multiplier = 4.0;  // adaptive: k in base + k*J
        int64_t min_delay_ms = 40;
        int64_t max_delay_ms = 1000;
        int64_t max_decrease_per_sec_ms = 5; // hysteresis: shrink slowly
        uint32_t rtp_clock_hz = 90000;
    };

    using ReleaseCallback = std::function<void(AccessUnit)>;

    explicit JitterBuffer(Config cfg, ReleaseCallback on_release)
        : cfg_(cfg), on_release_(std::move(on_release)) {}

    ~JitterBuffer() { stop(); }

    void start() {
        running_ = true;
        thread_ = std::jthread([this](std::stop_token st) { playout_loop(st); });
    }

    void stop() {
        running_ = false;
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.request_stop();
            cv_.notify_all();
            thread_.join();
        }
    }

    // Called from the RTP/depacketizer thread whenever a new access unit is
    // fully reassembled. Thread-safe.
    void push(AccessUnit au) {
        std::scoped_lock lock(mutex_);

        MonoNs arrival = now_mono_ns();
        if (!have_first_) {
            first_rtp_ts_ = au.rtp_timestamp;
            first_arrival_ns_ = arrival;
            have_first_ = true;
        }
        au.pts_ns = rtp_to_pts_ns(au.rtp_timestamp);

        update_jitter_estimate(au.pts_ns, arrival);
        recompute_target_delay();

        // Insertion sort by PTS -- access units arrive nearly in order
        // (HEVC/AV1 with no B-frames in our test config), so this is O(1)
        // amortized in practice, not O(n).
        auto it = std::upper_bound(
            queue_.begin(), queue_.end(), au,
            [](const AccessUnit& a, const AccessUnit& b) { return a.pts_ns < b.pts_ns; });
        queue_.insert(it, std::move(au));

        cv_.notify_one();
    }

    struct Stats {
        double jitter_estimate_ms = 0.0;
        int64_t current_delay_ms = 0;
        uint64_t frames_released = 0;
        uint64_t frames_skipped_for_loss = 0;
    };
    [[nodiscard]] Stats stats() const {
        std::scoped_lock lock(mutex_);
        return stats_;
    }

private:
    MonoNs rtp_to_pts_ns(uint32_t rtp_ts) const {
        // Handles 32-bit RTP timestamp wraparound via signed difference.
        int32_t delta = int32_t(rtp_ts - first_rtp_ts_);
        int64_t delta_ns = (int64_t(delta) * 1'000'000'000LL) / cfg_.rtp_clock_hz;
        return first_arrival_ns_ + delta_ns;
    }

    void update_jitter_estimate(MonoNs pts_ns, MonoNs arrival_ns) {
        if (cfg_.mode != Mode::Adaptive) return;
        int64_t transit_ns = arrival_ns - pts_ns;
        if (have_last_transit_) {
            double d_ms = double(transit_ns - last_transit_ns_) / 1e6;
            jitter_estimate_ms_ += (std::fabs(d_ms) - jitter_estimate_ms_) / 16.0;
        }
        last_transit_ns_ = transit_ns;
        have_last_transit_ = true;
        stats_.jitter_estimate_ms = jitter_estimate_ms_;
    }

    void recompute_target_delay() {
        int64_t desired_ms;
        if (cfg_.mode == Mode::Fixed) {
            desired_ms = cfg_.fixed_delay_ms;
        } else {
            desired_ms = cfg_.base_delay_ms +
                         int64_t(cfg_.jitter_multiplier * jitter_estimate_ms_);
            desired_ms = std::clamp(desired_ms, cfg_.min_delay_ms, cfg_.max_delay_ms);
        }

        if (desired_ms > current_delay_ms_) {
            current_delay_ms_ = desired_ms; // grow immediately on jitter spikes
        } else if (desired_ms < current_delay_ms_) {
            // shrink gradually -- see file header for rationale
            MonoNs now = now_mono_ns();
            double sec_since_shrink = double(now - last_shrink_ns_) / 1e9;
            int64_t allowed = int64_t(sec_since_shrink * double(cfg_.max_decrease_per_sec_ms));
            if (allowed > 0) {
                current_delay_ms_ = std::max(desired_ms, current_delay_ms_ - allowed);
                last_shrink_ns_ = now;
            }
        }
        stats_.current_delay_ms = current_delay_ms_;
    }

    void playout_loop(std::stop_token st) {
        std::unique_lock lock(mutex_);
        while (!st.stop_requested() && running_) {
            if (queue_.empty()) {
                cv_.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }

            MonoNs deadline = queue_.front().pts_ns + current_delay_ms_ * 1'000'000LL;
            MonoNs now = now_mono_ns();

            if (now < deadline) {
                auto wait_ns = std::chrono::nanoseconds(deadline - now);
                cv_.wait_for(lock, wait_ns);
                continue; // re-check queue/deadline after waking (new arrivals may reorder)
            }

            AccessUnit au = std::move(queue_.front());
            queue_.pop_front();
            stats_.frames_released++;

            lock.unlock();
            on_release_(std::move(au));
            lock.lock();
        }
    }

    Config cfg_;
    ReleaseCallback on_release_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AccessUnit> queue_;
    std::atomic<bool> running_{false};
    std::jthread thread_;

    bool have_first_ = false;
    uint32_t first_rtp_ts_ = 0;
    MonoNs first_arrival_ns_ = 0;

    bool have_last_transit_ = false;
    int64_t last_transit_ns_ = 0;
    double jitter_estimate_ms_ = 0.0;
    int64_t current_delay_ms_ = 0;
    MonoNs last_shrink_ns_ = 0;

    Stats stats_;
};

} // namespace llcmaf
