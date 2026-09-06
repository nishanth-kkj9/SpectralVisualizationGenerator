// Phase 9 — spectragen CLI integration tests.
// Generates synthetic WAV, runs spectragen as subprocess, checks exit codes.

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

// ============================================================================
// WAV writer (PCM 16-bit mono, no external deps)
// ============================================================================
static bool write_wav(const std::string& path, int sample_rate, int num_samples,
                      const float* samples) {
    std::ofstream f(path, std::ios::out | std::ios::binary);
    if (!f) return false;

    int16_t bits_per_sample = 16;
    int16_t num_channels = 1;
    int16_t block_align = num_channels * bits_per_sample / 8;
    int byte_rate = sample_rate * block_align;
    int data_size = num_samples * block_align;
    int chunk_size = 36 + data_size;

    // RIFF header
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&chunk_size), 4);
    f.write("WAVE", 4);

    // fmt chunk
    f.write("fmt ", 4);
    int fmt_size = 16;
    f.write(reinterpret_cast<const char*>(&fmt_size), 4);
    int16_t fmt_tag = 1; // PCM
    f.write(reinterpret_cast<const char*>(&fmt_tag), 2);
    f.write(reinterpret_cast<const char*>(&num_channels), 2);
    f.write(reinterpret_cast<const char*>(&sample_rate), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

    // data chunk
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_size), 4);

    // Write samples as int16
    std::vector<int16_t> buf(static_cast<size_t>(num_samples));
    for (int i = 0; i < num_samples; ++i) {
        float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        buf[static_cast<size_t>(i)] = static_cast<int16_t>(clamped * 32767.0f);
    }
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(data_size));
    return f.good();
}

// ============================================================================
// Generate sine wave
// ============================================================================
static std::vector<float> gen_sine(int sample_rate, double freq_hz, double duration_sec) {
    int n = static_cast<int>(static_cast<double>(sample_rate) * duration_sec);
    std::vector<float> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(sample_rate);
        samples[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * freq_hz * t));
    }
    return samples;
}

// ============================================================================
// Run command, return exit code
// ============================================================================
static int run_cmd(const std::string& cmd) {
    // Use CreateProcess to avoid Windows cmd.exe quote-stripping
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::string mutable_cmd = cmd;

    // Redirect stderr to NUL
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE h_null = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = h_null;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    BOOL ok = CreateProcessA(
        NULL,
        mutable_cmd.data(),
        NULL, NULL, TRUE, 0, NULL, NULL,
        &si, &pi);
    CloseHandle(h_null);

    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
}

// ============================================================================
// Tests
// ============================================================================
static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

// Find project root (up from build/ or tests/)
static std::string find_project_root() {
    // Try relative paths from typical build locations
    const char* candidates[] = {
        ".",                           // if run from project root
        "..",                          // if run from build/
        "../..",                       // if run from build/Debug/
    };
    for (auto c : candidates) {
        if (fs::exists(fs::path(c) / "src" / "cli" / "main.cpp")) {
            return fs::absolute(c).string();
        }
    }
    return ".";
}

static void test_help(const std::string& exe) {
    std::fprintf(stderr, "[test_help]\n");
    std::string cmd = "\"" + exe + "\" --help";
    int rc = run_cmd(cmd);
    CHECK(rc == 0, "--help exit 0");

    // Check stdout contains "Usage"
    std::string cmd_out = "\"" + exe + "\" --help";
    FILE* pipe = _popen(cmd_out.c_str(), "r");
    if (pipe) {
        char buf[256];
        std::string output;
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        _pclose(pipe);
        CHECK(output.find("Usage") != std::string::npos, "--help contains Usage");
    }
}

static void test_version(const std::string& exe) {
    std::fprintf(stderr, "[test_version]\n");
    std::string cmd = "\"" + exe + "\" --version";
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[256];
        std::string output;
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        _pclose(pipe);
        CHECK(output.find("0.1.0") != std::string::npos, "--version contains 0.1.0");
    }
    int rc = run_cmd(cmd);
    CHECK(rc == 0, "--version exit 0");
}

static void test_no_args(const std::string& exe) {
    std::fprintf(stderr, "[test_no_args]\n");
    int rc = run_cmd("\"" + exe + "\"");
    CHECK(rc == 1, "no args exit 1");
}

static void test_bad_flag(const std::string& exe) {
    std::fprintf(stderr, "[test_bad_flag]\n");
    int rc = run_cmd("\"" + exe + "\" --bogus");
    CHECK(rc == 1, "bad flag exit 1");
}

static void test_nonexistent_file(const std::string& exe) {
    std::fprintf(stderr, "[test_nonexistent_file]\n");
    int rc = run_cmd("\"" + exe + "\" nonexistent.wav --output test_out.png");
    CHECK(rc == 2, "nonexistent file exit 2");
}

