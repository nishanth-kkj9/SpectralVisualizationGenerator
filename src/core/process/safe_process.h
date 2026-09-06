#pragma once

// S3 — Safe child-process abstraction (Windows).
// No shell is ever involved: argv elements are quoted per
// CommandLineToArgvW rules and passed directly to CreateProcessW,
// so hostile paths/args stay literal and cannot become commands.
// Binary-safe pipes (no text-mode translation) for PCM I/O.

#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

// Explicit tool override (highest precedence). Empty = not set.
void set_ffmpeg_path(const std::string& path);
void set_ffprobe_path(const std::string& path);

// Resolve an executable: explicit override -> env var -> bare name (PATH).
// kind must be "ffmpeg" or "ffprobe" (selects override + env variable).
std::string resolve_tool(const std::string& kind);

// Quote one argv element per MSVCRT CommandLineToArgvW rules.
std::string quote_arg(const std::string& arg);

class SafeProcess {
public:
    SafeProcess() = default;
    ~SafeProcess();

    SafeProcess(const SafeProcess&) = delete;
    SafeProcess& operator=(const SafeProcess&) = delete;

    struct Options {
        bool capture_stdout = false;
        bool capture_stderr = false;  // drained on a thread, no pipe stalls
        bool provide_stdin = false;
    };

    // Spawn exe with its arguments (do NOT include the program name in
    // argv — it is derived from exe). Returns false if the process could
    // not be started (last_error set).
    bool spawn(const std::string& exe, const std::vector<std::string>& argv,
               const Options& opts);

    bool running() const;

    // stdin: false on broken pipe / not provided.
    bool write_stdin(const uint8_t* data, size_t size);
    void close_stdin();

    // stdout: bytes read, 0 = EOF, (size_t)-1 = error.
    size_t read_stdout(uint8_t* out, size_t max_size);

    // stderr drained during run; full text available after wait().
    std::string stderr_text();

    // exit code after wait(); -1 if never started / still running.
    int wait();
    int exit_code() const { return exit_code_; }

    // Force-terminate (used by dtor + cancellation paths).
    void kill();

    std::string last_error() const { return last_error_; }

private:
    void cleanup();
    void drain_stderr();

    void* proc_ = nullptr;    // HANDLE
    void* thread_ = nullptr;  // primary thread HANDLE
    void* child_stdin_ = nullptr;
    void* child_stdout_ = nullptr;
    void* child_stderr_ = nullptr;
    void* err_thread_ = nullptr;
    std::string stderr_buf_;
    void* stderr_mutex_ = nullptr;
    int exit_code_ = -1;
    bool killed_ = false;
    std::string last_error_;
};

} // namespace Spectral
