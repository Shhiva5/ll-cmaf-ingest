// ingest_main.cpp
//
// End-to-end wiring of the pipeline described in docs/ARCHITECTURE.md:
//
//   UDP/RTP (from ffmpeg) --> RtpReceiver --> HevcDepacketizer (inside
//   RtpReceiver) --> AccessUnitAssembler (groups NALs by RTP timestamp)
//   --> JitterBuffer --> CmafMuxer --> HttpChunkedServer --> browser/ffplay
//
// Run `ll_cmaf_ingest --help` for CLI options. See docs for the
// reasoning behind each stage and scripts/gen_test_stream.sh for how to
// point a real ffmpeg encode at this program.

#include <iostream>
#include <sstream>
#include <csignal>
#include <atomic>
#include <optional>
#include <cstring>
#include <thread>
#include <chrono>

#include "common/types.hpp"
#include "common/byte_io.hpp"
#include "hevc/hevc_sps_parser.hpp"
#include "rtp/rtp_receiver.hpp"
#include "jitter/jitter_buffer.hpp"
#include "mux/cmaf_muxer.hpp"
#include "mux/hvcc_builder.hpp"
#include "server/http_chunked_server.hpp"

using namespace llcmaf;

namespace {

std::atomic<bool> g_shutdown{false};
void handle_sigint(int) { g_shutdown = true; }

struct Args {
    uint16_t rtp_port = 5004;
    uint16_t http_port = 8080;
    JitterBuffer::Mode jitter_mode = JitterBuffer::Mode::Adaptive;
    int64_t fixed_delay_ms = 100;
    int64_t base_delay_ms = 40;
    double jitter_multiplier = 4.0;
    uint32_t rtp_clock_hz = 90000;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--rtp-port") a.rtp_port = uint16_t(std::stoi(next()));
        else if (arg == "--http-port") a.http_port = uint16_t(std::stoi(next()));
        else if (arg == "--jitter-mode") {
            std::string v = next();
            a.jitter_mode = (v == "fixed") ? JitterBuffer::Mode::Fixed : JitterBuffer::Mode::Adaptive;
        }
        else if (arg == "--fixed-delay-ms") a.fixed_delay_ms = std::stoll(next());
        else if (arg == "--base-delay-ms") a.base_delay_ms = std::stoll(next());
        else if (arg == "--jitter-multiplier") a.jitter_multiplier = std::stod(next());
        else if (arg == "--help") {
            std::cout <<
                "ll_cmaf_ingest [options]\n"
                "  --rtp-port PORT         UDP port to receive HEVC RTP on (default 5004)\n"
                "  --http-port PORT        HTTP port to serve LL-CMAF on (default 8080)\n"
                "  --jitter-mode MODE      'fixed' or 'adaptive' (default adaptive)\n"
                "  --fixed-delay-ms MS     delay when --jitter-mode=fixed (default 100)\n"
                "  --base-delay-ms MS      adaptive mode floor delay (default 40)\n"
                "  --jitter-multiplier K   adaptive mode: delay = base + K*jitter (default 4.0)\n";
            std::exit(0);
        }
        else throw std::runtime_error("unknown argument: " + arg);
    }
    return a;
}

// Groups NAL units emitted by the depacketizer (one at a time, in arrival
// order) into AccessUnits by RTP timestamp. HEVC/AV1 access units are, by
// definition, all the NALs sharing one presentation timestamp; ffmpeg's RTP
// sender emits them contiguously, so a simple "flush when the timestamp
// changes" assembler is sufficient here (no B-frame reordering at the RTP
// layer in our test configuration -- see docs/RTP_DEPACKETIZATION.md "Assumptions").
class AccessUnitAssembler {
public:
    using AuCallback = std::function<void(AccessUnit)>;
    explicit AccessUnitAssembler(AuCallback on_au) : on_au_(std::move(on_au)) {}

    void on_nal(NalUnit nal) {
        if (have_current_ && nal.rtp_timestamp != current_.rtp_timestamp) {
            flush();
        }
        if (!have_current_) {
            current_ = AccessUnit{};
            current_.rtp_timestamp = nal.rtp_timestamp;
            have_current_ = true;
        }
        if (nal.is_idr()) current_.is_idr = true;
        current_.nals.push_back(std::move(nal));
    }

    void flush() {
        if (have_current_ && !current_.nals.empty()) {
            on_au_(std::move(current_));
        }
        current_ = AccessUnit{};
        have_current_ = false;
    }

private:
    AuCallback on_au_;
    AccessUnit current_;
    bool have_current_ = false;
};

// Owns the "have we built the init segment yet" state machine: captures
// VPS/SPS/PPS from the stream itself (rather than requiring them to be
// supplied out of band), parses the SPS for resolution/profile, and builds
// the CMAF init segment exactly once.
class InitSegmentBuilder {
public:
    // Returns the init segment the first time all three parameter sets and
    // a valid SPS parse are available; std::nullopt on every call before
    // and after that (caller checks has_built() to avoid rebuilding).
    std::optional<std::vector<std::byte>> feed(const AccessUnit& au, CmafMuxer& muxer) {
        if (built_) return std::nullopt;

        for (const auto& nal : au.nals) {
            if (nal.is_vps()) params_.vps = nal.payload;
            else if (nal.is_sps()) params_.sps = nal.payload;
            else if (nal.is_pps()) params_.pps = nal.payload;
        }

        if (params_.vps.empty() || params_.sps.empty() || params_.pps.empty()) {
            return std::nullopt;
        }

        std::span<const std::byte> sps_no_header(params_.sps.data() + 2, params_.sps.size() - 2);
        auto rbsp = strip_emulation_prevention(sps_no_header);
        auto sps_info = parse_hevc_sps(rbsp);
        if (!sps_info) {
            std::cerr << "warning: failed to parse SPS, waiting for a cleaner one\n";
            return std::nullopt;
        }

        sps_info_ = *sps_info;
        built_ = true;
        return muxer.build_init_segment(*sps_info_, params_);
    }

