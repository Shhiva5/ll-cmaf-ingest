// test_jitter_buffer.cpp
//
// These tests exercise real wall-clock timing (the playout thread sleeps
// against std::chrono::steady_clock), so they use small millisecond-scale
// delays and generous tolerances rather than trying to be sub-millisecond
// precise -- that trade-off is intentional and documented inline.

#include "mini_test.hpp"
#include "jitter/jitter_buffer.hpp"
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>

using namespace llcmaf;
using namespace std::chrono_literals;

TEST(fixed_delay_buffer_reorders_out_of_order_frames) {
    JitterBuffer::Config cfg;
    cfg.mode = JitterBuffer::Mode::Fixed;
    cfg.fixed_delay_ms = 60;
    cfg.rtp_clock_hz = 90000;

    std::mutex out_mutex;
    std::vector<uint32_t> released_order;

    JitterBuffer jb(cfg, [&](AccessUnit au) {
        std::scoped_lock lock(out_mutex);
        released_order.push_back(au.rtp_timestamp);
    });
    jb.start();

    // Push access units 0, 2, 1 (out of PTS order) in quick succession --
    // the buffer must still release them in PTS order: 0, 1, 2.
    AccessUnit a0; a0.rtp_timestamp = 0;
    AccessUnit a1; a1.rtp_timestamp = 9000;  // +100ms at 90kHz
    AccessUnit a2; a2.rtp_timestamp = 18000; // +200ms at 90kHz

    jb.push(a0);
    jb.push(a2);
    jb.push(a1);

    std::this_thread::sleep_for(400ms); // well past fixed_delay + all PTS offsets
    jb.stop();

    std::scoped_lock lock(out_mutex);
    ASSERT_EQ(released_order.size(), size_t(3));
    ASSERT_EQ(released_order[0], 0u);
    ASSERT_EQ(released_order[1], 9000u);
    ASSERT_EQ(released_order[2], 18000u);
}

TEST(fixed_delay_buffer_does_not_release_before_deadline) {
    JitterBuffer::Config cfg;
    cfg.mode = JitterBuffer::Mode::Fixed;
    cfg.fixed_delay_ms = 200;

    std::atomic<int> release_count{0};
    JitterBuffer jb(cfg, [&](AccessUnit) { release_count++; });
    jb.start();

    AccessUnit a0; a0.rtp_timestamp = 0;
    jb.push(a0);

    std::this_thread::sleep_for(60ms);
    // 60ms < 200ms delay -- nothing should have been released yet.
    ASSERT_EQ(release_count.load(), 0);

    std::this_thread::sleep_for(200ms); // now past the 200ms deadline
    ASSERT_EQ(release_count.load(), 1);

    jb.stop();
}

TEST(adaptive_buffer_grows_delay_under_simulated_jitter) {
    JitterBuffer::Config cfg;
    cfg.mode = JitterBuffer::Mode::Adaptive;
    cfg.base_delay_ms = 20;
    cfg.jitter_multiplier = 4.0;
    cfg.min_delay_ms = 20;
    cfg.max_delay_ms = 1000;

    JitterBuffer jb(cfg, [](AccessUnit) {});
    jb.start();

    // Feed frames whose *arrival* jitter varies a lot relative to their PTS
    // spacing, by controlling wall-clock push timing directly (push() reads
    // now_mono_ns() internally as the "arrival" time).
    uint32_t rtp_ts = 0;
    for (int i = 0; i < 20; ++i) {
        AccessUnit au;
        au.rtp_timestamp = rtp_ts;
        jb.push(au);
        rtp_ts += 9000; // 100ms of PTS per frame at 90kHz

        // Alternate between near-instant and delayed pushes to inject
        // variance into inter-arrival spacing vs. PTS spacing.
        if (i % 2 == 0) std::this_thread::sleep_for(20ms);
        else std::this_thread::sleep_for(180ms);
    }

    auto stats = jb.stats();
    jb.stop();

    // With that much induced variance, the adaptive delay must have grown
    // meaningfully above the floor -- we don't assert an exact value since
    // EWMA convergence depends on scheduler timing, just that it moved.
    ASSERT_TRUE(stats.current_delay_ms > cfg.base_delay_ms);
    ASSERT_TRUE(stats.jitter_estimate_ms > 0.0);
}

TEST(loss_causes_skip_not_indefinite_stall) {
    // If a frame's PTS slot is simply never pushed, the buffer must not
    // block the *next* frame forever -- it should still release later
    // frames once their own deadlines pass.
    JitterBuffer::Config cfg;
    cfg.mode = JitterBuffer::Mode::Fixed;
    cfg.fixed_delay_ms = 50;

    std::mutex out_mutex;
    std::vector<uint32_t> released;
    JitterBuffer jb(cfg, [&](AccessUnit au) {
        std::scoped_lock lock(out_mutex);
        released.push_back(au.rtp_timestamp);
    });
    jb.start();

    AccessUnit a0; a0.rtp_timestamp = 0;
    // a1 (rtp_timestamp = 9000) is deliberately never pushed -- simulates
    // total loss of that frame.
    AccessUnit a2; a2.rtp_timestamp = 18000;

    jb.push(a0);
    jb.push(a2);

    std::this_thread::sleep_for(400ms);
    jb.stop();

    std::scoped_lock lock(out_mutex);
    ASSERT_EQ(released.size(), size_t(2));
    ASSERT_EQ(released[0], 0u);
    ASSERT_EQ(released[1], 18000u);
}
