// spectragen — Production CLI for SpectralVisualizationGenerator.
// Uses MediaDecoder, fft.h, SpectralDataset, and renderers directly.
// No GUI, no GPU, no duplicated DSP logic beyond STFT loop.

#include "pipeline.h"
#include "batch.h"
#include "multiband_analyzer.h"
#include "spectral_dataset.h"

#include <atomic>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static std::atomic<bool> g_cancel{false};
static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_cancel.store(true);
        return TRUE;
    }
    return FALSE;
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Version
// ============================================================================
static constexpr const char* VERSION = "0.1.0";

// ============================================================================
// Exit codes
// ============================================================================
enum ExitCode : int {
    OK              = 0,
    BadArgs         = 1,
    FileNotFound    = 2,
    DecodeError     = 3,
    AnalysisError   = 4,
    RenderError     = 5,
    DependencyError = 6,
};

// ============================================================================
// CLI configuration — extends shared pipeline config with CLI-only flags.
// ============================================================================
struct CliConfig : public Spectral::GenerateConfig {
    bool show_help = false;
    bool show_version = false;
    bool multiband = false;
    // Batch mode: spectragen batch <input> <output> [options]
    bool batch_mode = false;
    bool recursive = false;
    int jobs = 0;
    int retries = 1;
    std::string exts;  // comma-separated, empty = known media exts
};

// ============================================================================
// Usage / help
// ============================================================================
static void print_usage() {
    std::cout <<
        "spectragen — generate spectrogram or spectrum visualization from audio\n"
        "\n"
        "Usage:\n"
        "  spectragen <input> --output <output.png> [options]\n"
        "\n"
        "Options:\n"
        "  -o, --output <path>         Output PNG file (required)\n"
        "  -v, --visualization <type>  spectrogram | spectrum (default: spectrogram)\n"
        "  --fft <size>                FFT size, power of 2 (default: 1024)\n"
        "  --hop <samples>             Hop size (default: fft/2)\n"
        "  --window <type>             hann | hamming | blackman | rectangular (default: hann)\n"
        "  --overlap <ratio>           Overlap 0..1 (default: 0.5)\n"
        "  --min-frequency <hz>        Minimum frequency in Hz (default: 0)\n"
        "  --max-frequency <hz>        Maximum frequency in Hz (default: auto/nyquist)\n"
        "  --db-range <db>             Dynamic range in dB (default: 80)\n"
        "  --resolution <WxH>          Output dimensions (default: 1024x512)\n"
        "  --output-format <type>      image | video (auto-detected from extension)\n"
        "  --fps <n>                   Video framerate (default: 30)\n"
        "  --codec <name>              libx264 | libx265 | libvpx-vp9 (default: libx264)\n"
        "  --crf <0-51>                Quality (lower=better, default: 18)\n"
        "  --duration <seconds>        Time window for video frames (default: 5.0)\n"
        "  --freq-scale <scale>        linear | log | mel | bark | erb | cqt (default: log)\n"
        "  --cqt-center <hz>           CQT center frequency (default: 440)\n"
        "  --cqt-q <factor>            CQT quality factor / bins per octave (default: 12)\n"
        "  --reassigned                Use time-frequency reassignment for improved resolution\n"
        "  --multiband                 Compare fixed/short/long/multi-band STFT\n"
        "  --gpu                       Use GPU spectrogram rendering (CPU fallback)\n"
        "\n"
        "Batch:\n"
        "  spectragen batch <input> --output <dir> [options]\n"
        "  <input>                     File or folder (folders scanned for media)\n"
        "  --recursive                 Scan folders recursively\n"
        "  --jobs <n>                  Parallel jobs, 0 = auto (default: 0)\n"
        "  --retries <n>               Extra attempts per file (default: 1)\n"
        "  --ext <a,b,c>               Extension filter, e.g. wav,mp3 (default: all media)\n"
        "  -h, --help                  Show this help\n"
        "  -V, --version               Show version\n"
        "\n"
        "Exit codes:\n"
        "  0  success\n"
        "  1  invalid arguments\n"
        "  2  input file not found\n"
        "  3  decode error (requires ffmpeg in PATH)\n"
        "  4  analysis error\n"
        "  5  render error\n"
        "  6  dependency missing (ffmpeg not found)\n"
        "\n"
        "Examples:\n"
        "  spectragen audio.wav -o spectrogram.png\n"
        "  spectragen music.mp4 -o spectrum.png -v spectrum --fft 2048\n"
        "  spectragen recording.wav -o out.png --resolution 1920x1080 --db-range 100\n";
}

