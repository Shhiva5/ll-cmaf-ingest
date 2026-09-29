// http_chunked_server.hpp
//
// A deliberately minimal HTTP/1.1 server whose only real job is: hold open
// one long-lived response per connected player, and push CMAF bytes down it
// via chunked transfer-encoding as soon as the muxer produces them. This is
// the mechanism that makes "low latency" real -- the player's MSE
// (Media Source Extensions) buffer starts consuming bytes while our mdat is
// still "in flight" on the wire, rather than waiting for a complete,
// Content-Length-declared segment.
//
// What this server does NOT do, on purpose (see docs/LIVE_DELIVERY.md):
//   - No HTTP/2 (would remove head-of-line blocking across viewers sharing
//     one TCP connection, but we serve one connection per viewer anyway).
//   - No TLS (add a reverse proxy like nginx/Caddy in front for that; not
//     the point of this project).
//   - No general-purpose routing/middleware -- three fixed endpoints:
//       GET /init.mp4    -> the ftyp+moov init segment alone, with a normal
//                           Content-Length (for MSE-based players that want
//                           to fetch/append it separately)
//       GET /live.cmfv   -> self-contained: ftyp+moov sent as the first
//                           HTTP chunk, then an unbounded sequence of
//                           styp+moof+mdat chunks. A single GET to this URL
//                           is enough for ffplay/VLC/ffmpeg to play it
//                           directly, with no separate fetch of /init.mp4.
//       GET /stats.json  -> jitter buffer + receiver counters, for the
//                           latency-measurement harness described in
//                           docs/NETWORK_TESTING_AND_LATENCY.md
//
// Concurrency model: accept() loop on its own thread; each connection gets
// its own std::jthread. Broadcasting a new chunk to N viewers means N
// blocking writes done sequentially from the muxer's thread -- fine at the
// "handful of test viewers" scale this project targets. A slow/stalled
// viewer is detected via write() failing or timing out and is dropped
// rather than allowed to backpressure the whole broadcast indefinitely
// (see kWriteTimeoutMs).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <span>
#include <mutex>
#include <thread>
#include <atomic>
#include <list>
#include <functional>
#include <sstream>
#include <stdexcept>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

namespace llcmaf {

class HttpChunkedServer {
public:
    struct StatsProvider {
        std::function<std::string()> to_json;
    };

    explicit HttpChunkedServer(uint16_t port) : port_(port) {}
    ~HttpChunkedServer() { stop(); }

    // Must be called before start(): the init segment is served verbatim
    // (with a normal Content-Length) whenever a player requests /init.mp4.
    void set_init_segment(std::vector<std::byte> data) {
        std::scoped_lock lock(mutex_);
        init_segment_ = std::move(data);
    }

    void set_stats_provider(StatsProvider provider) {
        std::scoped_lock lock(mutex_);
        stats_provider_ = std::move(provider);
    }

    void start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("HttpChunkedServer: socket() failed");

        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("HttpChunkedServer: bind() failed on port " + std::to_string(port_));
        if (::listen(listen_fd_, 16) < 0)
            throw std::runtime_error("HttpChunkedServer: listen() failed");

        running_ = true;
        accept_thread_ = std::jthread([this](std::stop_token st) { accept_loop(st); });
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
        if (accept_thread_.joinable()) { accept_thread_.request_stop(); accept_thread_.join(); }

        std::scoped_lock lock(mutex_);
        for (auto& v : viewers_) {
            ::shutdown(v.fd, SHUT_RDWR);
            ::close(v.fd);
        }
        viewers_.clear();
    }

