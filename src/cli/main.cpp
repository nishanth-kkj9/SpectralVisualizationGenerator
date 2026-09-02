// spectragen — Production CLI for SpectralVisualizationGenerator.
// Uses MediaDecoder, fft.h, SpectralDataset, and renderers directly.
// No GUI, no GPU, no duplicated DSP logic beyond STFT loop.

#include "fft.h"
#include "media_decoder.h"
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
#include "png_encoder.h"
#include "spectral_dataset.h"
#include "multiband_analyzer.h"
#include "video_renderer.h"

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
// CLI configuration
// ============================================================================
struct CliConfig {
    std::string input_path;
    std::string output_path;
    std::string visualization = "spectrogram";
    int fft_size = 1024;
    int hop_size = 0;       // 0 = fft_size/2
    std::string window = "hann";
    float overlap = 0.5f;
    float min_freq = 0.0f;
    float max_freq = 0.0f;  // 0 = auto (nyquist)
    float db_range = 80.0f;
    int width = 1024;
    int height = 512;
    bool show_help = false;
    bool show_version = false;
    // Video output
    std::string output_format = "image";  // "image" (PNG) or "video" (MP4/WebM)
    int fps = 30;
    std::string video_codec = "libx264";
    int crf = 18;
    float window_seconds = 5.0f;
    // Frequency scale
    std::string freq_scale = "log";
    float cqt_center = 440.0f;
    float cqt_q = 12.0f;
    // Reassignment
    bool reassigned = false;
    // Multi-band comparison
    bool multiband = false;
    std::vector<float> all_samples;  // decoded audio for multiband analysis
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
        if (arg == "--multiband") {
            cfg.multiband = true;
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
    if (cfg.output_format == "image" && !cfg.output_path.empty()) {
        auto ext = cfg.output_path.substr(cfg.output_path.find_last_of('.'));
        if (ext == ".mp4" || ext == ".webm" || ext == ".mkv") {
            cfg.output_format = "video";
        }
    }

    // Defaults
    if (cfg.hop_size == 0) {
        cfg.hop_size = cfg.fft_size / 2;
    }

    return 0;
}

static Spectral::FrequencyScale parse_freq_scale(const std::string& s) {
    if (s == "mel")    return Spectral::FrequencyScale::Mel;
    if (s == "bark")   return Spectral::FrequencyScale::Bark;
    if (s == "erb")    return Spectral::FrequencyScale::Erb;
    if (s == "cqt")    return Spectral::FrequencyScale::CQT;
    if (s == "linear") return Spectral::FrequencyScale::Linear;
    return Spectral::FrequencyScale::Logarithmic;
}

// ============================================================================
// Window function selection
// ============================================================================
static std::vector<float> make_window(const std::string& type, int n) {
    if (type == "hamming")      return window_hamming(n);
    if (type == "blackman")     return window_blackman(n);
    if (type == "rectangular")  return window_rectangular(n);
    return window_hann(n);  // default
}

// ============================================================================
// STFT analysis → SpectralDataset
// ============================================================================
static int run_analysis(CliConfig& cfg, Spectral::SpectralDataset& dataset) {
    // Open audio
    MediaDecoder decoder;
    if (!decoder.open(cfg.input_path)) {
        std::cerr << "Error: cannot open '" << cfg.input_path << "'\n";
        return ExitCode::FileNotFound;
    }

    int sr = decoder.sample_rate();
    if (sr <= 0) {
        std::cerr << "Error: invalid sample rate from decoder\n";
        return ExitCode::DecodeError;
    }

    // Configure axes
    int num_bins = cfg.fft_size / 2 + 1;
    dataset.mutable_frequency_axis() = Spectral::FrequencyAxis(cfg.fft_size, sr);

    // Window function
    std::vector<float> win = make_window(cfg.window, cfg.fft_size);
    float cg = window_coherent_gain(win);

    // Read all audio into buffer (mono mix)
    std::vector<float> audio;
    AudioFrame frame;
    while (decoder.read_frame(frame)) {
        if (frame.num_channels == 1) {
            audio.insert(audio.end(), frame.samples.begin(), frame.samples.end());
        } else {
            // Mix to mono
            size_t frame_samples = frame.samples.size() / static_cast<size_t>(frame.num_channels);
            audio.reserve(audio.size() + frame_samples);
            for (size_t i = 0; i < frame_samples; ++i) {
                float sum = 0.0f;
                for (int ch = 0; ch < frame.num_channels; ++ch) {
                    sum += frame.samples[i * static_cast<size_t>(frame.num_channels) + ch];
                }
                audio.push_back(sum / static_cast<float>(frame.num_channels));
            }
        }
    }
    decoder.close();

    if (audio.empty()) {
        std::cerr << "Error: no audio data decoded\n";
        return ExitCode::DecodeError;
    }

    cfg.all_samples = audio;

    // STFT loop
    int total_samples = static_cast<int>(audio.size());
    int frame_idx = 0;
    for (int start = 0; start + cfg.fft_size <= total_samples; start += cfg.hop_size) {
        // Apply window and copy to complex buffer
        std::vector<complex_f> buf(static_cast<size_t>(cfg.fft_size));
        for (int j = 0; j < cfg.fft_size; ++j) {
            buf[static_cast<size_t>(j)] = complex_f(audio[static_cast<size_t>(start + j)] * win[static_cast<size_t>(j)], 0.0f);
        }

        // Forward FFT
        fft(buf);

        // Extract magnitudes (positive frequencies only)
        Spectral::SpectralFrame sf;
        sf.frame_index = frame_idx;
        sf.n_fft = cfg.fft_size;
        sf.window_factor = cg;
        sf.timestamp = static_cast<double>(start) / static_cast<double>(sr);

        sf.magnitudes.resize(static_cast<size_t>(num_bins));
        sf.phases.resize(static_cast<size_t>(num_bins));
        sf.power.resize(static_cast<size_t>(num_bins));

        float scale = 1.0f / static_cast<float>(cfg.fft_size);
        for (int k = 0; k < num_bins; ++k) {
            float re = buf[static_cast<size_t>(k)].real();
            float im = buf[static_cast<size_t>(k)].imag();
            float mag = std::sqrt(re * re + im * im) * scale;
            sf.magnitudes[static_cast<size_t>(k)] = mag;
            sf.phases[static_cast<size_t>(k)] = std::atan2(im, re);
            sf.power[static_cast<size_t>(k)] = mag * mag;
        }

        // Per-frame stats
        float sum_sq = 0.0f;
        float peak = 0.0f;
        float centroid_num = 0.0f;
        float centroid_den = 0.0f;
        for (int k = 0; k < num_bins; ++k) {
            float m = sf.magnitudes[static_cast<size_t>(k)];
            sum_sq += m * m;
            if (m > peak) peak = m;
            float freq = static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(cfg.fft_size);
            centroid_num += freq * m;
            centroid_den += m;
        }
        sf.rms = std::sqrt(sum_sq / static_cast<float>(num_bins));
        sf.peak_magnitude = peak;
        sf.spectral_centroid = (centroid_den > 0.0f) ? centroid_num / centroid_den : 0.0f;

        // Reassignment: compute instantaneous frequency and group delay
        if (cfg.reassigned) {
            // Group delay STFT: FFT{n * w[n] * x[n]} (time reassignment kernel)
            std::vector<complex_f> X_tau(static_cast<size_t>(cfg.fft_size));
            for (int j = 0; j < cfg.fft_size; ++j) {
                X_tau[static_cast<size_t>(j)] = complex_f(
                    static_cast<float>(j) * win[static_cast<size_t>(j)] * audio[static_cast<size_t>(start + j)], 0.0f);
            }
            fft(X_tau);

            // Window derivative STFT: FFT{w'[n] * x[n]} (frequency reassignment kernel)
            std::vector<float> w_deriv(static_cast<size_t>(cfg.fft_size));
            for (int j = 0; j < cfg.fft_size; ++j) {
                w_deriv[static_cast<size_t>(j)] = static_cast<float>(PI) / static_cast<float>(cfg.fft_size - 1) *
                    std::sin(2.0f * PI * static_cast<float>(j) / static_cast<float>(cfg.fft_size - 1));
            }
            std::vector<complex_f> X_dg(static_cast<size_t>(cfg.fft_size));
            for (int j = 0; j < cfg.fft_size; ++j) {
                X_dg[static_cast<size_t>(j)] = complex_f(
                    w_deriv[static_cast<size_t>(j)] * audio[static_cast<size_t>(start + j)], 0.0f);
            }
            fft(X_dg);

            sf.reassigned_times.resize(static_cast<size_t>(num_bins));
            sf.reassigned_freqs.resize(static_cast<size_t>(num_bins));

            for (int k = 0; k < num_bins; ++k) {
                float re = buf[static_cast<size_t>(k)].real();
                float im = buf[static_cast<size_t>(k)].imag();
                float mag_sq = re * re + im * im;
                if (mag_sq > 1e-12f) {
                    complex_f conj_X(re, -im);
                    // Instantaneous frequency: base freq + correction from window derivative
                    float corr_freq = (conj_X * X_dg[static_cast<size_t>(k)]).imag();
                    sf.reassigned_freqs[static_cast<size_t>(k)] =
                        static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(cfg.fft_size) +
                        corr_freq / (2.0f * PI * mag_sq) * static_cast<float>(sr);
                    // Group delay: center of mass in time
                    float dot_tau = (conj_X * X_tau[static_cast<size_t>(k)]).real();
                    sf.reassigned_times[static_cast<size_t>(k)] =
                        dot_tau / mag_sq / static_cast<float>(sr);
                } else {
                    sf.reassigned_freqs[static_cast<size_t>(k)] = 0.0f;
                    sf.reassigned_times[static_cast<size_t>(k)] = 0.0f;
                }
            }
        }

        dataset.add_frame(sf);
        ++frame_idx;
    }

    if (dataset.frame_count() == 0) {
        std::cerr << "Error: no frames produced (audio too short for FFT size " << cfg.fft_size << ")\n";
        return ExitCode::AnalysisError;
    }

    // Set time axis
    dataset.mutable_time_axis() = Spectral::TimeAxis(dataset.frame_count(), cfg.hop_size, sr);

    // Set metadata
    auto& src = dataset.mutable_source_metadata();
    src.file_path = cfg.input_path;

    auto& am = dataset.mutable_analysis_metadata();
    am.fft_size = cfg.fft_size;
    am.hop_size = cfg.hop_size;
    am.sample_rate = sr;

    return ExitCode::OK;
}

// ============================================================================
// Render and write PNG
// ============================================================================
static int run_render(const CliConfig& cfg, const Spectral::SpectralDataset& dataset) {
    if (cfg.visualization == "spectrogram") {
        Spectral::SpectrogramConfig sc;
        sc.width = cfg.width;
        sc.height = cfg.height;
        sc.freq_scale = parse_freq_scale(cfg.freq_scale);
        sc.freq_min_hz = (cfg.min_freq > 0) ? cfg.min_freq : 20.0f;
        sc.freq_max_hz = cfg.max_freq;
        sc.cqt_center_hz = cfg.cqt_center;
        sc.cqt_q = cfg.cqt_q;
        sc.db_ceiling = 0.0f;
        sc.db_floor = -cfg.db_range;

        Spectral::SpectrogramRenderer renderer(sc);
        auto err = renderer.render_to_png(dataset, cfg.output_path);
        if (err != Spectral::RenderError::Ok) {
            std::cerr << "Error: render failed (code " << static_cast<int>(err) << ")\n";
            return ExitCode::RenderError;
        }
    } else {
        Spectral::SpectrumConfig sc;
        sc.width = cfg.width;
        sc.height = cfg.height;
        sc.freq_scale = parse_freq_scale(cfg.freq_scale);
        sc.freq_min_hz = (cfg.min_freq > 0) ? cfg.min_freq : 20.0f;
        sc.freq_max_hz = cfg.max_freq;
        sc.cqt_center_hz = cfg.cqt_center;
        sc.cqt_q = cfg.cqt_q;
        sc.db_ceiling = 0.0f;
        sc.db_floor = -cfg.db_range;

        Spectral::SpectrumRenderer renderer(sc);
        auto err = renderer.render_to_png(dataset, cfg.output_path);
        if (err != Spectral::SpectrumError::Ok) {
            std::cerr << "Error: render failed (code " << static_cast<int>(err) << ")\n";
            return ExitCode::RenderError;
        }
    }

    return ExitCode::OK;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    CliConfig cfg;
    int rc = parse_args(argc, argv, cfg);
    if (rc != 0) return ExitCode::BadArgs;

    if (cfg.show_help) { print_usage(); return ExitCode::OK; }
    if (cfg.show_version) { std::cout << VERSION << "\n"; return ExitCode::OK; }

    // Analyze
    Spectral::SpectralDataset dataset;
    rc = run_analysis(cfg, dataset);
    if (rc != ExitCode::OK) return rc;

    std::cerr << "Analyzed " << dataset.frame_count() << " frames, "
              << cfg.fft_size << "-point FFT, "
              << dataset.sample_rate() << " Hz\n";

    // Multi-band comparison
    if (cfg.multiband) {
        int sr = dataset.sample_rate();
        float sr_f = static_cast<float>(sr);

        auto fixed = Spectral::MultiBandAnalyzer::analyze_single(
            cfg.all_samples, sr, cfg.fft_size, cfg.hop_size);
        auto short_w = Spectral::MultiBandAnalyzer::analyze_single(
            cfg.all_samples, sr, 256, 64);
        auto long_w = Spectral::MultiBandAnalyzer::analyze_single(
            cfg.all_samples, sr, 4096, 1024);
        auto multi = Spectral::MultiBandAnalyzer::analyze(
            cfg.all_samples, sr, cfg.fft_size, cfg.hop_size);

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
    }

    // Video output
    if (cfg.output_format == "video") {
        Spectral::VideoRendererConfig vrcfg;
        vrcfg.width = cfg.width;
        vrcfg.height = cfg.height;
        vrcfg.fps = cfg.fps;
        vrcfg.codec = cfg.video_codec;
        vrcfg.crf = cfg.crf;
        vrcfg.freq_scale = parse_freq_scale(cfg.freq_scale);
        vrcfg.freq_min_hz = (cfg.min_freq > 0) ? cfg.min_freq : 20.0f;
        vrcfg.freq_max_hz = cfg.max_freq;
        vrcfg.db_floor = -cfg.db_range;
        vrcfg.db_ceiling = 0.0f;
        vrcfg.window_seconds = cfg.window_seconds;

        Spectral::VideoRenderer vrend(vrcfg);
        auto verr = vrend.render(dataset, cfg.output_path);
        if (verr != Spectral::VideoRenderError::Ok) {
            std::cerr << "Error: video render failed (code " << static_cast<int>(verr) << ")\n";
            return ExitCode::RenderError;
        }
        std::cerr << "Wrote " << cfg.output_path << " (" << cfg.fps << " fps, "
                  << cfg.video_codec << " crf=" << cfg.crf << ")\n";
        return ExitCode::OK;
    }

    // Render
    rc = run_render(cfg, dataset);
    if (rc != ExitCode::OK) return rc;

    std::cerr << "Wrote " << cfg.output_path << "\n";
    return ExitCode::OK;
}