// ============================================================================
// Argument parsing
// ============================================================================
static int parse_args(int argc, char* argv[], CliConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            cfg.show_help = true;
            return 0;
        }
        if (arg == "-V" || arg == "--version") {
            cfg.show_version = true;
            return 0;
        }

        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.output_path = argv[++i];
            continue;
        }
        if (arg == "-v" || arg == "--visualization") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.visualization = argv[++i];
            if (cfg.visualization != "spectrogram" && cfg.visualization != "spectrum") {
                std::cerr << "Error: --visualization must be 'spectrogram' or 'spectrum'\n";
                return 1;
            }
            continue;
        }
        if (arg == "--fft") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.fft_size = std::atoi(argv[++i]);
            if (cfg.fft_size <= 0 || (cfg.fft_size & (cfg.fft_size - 1)) != 0) {
                std::cerr << "Error: --fft must be a positive power of 2\n";
                return 1;
            }
            continue;
        }
        if (arg == "--hop") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.hop_size = std::atoi(argv[++i]);
            if (cfg.hop_size <= 0) { std::cerr << "Error: --hop must be positive\n"; return 1; }
            continue;
        }
        if (arg == "--window") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.window = argv[++i];
            if (cfg.window != "hann" && cfg.window != "hamming" &&
                cfg.window != "blackman" && cfg.window != "rectangular") {
                std::cerr << "Error: --window must be hann, hamming, blackman, or rectangular\n";
                return 1;
            }
            continue;
        }
        if (arg == "--overlap") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.overlap = std::atof(argv[++i]);
            if (cfg.overlap < 0.0f || cfg.overlap >= 1.0f) {
                std::cerr << "Error: --overlap must be in [0, 1)\n";
                return 1;
            }
            continue;
        }
        if (arg == "--min-frequency") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.min_freq = std::atof(argv[++i]);
            if (cfg.min_freq < 0) { std::cerr << "Error: --min-frequency must be >= 0\n"; return 1; }
            continue;
        }
        if (arg == "--max-frequency") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.max_freq = std::atof(argv[++i]);
            if (cfg.max_freq < 0) { std::cerr << "Error: --max-frequency must be >= 0\n"; return 1; }
            continue;
        }
        if (arg == "--db-range") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.db_range = std::atof(argv[++i]);
            if (cfg.db_range <= 0) { std::cerr << "Error: --db-range must be > 0\n"; return 1; }
            continue;
        }
        if (arg == "--resolution") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            std::string res = argv[++i];
            size_t xPos = res.find('x');
            if (xPos == std::string::npos) xPos = res.find('X');
            if (xPos == std::string::npos) {
                std::cerr << "Error: --resolution must be WxH (e.g. 1024x512)\n";
                return 1;
            }
            cfg.width = std::atoi(res.substr(0, xPos).c_str());
            cfg.height = std::atoi(res.substr(xPos + 1).c_str());
            if (cfg.width <= 0 || cfg.height <= 0) {
                std::cerr << "Error: --resolution dimensions must be positive\n";
                return 1;
            }
            continue;
        }
        if (arg == "--output-format") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.output_format = argv[++i];
            if (cfg.output_format != "image" && cfg.output_format != "video") {
                std::cerr << "Error: --output-format must be image or video\n";
                return 1;
            }
            continue;
        }
        if (arg == "--fps") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.fps = std::atoi(argv[++i]);
            if (cfg.fps <= 0 || cfg.fps > 120) {
                std::cerr << "Error: --fps must be 1..120\n";
                return 1;
            }
            continue;
        }
        if (arg == "--codec") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.video_codec = argv[++i];
            continue;
        }
        if (arg == "--crf") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.crf = std::atoi(argv[++i]);
            if (cfg.crf < 0 || cfg.crf > 51) {
                std::cerr << "Error: --crf must be 0..51\n";
                return 1;
            }
            continue;
        }
        if (arg == "--duration") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.window_seconds = static_cast<float>(std::atof(argv[++i]));
            if (cfg.window_seconds <= 0.0f) {
                std::cerr << "Error: --duration must be > 0\n";
                return 1;
            }
            continue;
        }
        if (arg == "--freq-scale") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.freq_scale = argv[++i];
            if (cfg.freq_scale != "linear" && cfg.freq_scale != "log" &&
                cfg.freq_scale != "mel" && cfg.freq_scale != "bark" &&
                cfg.freq_scale != "erb" && cfg.freq_scale != "cqt") {
                std::cerr << "Error: --freq-scale must be linear, log, mel, bark, erb, or cqt\n";
                return 1;
            }
            continue;
        }
        if (arg == "--cqt-center") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.cqt_center = static_cast<float>(std::atof(argv[++i]));
            if (cfg.cqt_center <= 0.0f) { std::cerr << "Error: --cqt-center must be > 0\n"; return 1; }
            continue;
        }
        if (arg == "--cqt-q") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.cqt_q = static_cast<float>(std::atof(argv[++i]));
            if (cfg.cqt_q <= 0.0f) { std::cerr << "Error: --cqt-q must be > 0\n"; return 1; }
            continue;
        }
        if (arg == "--reassigned") {
            cfg.reassigned = true;
            continue;
        }
        if (arg == "--gpu") {
            cfg.use_gpu = true;
            continue;
        }
        if (arg == "--multiband") {
            cfg.multiband = true;
            continue;
        }
        if (arg == "--recursive") {
            cfg.recursive = true;
            continue;
        }
        if (arg == "--jobs") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.jobs = std::atoi(argv[++i]);
            if (cfg.jobs < 0) { std::cerr << "Error: --jobs must be >= 0 (0 = auto)\n"; return 1; }
            continue;
        }
        if (arg == "--retries") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.retries = std::atoi(argv[++i]);
            if (cfg.retries < 0) { std::cerr << "Error: --retries must be >= 0\n"; return 1; }
            continue;
        }
        if (arg == "--ext") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.exts = argv[++i];
            continue;
        }
        if (arg == "batch" && cfg.input_path.empty() && i == 1) {
            cfg.batch_mode = true;
            continue;
        }

        // Positional: input file
        if (cfg.input_path.empty() && arg[0] != '-') {
            cfg.input_path = arg;
            continue;
        }

        std::cerr << "Error: unknown argument '" << arg << "'\n";
        return 1;
    }

    // Validate required args
    if (cfg.input_path.empty()) {
        std::cerr << "Error: no input file specified\n";
        return 1;
    }
    if (cfg.output_path.empty()) {
        std::cerr << "Error: no output file specified (use --output)\n";
        return 1;
    }

    // Auto-detect output format from extension if not explicitly set
    // ponytail: batch outputs are directories (no extension) — guard npos
    if (cfg.output_format == "image" && !cfg.output_path.empty()) {
        auto dot = cfg.output_path.find_last_of('.');
        if (dot != std::string::npos) {
            auto ext = cfg.output_path.substr(dot);
            if (ext == ".mp4" || ext == ".webm" || ext == ".mkv") {
                cfg.output_format = "video";
            }
        }
    }

    // Defaults
    if (cfg.hop_size == 0) {
        cfg.hop_size = cfg.fft_size / 2;
    }

    return 0;
}

