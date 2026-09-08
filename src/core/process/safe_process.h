#pragma once

// S3 + Phase 1 — Safe child-process abstraction (Windows).
// No shell is ever involved: argv elements are quoted per
// CommandLineToArgvW rules and passed directly to CreateProcessW,
// so hostile paths/args stay literal and cannot become commands.
// Binary-safe pipes (no text-mode translation) for PCM I/O.
//
// Phase 1 lifetime model (read before touching):
// - The stderr drain thread is ALWAYS joined before its pipe handle is
//   closed, before the buffer is reset, and before destruction —
//   regardless of whether the child already exited.
// - kill()/cleanup()/spawn()/dtor are safe in any order and any number
//   of times; wait() is re-entrant and returns the stored exit code.
// - stderr capture survives wait()/cleanup() and is reset only by the
//   next spawn(), so diagnostics are never lost or mixed across runs.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
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

#ifdef _MSC_VER
#pragma warning(push)
// 1-byte tail padding after the grouped flag byte is benign and intentional.
#pragma warning(disable : 4820)
#endif
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
    // not be started (last_error set). Any previous process/state is
    // released first, so spawn() is safe to call repeatedly.
    bool spawn(const std::string& exe, const std::vector<std::string>& argv,
               const Options& opts);

    bool running() const;

    // stdin: false on broken pipe / not provided.
    bool write_stdin(const uint8_t* data, size_t size);
    void close_stdin();

    // stdout: bytes read, 0 = EOF, (size_t)-1 = error.
    size_t read_stdout(uint8_t* out, size_t max_size);

    // Cancellable stdout read for long streams (decode). Polls the pipe in
    // bounded slices so a cancellation request is observed within ~10ms
    // instead of blocking in ReadFile until the child produces more data.
    // Returns bytes read (0 = EOF). When cancel is observed the child keeps
    // running: was_cancelled is set, 0 is returned, and the caller must
    // treat it as an abort (never as EOF) and terminate via kill().
    size_t read_stdout_cancelable(uint8_t* out, size_t max_size,
                                  const std::atomic<bool>* cancel,
                                  bool& was_cancelled);

    // stderr drained during run; full text stays available after wait()
    // and cleanup(), until the next spawn().
    std::string stderr_text();

    // exit code after wait(); stored code when re-called or after cleanup;
    // -1 if never successfully spawned.
    int wait();
    int exit_code() const { return exit_code_; }

    // Force-terminate if running (no-op otherwise), then join the drain
    // thread. Safe to call in any state, any number of times.
    void kill();

    // Release all OS resources (joins the drain thread first). Safe to
    // call in any state, any number of times. Keeps captured stderr text.
    void cleanup();

    std::string last_error() const { return last_error_; }

private:
    // Join the drain thread if active. Never blocks forever: the thread
    // only blocks in ReadFile, which unblocks on child death or pipe
    // close — callers terminate/confirm death before joining a live child.
    void join_err_thread();
    void close_handles();

    void drain_stderr();

    void* proc_ = nullptr;         // HANDLE, null when none owned
    void* child_stdin_ = nullptr;  // HANDLEs, null when not owned/closed
    void* child_stdout_ = nullptr;
    void* child_stderr_ = nullptr;
    std::thread err_thread_;       // value member: join() or detach() enforced
    std::mutex stderr_mutex_;      // value member: always safe to lock
    std::string stderr_buf_;       // reset only by spawn()
    std::string last_error_;
    int exit_code_ = -1;
    // 1-byte tail grouped to avoid padding warnings: do not interleave.
    std::atomic<bool> stop_drain_{false};
    bool has_process_ = false;
    bool killed_ = false;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace Spectral