    [[nodiscard]] bool has_built() const { return built_; }
    [[nodiscard]] const HevcSpsInfo& sps_info() const { return *sps_info_; }

private:
    HevcParamSets params_;
    std::optional<HevcSpsInfo> sps_info_;
    bool built_ = false;
};

} // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "argument error: " << e.what() << "\n";
        return 1;
    }

    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    // Width/height are patched into the muxer's stsd/tkhd only via the
    // config passed at construction; since we don't know them until the
    // first SPS arrives, we rebuild the muxer's *config* copy is avoided by
    // keeping width/height at 0 in stsd -- most MSE-based LL-HLS/DASH
    // players ignore the sample entry's width/height and take the real
    // dimensions from the decoder, so this is a documented simplification
    // (see docs/MUXER.md) rather than a correctness bug for playback.
    CmafMuxer muxer(CmafMuxerConfig{args.rtp_clock_hz, 1920, 1080});
    InitSegmentBuilder init_builder;
    HttpChunkedServer http(args.http_port);

    uint32_t last_au_rtp_ts = 0;
    bool have_last_au = false;
    uint32_t default_sample_duration = args.rtp_clock_hz / 25; // fallback until 2 frames seen
    uint32_t first_au_rtp_ts = 0;

    // --- JitterBuffer: release callback muxes and broadcasts each AU ------
    JitterBuffer::Config jb_cfg;
    jb_cfg.mode = args.jitter_mode;
    jb_cfg.fixed_delay_ms = args.fixed_delay_ms;
    jb_cfg.base_delay_ms = args.base_delay_ms;
    jb_cfg.jitter_multiplier = args.jitter_multiplier;
    jb_cfg.rtp_clock_hz = args.rtp_clock_hz;

    JitterBuffer jitter_buffer(jb_cfg, [&](AccessUnit au) {
        if (!init_builder.has_built()) {
            if (auto init_seg = init_builder.feed(au, muxer)) {
                http.set_init_segment(*init_seg);
                std::cout << "[init] built init segment (" << init_seg->size() << " bytes)\n";
            } else {
                return; // not enough parameter sets yet; can't mux this AU
            }
        }

        if (!have_last_au) {
            first_au_rtp_ts = au.rtp_timestamp;
            have_last_au = true;
        } else {
            uint32_t delta = au.rtp_timestamp - last_au_rtp_ts;
            if (delta > 0) default_sample_duration = delta; // track real spacing
        }

        uint64_t base_decode_time = uint64_t(au.rtp_timestamp - first_au_rtp_ts);
        last_au_rtp_ts = au.rtp_timestamp;

        std::vector<AccessUnit> single{std::move(au)};
        auto chunk = muxer.mux_chunk(single, base_decode_time, default_sample_duration);
        http.broadcast_chunk(chunk);
    });

    // --- AccessUnitAssembler + RtpReceiver ---------------------------------
    AccessUnitAssembler assembler([&](AccessUnit au) { jitter_buffer.push(std::move(au)); });

    RtpReceiver receiver(args.rtp_port, [&](NalUnit nal) { assembler.on_nal(std::move(nal)); });

    // --- Stats endpoint -----------------------------------------------------
    http.set_stats_provider({[&]() -> std::string {
        auto jstats = jitter_buffer.stats();
        auto rstats = receiver.stats();
        std::ostringstream j;
        j << "{"
          << "\"jitter_estimate_ms\":" << jstats.jitter_estimate_ms << ","
          << "\"current_delay_ms\":" << jstats.current_delay_ms << ","
          << "\"frames_released\":" << jstats.frames_released << ","
          << "\"packets_received\":" << rstats.packets_received << ","
          << "\"sequence_gaps\":" << rstats.sequence_gaps << ","
          << "\"viewers\":" << http.viewer_count()
          << "}";
        return j.str();
    }});

    std::cout << "LL-CMAF ingest engine starting\n"
              << "  RTP listen port : " << args.rtp_port << "\n"
              << "  HTTP port       : " << args.http_port << "\n"
              << "  jitter mode     : " << (args.jitter_mode == JitterBuffer::Mode::Adaptive ? "adaptive" : "fixed") << "\n"
              << "  init segment    : http://localhost:" << args.http_port << "/init.mp4\n"
              << "  live stream     : http://localhost:" << args.http_port << "/live.cmfv\n"
              << "  stats           : http://localhost:" << args.http_port << "/stats.json\n"
              << "Waiting for an ffmpeg RTP source... (Ctrl+C to stop)\n";

    jitter_buffer.start();
    http.start();
    receiver.start();

    while (!g_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down...\n";
    receiver.stop();
    jitter_buffer.stop();
    http.stop();
    return 0;
}