// ============================================================================
// Main
// ============================================================================
static int to_exit_code(const Spectral::Error& e) {
    switch (e.code) {
        case Spectral::JobError::Ok:            return ExitCode::OK;
        case Spectral::JobError::FileNotFound:  return ExitCode::FileNotFound;
        case Spectral::JobError::DecodeError:   return ExitCode::DecodeError;
        case Spectral::JobError::AnalysisError: return ExitCode::AnalysisError;
        case Spectral::JobError::RenderError:   return ExitCode::RenderError;
        case Spectral::JobError::BadConfig:     return ExitCode::BadArgs;
    }
    return ExitCode::BadArgs;
}

int main(int argc, char* argv[]) {
    CliConfig cfg;
    int rc = parse_args(argc, argv, cfg);
    if (rc != 0) return ExitCode::BadArgs;

    if (cfg.show_help) { print_usage(); return ExitCode::OK; }
    if (cfg.show_version) { std::cout << VERSION << "\n"; return ExitCode::OK; }

    auto progress = [](float f, const char* stage) {
        std::cerr << "\r[" << stage << "] " << static_cast<int>(f * 100) << "%" << std::flush;
    };

    // Batch mode: folders / multiple files, bounded parallelism, per-file results.
    if (cfg.batch_mode) {
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
        Spectral::BatchOptions bopts;
        bopts.recursive = cfg.recursive;
        bopts.jobs = cfg.jobs;
        bopts.retries = cfg.retries;
        if (!cfg.exts.empty()) {
            std::string cur;
            for (char c : cfg.exts + ",") {
                if (c == ',') {
                    if (!cur.empty()) bopts.extensions.push_back(cur);
                    cur.clear();
                } else if (c != ' ') {
                    cur += c;
                }
            }
        }
        auto files = Spectral::collect_inputs(cfg.input_path, bopts);
        if (files.empty()) {
            std::cerr << "Error: no media files found in '" << cfg.input_path << "'\n";
            return ExitCode::FileNotFound;
        }
        std::cerr << "Batch: " << files.size() << " file(s), "
                  << (bopts.jobs <= 0 ? "auto" : std::to_string(bopts.jobs))
                  << " jobs, " << bopts.retries << " retries\n";
        auto results = Spectral::run_batch(
            cfg, files, cfg.input_path, cfg.output_path, bopts, g_cancel,
            [](int done, int total, const char* file) {
                std::cerr << "\r[" << done << "/" << total << "] " << file << "   " << std::flush;
            });
        std::cerr << "\n";
        int ok = 0, fail = 0;
        for (const auto& r : results) {
            if (r.ok) {
                ++ok;
                std::cout << "OK   " << r.input << " -> " << r.output
                          << " (" << static_cast<int>(r.elapsed_ms) << "ms"
                          << ", attempts=" << r.attempts << ")\n";
            } else {
                ++fail;
                std::cout << "FAIL " << r.input << " : " << r.error
                          << " (attempts=" << r.attempts << ")\n";
            }
        }
        std::cout << "Batch done: " << ok << " ok, " << fail << " failed\n";
        return fail ? ExitCode::RenderError : ExitCode::OK;
    }

    // Multi-band comparison needs mid-pipeline access: analyze, print table, render.
    if (cfg.multiband) {
        Spectral::SpectralDataset dataset;
        std::vector<float> samples;
        Spectral::Error err = Spectral::analyze_dataset(cfg, dataset, samples, progress);
        std::cerr << "\n";
        if (!err.ok()) {
            std::cerr << "Error: " << err.message << "\n";
            return to_exit_code(err);
        }

        std::cerr << "Analyzed " << dataset.frame_count() << " frames, "
                  << cfg.fft_size << "-point FFT, "
                  << dataset.sample_rate() << " Hz\n";

        int sr = dataset.sample_rate();
        float sr_f = static_cast<float>(sr);

        auto fixed = Spectral::MultiBandAnalyzer::analyze_single(
            samples, sr, cfg.fft_size, cfg.hop_size);
        auto short_w = Spectral::MultiBandAnalyzer::analyze_single(
            samples, sr, 256, 64);
        auto long_w = Spectral::MultiBandAnalyzer::analyze_single(
            samples, sr, 4096, 1024);
        auto multi = Spectral::MultiBandAnalyzer::analyze(
            samples, sr, cfg.fft_size, cfg.hop_size);

        std::printf("\n%-16s %-15s %-15s %-14s\n",
                    "Method", "Time Res (s)", "Freq Res (Hz)", "Compute (ms)");
        std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                    "Fixed",
                    static_cast<float>(fixed.dataset.hop_size()) / sr_f,
                    fixed.dataset.frequency_resolution(), fixed.compute_ms);
        std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                    "Short (256)",
                    static_cast<float>(short_w.dataset.hop_size()) / sr_f,
                    short_w.dataset.frequency_resolution(), short_w.compute_ms);
        std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                    "Long (4096)",
                    static_cast<float>(long_w.dataset.hop_size()) / sr_f,
                    long_w.dataset.frequency_resolution(), long_w.compute_ms);
        std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                    "Multi-band",
                    static_cast<float>(multi.dataset.hop_size()) / sr_f,
                    multi.dataset.frequency_resolution(), multi.compute_ms);
        std::printf("\n");

        Spectral::Error rerr = Spectral::render_dataset(cfg, dataset);
        rc = to_exit_code(rerr);
        if (rc != ExitCode::OK) {
            std::cerr << "Error: " << rerr.message << "\n";
            return rc;
        }
        std::cerr << "Wrote " << cfg.output_path << "\n";
        return ExitCode::OK;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    Spectral::Error err = Spectral::run_job(cfg, progress, &g_cancel);
    std::cerr << "\n";
    if (!err.ok()) {
        std::cerr << "Error: " << err.message << "\n";
        return to_exit_code(err);
    }

    std::cerr << "Wrote " << cfg.output_path << "\n";
    return ExitCode::OK;
}
