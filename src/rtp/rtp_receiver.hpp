// rtp_receiver.hpp
//
// Minimal blocking UDP receiver. Runs on its own std::jthread, reads
// datagrams into a fixed-size buffer, parses the RTP header, timestamps
// arrival with the steady_clock, and hands off to the HevcDepacketizer.
//
// Deliberately simple: one recvfrom() loop, no io_uring, no epoll. At
// realistic single-stream bitrates (a few Mbps of HEVC) a blocking socket
// on its own thread has no trouble keeping up, and it keeps the code
// readable -- which matters more for this project than raw throughput.

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "rtp_packet.hpp"
#include "hevc_depacketizer.hpp"
#include "../common/types.hpp"

namespace llcmaf {

class RtpReceiver {
public:
    // on_nal is invoked (from the receiver's background thread) for every
    // fully reassembled NAL unit.
    RtpReceiver(uint16_t port, HevcDepacketizer::NalCallback on_nal)
        : depacketizer_(std::move(on_nal)) {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) throw std::runtime_error("RtpReceiver: socket() failed");

        // Reasonably large receive buffer so a scheduling hiccup on our side
        // doesn't cause the kernel to drop packets before we read them --
        // this is a *local* safety margin, separate from the jitter buffer,
        // which absorbs network-side delay variation.
        int rcvbuf = 4 * 1024 * 1024;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(sock_);
            throw std::runtime_error("RtpReceiver: bind() failed on port " + std::to_string(port));
        }
    }

    ~RtpReceiver() {
        stop();
        if (sock_ >= 0) ::close(sock_);
    }

    RtpReceiver(const RtpReceiver&) = delete;
    RtpReceiver& operator=(const RtpReceiver&) = delete;

    void start() {
        running_ = true;
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.request_stop();
            // Unblock the recvfrom() by shutting the socket down.
            ::shutdown(sock_, SHUT_RDWR);
            thread_.join();
        }
    }

    // Simple packet-loss/reorder counters for the latency report (Week 4).
    struct Stats {
        uint64_t packets_received = 0;
        uint64_t sequence_gaps = 0; // count of detected gaps (proxy for loss)
    };
    [[nodiscard]] Stats stats() const { return stats_; }

private:
    void run(std::stop_token st) {
        constexpr size_t kMaxDatagram = 65536;
        std::vector<std::byte> buf(kMaxDatagram);

        bool have_seq = false;
        uint16_t expected_seq = 0;

        while (!st.stop_requested() && running_) {
            ssize_t n = ::recv(sock_, buf.data(), buf.size(), 0);
            if (n <= 0) {
                if (!running_) break;
                continue; // interrupted or transient error; loop will exit via stop()
            }

            MonoNs arrival = now_mono_ns();
            auto hdr = parse_rtp_header(std::span<const std::byte>(buf.data(), size_t(n)));
            if (!hdr) continue;

            stats_.packets_received++;
            if (have_seq && hdr->sequence_number != expected_seq) {
                stats_.sequence_gaps++;
            }
            expected_seq = uint16_t(hdr->sequence_number + 1);
            have_seq = true;

            depacketizer_.on_rtp_packet(*hdr, arrival);
        }
    }

    int sock_ = -1;
    std::atomic<bool> running_{false};
    std::jthread thread_;
    HevcDepacketizer depacketizer_;
    Stats stats_;
};

} // namespace llcmaf