static void test_full_pipeline_spectrogram(const std::string& exe, const std::string& root) {
    std::fprintf(stderr, "[test_full_pipeline_spectrogram]\n");

    // Generate WAV
    std::string wav_path = root + "/tests/phase9/test_sine.wav";
    auto samples = gen_sine(44100, 440.0, 0.5);  // 440 Hz, 0.5 sec
    CHECK(write_wav(wav_path, 44100, static_cast<int>(samples.size()), samples.data()),
          "write WAV");

    // Run spectragen
    std::string out_png = root + "/tests/phase9/test_spectrogram.png";
    std::string cmd = "\"" + exe + "\" \"" + wav_path + "\" --output \"" + out_png + "\"";
    int rc = run_cmd(cmd);
    CHECK(rc == 0, "spectrogram pipeline exit 0");

    // Check output exists and has content
    CHECK(fs::exists(out_png), "output PNG exists");
    if (fs::exists(out_png)) {
        CHECK(fs::file_size(out_png) > 100, "output PNG has content");
    }
}

static void test_full_pipeline_spectrum(const std::string& exe, const std::string& root) {
    std::fprintf(stderr, "[test_full_pipeline_spectrum]\n");

    std::string wav_path = root + "/tests/phase9/test_sine.wav";
    std::string out_png = root + "/tests/phase9/test_spectrum.png";
    std::string cmd = "\"" + exe + "\" \"" + wav_path + "\" --output \"" + out_png + "\" -v spectrum";
    int rc = run_cmd(cmd);
    CHECK(rc == 0, "spectrum pipeline exit 0");

    CHECK(fs::exists(out_png), "spectrum PNG exists");
    if (fs::exists(out_png)) {
        CHECK(fs::file_size(out_png) > 100, "spectrum PNG has content");
    }
}

static void test_fft_flag(const std::string& exe, const std::string& root) {
    std::fprintf(stderr, "[test_fft_flag]\n");

    std::string wav_path = root + "/tests/phase9/test_sine.wav";
    std::string out_png = root + "/tests/phase9/test_fft2048.png";
    std::string cmd = "\"" + exe + "\" \"" + wav_path + "\" --output \"" + out_png + "\" --fft 2048";
    int rc = run_cmd(cmd);
    CHECK(rc == 0, "--fft 2048 exit 0");
}

static void test_bad_fft_size(const std::string& exe) {
    std::fprintf(stderr, "[test_bad_fft_size]\n");
    int rc = run_cmd("\"" + exe + "\" dummy.wav --output out.png --fft 100");
    CHECK(rc == 1, "bad fft size exit 1");
}

static void test_bad_window(const std::string& exe) {
    std::fprintf(stderr, "[test_bad_window]\n");
    int rc = run_cmd("\"" + exe + "\" dummy.wav --output out.png --window bogus");
    CHECK(rc == 1, "bad window exit 1");
}

static void test_bad_resolution(const std::string& exe) {
    std::fprintf(stderr, "[test_bad_resolution]\n");
    int rc = run_cmd("\"" + exe + "\" dummy.wav --output out.png --resolution 100");
    CHECK(rc == 1, "bad resolution exit 1");
}

static void test_cleanup(const std::string& root) {
    std::fprintf(stderr, "[cleanup]\n");
    const char* files[] = {
        "tests/phase9/test_sine.wav",
        "tests/phase9/test_spectrogram.png",
        "tests/phase9/test_spectrum.png",
        "tests/phase9/test_fft2048.png",
    };
    for (auto f : files) {
        fs::path p = fs::path(root) / f;
        if (fs::exists(p)) fs::remove(p);
    }
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: test_spectragen_cli <spectragen-path>\n");
        return 1;
    }

    std::string exe = argv[1];
    std::string root = find_project_root();

    std::fprintf(stderr, "spectragen: %s\n", exe.c_str());
    std::fprintf(stderr, "project root: %s\n", root.c_str());

    // Ensure test directory exists
    fs::create_directories(fs::path(root) / "tests" / "phase9");

    test_help(exe);
    test_version(exe);
    test_no_args(exe);
    test_bad_flag(exe);
    test_nonexistent_file(exe);
    test_bad_fft_size(exe);
    test_bad_window(exe);
    test_bad_resolution(exe);

    // Full pipeline tests (require ffmpeg)
    bool has_ffmpeg = (run_cmd("where ffmpeg") == 0);
    if (has_ffmpeg) {
        test_full_pipeline_spectrogram(exe, root);
        test_full_pipeline_spectrum(exe, root);
        test_fft_flag(exe, root);
    } else {
        std::fprintf(stderr, "  [SKIP] pipeline tests (ffmpeg not in PATH)\n");
    }

    test_cleanup(root);

    std::fprintf(stderr, "\n=== spectragen_cli: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