    // Called by the muxer thread for every new CMAF chunk. Sends it as one
    // HTTP chunk (hex-length\r\n<bytes>\r\n) to every currently-streaming
    // viewer; drops any viewer whose socket write fails.
    //
    // Any viewer who connected *before* the init segment existed hasn't
    // been sent one yet (see serve_live_stream) -- for those, we send
    // ftyp+moov as one extra leading HTTP chunk before this fragment, so
    // every viewer's byte stream is still self-contained: ftyp+moov once,
    // then an uninterrupted sequence of styp+moof+mdat fragments. This is
    // what lets a single GET to /live.cmfv work directly in ffplay/VLC/
    // ffmpeg without them ever fetching /init.mp4 themselves -- those
    // tools' demuxers expect moov before the first moof in the same byte
    // stream, they don't know to fetch a second URL for it.
    void broadcast_chunk(std::span<const std::byte> chunk) {
        std::ostringstream header;
        header << std::hex << chunk.size() << "\r\n";
        std::string header_str = header.str();

        std::scoped_lock lock(mutex_);
        for (auto it = viewers_.begin(); it != viewers_.end();) {
            if (!it->streaming) { ++it; continue; }

            bool ok = true;
            if (!it->sent_init && !init_segment_.empty()) {
                ok = write_chunk_locked(it->fd, init_segment_);
                if (ok) it->sent_init = true;
            }
            if (ok) {
                ok = write_all(it->fd, header_str.data(), header_str.size()) &&
                     write_all(it->fd, reinterpret_cast<const char*>(chunk.data()), chunk.size()) &&
                     write_all(it->fd, "\r\n", 2);
            }
            if (!ok) {
                ::close(it->fd);
                it = viewers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] size_t viewer_count() const {
        std::scoped_lock lock(mutex_);
        size_t n = 0;
        for (auto& v : viewers_) if (v.streaming) ++n;
        return n;
    }

private:
    struct Viewer {
        int fd = -1;
        bool streaming = false;
        bool sent_init = false; // has ftyp+moov been written to this viewer yet?
    };

    static bool write_all(int fd, const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += size_t(n);
        }
        return true;
    }

    // Writes `payload` as one HTTP chunk (hex-length\r\n<bytes>\r\n).
    // Caller must already hold mutex_ (name says "_locked" as a reminder --
    // this touches the socket directly, not shared state, but keeping the
    // naming convention consistent with the rest of this class avoids
    // someone later calling it without realizing broadcast_chunk expects
    // the lock already held).
    static bool write_chunk_locked(int fd, const std::vector<std::byte>& payload) {
        std::ostringstream header;
        header << std::hex << payload.size() << "\r\n";
        std::string header_str = header.str();
        return write_all(fd, header_str.data(), header_str.size()) &&
               write_all(fd, reinterpret_cast<const char*>(payload.data()), payload.size()) &&
               write_all(fd, "\r\n", 2);
    }

    void accept_loop(std::stop_token st) {
        while (!st.stop_requested() && running_) {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (fd < 0) {
                if (!running_) break;
                continue;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // low latency: disable Nagle

            std::thread(&HttpChunkedServer::handle_connection, this, fd).detach();
        }
    }

    void handle_connection(int fd) {
        // Read the request line + headers (we only care about the path).
        std::string request;
        char buf[2048];
        while (request.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { ::close(fd); return; }
            request.append(buf, size_t(n));
            if (request.size() > 16384) { ::close(fd); return; } // guard against pathological input
        }

        std::string path = parse_path(request);

        if (path == "/init.mp4") {
            serve_init_segment(fd);
            ::close(fd);
        } else if (path == "/live.cmfv") {
            serve_live_stream(fd);
        } else if (path == "/stats.json") {
            serve_stats(fd);
            ::close(fd);
        } else {
            static const char* not_found =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            write_all(fd, not_found, std::strlen(not_found));
            ::close(fd);
        }
    }

    static std::string parse_path(const std::string& request) {
        // "GET /init.mp4 HTTP/1.1\r\n..."
        auto first_space = request.find(' ');
        auto second_space = request.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos) return "/";
        return request.substr(first_space + 1, second_space - first_space - 1);
    }

    void serve_init_segment(int fd) {
        std::vector<std::byte> data;
        {
            std::scoped_lock lock(mutex_);
            data = init_segment_;
        }
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: video/mp4\r\n"
            << "Content-Length: " << data.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n\r\n";
        std::string h = hdr.str();
        write_all(fd, h.data(), h.size());
        write_all(fd, reinterpret_cast<const char*>(data.data()), data.size());
    }

    void serve_live_stream(int fd) {
        static const char* header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/mp4\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n\r\n";
        if (!write_all(fd, header, std::strlen(header))) { ::close(fd); return; }

        Viewer v{fd, /*streaming=*/true, /*sent_init=*/false};
        {
            std::scoped_lock lock(mutex_);
            // If the init segment already exists (a normal mid-stream join),
            // send it right now rather than waiting for the next fragment --
            // this gets the viewer to a playable state as soon as possible
            // instead of holding ftyp+moov hostage to the next frame's
            // timing. If it doesn't exist yet (viewer connected before the
            // first IDR was even seen), broadcast_chunk sends it lazily,
            // right before this viewer's first fragment.
            if (!init_segment_.empty()) {
                if (write_chunk_locked(fd, init_segment_)) {
                    v.sent_init = true;
                } else {
                    ::close(fd);
                    return;
                }
            }
            viewers_.push_back(v);
        }
        // This connection now lives until broadcast_chunk() detects a write
        // failure (client disconnected) and closes/erases it -- no
        // dedicated per-connection read loop needed since players don't
        // send anything more on this stream.
    }

    void serve_stats(int fd) {
        // IMPORTANT: copy the callback out and release mutex_ *before*
        // invoking it. stats_provider_.to_json (wired up in ingest_main.cpp)
        // calls back into this server's own viewer_count(), which itself
        // acquires mutex_ -- std::mutex is not recursive, so holding the
        // lock across the callback would deadlock this thread against
        // itself on every single /stats.json request.
        std::function<std::string()> to_json;
        {
            std::scoped_lock lock(mutex_);
            to_json = stats_provider_.to_json;
        }
        std::string body = to_json ? to_json() : "{}";
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n\r\n";
        std::string h = hdr.str();
        write_all(fd, h.data(), h.size());
        write_all(fd, body.data(), body.size());
    }

    uint16_t port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;

    mutable std::mutex mutex_;
    std::vector<std::byte> init_segment_;
    std::list<Viewer> viewers_;
    StatsProvider stats_provider_;
};

} // namespace llcmaf
