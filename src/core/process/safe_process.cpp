#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "safe_process.h"

#include <windows.h>
#include <process.h>  // _beginthreadex for <thread> under WIN32_LEAN_AND_MEAN

#include <cstdlib>
#include <mutex>
#include <thread>

namespace Spectral {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

std::string narrow_last_error(DWORD code) {
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out;
    if (n > 0 && buf) {
        int m = WideCharToMultiByte(CP_UTF8, 0, buf, n, nullptr, 0, nullptr, nullptr);
        out.resize(static_cast<size_t>(m > 0 ? m : 0));
        if (m > 0) WideCharToMultiByte(CP_UTF8, 0, buf, n, out.data(), m, nullptr, nullptr);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    }
    LocalFree(buf);
    return out;
}

struct ToolOverride {
    std::string ffmpeg;
    std::string ffprobe;
    std::mutex mutex;
};

ToolOverride& overrides() {
    static ToolOverride o;
    return o;
}

} // namespace

void set_ffmpeg_path(const std::string& path) {
    std::lock_guard<std::mutex> lk(overrides().mutex);
    overrides().ffmpeg = path;
}

void set_ffprobe_path(const std::string& path) {
    std::lock_guard<std::mutex> lk(overrides().mutex);
    overrides().ffprobe = path;
}

std::string resolve_tool(const std::string& kind) {
    const bool probe = (kind == "ffprobe");
    {
        std::lock_guard<std::mutex> lk(overrides().mutex);
        const std::string& o = probe ? overrides().ffprobe : overrides().ffmpeg;
        if (!o.empty()) return o;
    }
    const char* envname = probe ? "FFPROBE_BINARY" : "FFMPEG_BINARY";
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, envname) == 0 && val) {
        std::string v(val);
        free(val);
        if (!v.empty()) return v;
    }
    return kind;  // PATH lookup by CreateProcess
}

std::string quote_arg(const std::string& arg) {
    if (arg.empty()) return "\"\"";
    bool need = false;
    for (char ch : arg) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '"' || ch == '\'' ||
            ch == '&' || ch == '|' || ch == ';' || ch == '<' || ch == '>' ||
            ch == '^' || ch == '%' || ch == '!' || ch == '$' || ch == '`') {
            need = true;
            break;
        }
    }
    if (!need) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

SafeProcess::~SafeProcess() {
    kill();
    cleanup();
}

bool SafeProcess::spawn(const std::string& exe, const std::vector<std::string>& argv,
                        const Options& opts) {
    kill();
    cleanup();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_stdin_r = nullptr, h_stdin_w = nullptr;
    HANDLE h_stdout_r = nullptr, h_stdout_w = nullptr;
    HANDLE h_stderr_r = nullptr, h_stderr_w = nullptr;

    auto fail = [&](const std::string& what, DWORD code) {
        last_error_ = what + ": " + narrow_last_error(code);
        if (h_stdin_r) CloseHandle(h_stdin_r);
        if (h_stdin_w) CloseHandle(h_stdin_w);
        if (h_stdout_r) CloseHandle(h_stdout_r);
        if (h_stdout_w) CloseHandle(h_stdout_w);
        if (h_stderr_r) CloseHandle(h_stderr_r);
        if (h_stderr_w) CloseHandle(h_stderr_w);
        return false;
    };

    if (opts.provide_stdin) {
        if (!CreatePipe(&h_stdin_r, &h_stdin_w, &sa, 1 << 20)) return fail("stdin pipe", GetLastError());
        SetHandleInformation(h_stdin_w, HANDLE_FLAG_INHERIT, 0);
    }
    if (opts.capture_stdout) {
        if (!CreatePipe(&h_stdout_r, &h_stdout_w, &sa, 1 << 20)) return fail("stdout pipe", GetLastError());
        SetHandleInformation(h_stdout_r, HANDLE_FLAG_INHERIT, 0);
    }
    if (opts.capture_stderr) {
        if (!CreatePipe(&h_stderr_r, &h_stderr_w, &sa, 1 << 16)) return fail("stderr pipe", GetLastError());
        SetHandleInformation(h_stderr_r, HANDLE_FLAG_INHERIT, 0);
    }

    std::string cmdline = quote_arg(exe);
    for (const auto& a : argv) {
        cmdline.push_back(' ');
        cmdline += quote_arg(a);
    }
    std::wstring wcmd = widen(cmdline);
    if (wcmd.empty() && !cmdline.empty()) return fail("utf8->wide", GetLastError());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = opts.provide_stdin ? h_stdin_r : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = opts.capture_stdout ? h_stdout_w : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = opts.capture_stderr ? h_stderr_w : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    // No shell: executable + quoted argv go straight to the child.
    // bInheritHandles=TRUE only shares the explicitly inheritable pipe ends.
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Parent closes child ends immediately.
    if (h_stdin_r) CloseHandle(h_stdin_r);
    if (h_stdout_w) CloseHandle(h_stdout_w);
    if (h_stderr_w) CloseHandle(h_stderr_w);
    if (!ok) {
        DWORD code = GetLastError();
        if (h_stdin_w) CloseHandle(h_stdin_w);
        if (h_stdout_r) CloseHandle(h_stdout_r);
        if (h_stderr_r) CloseHandle(h_stderr_r);
        last_error_ = "cannot start '" + exe + "': " + narrow_last_error(code);
        return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    thread_ = nullptr;
    if (opts.provide_stdin) child_stdin_ = h_stdin_w;
    if (opts.capture_stdout) child_stdout_ = h_stdout_r;
    if (opts.capture_stderr) {
        child_stderr_ = h_stderr_r;
        stderr_mutex_ = new std::mutex();
        // Drain on a thread: ffmpeg blocks if stderr pipe fills up.
        err_thread_ = new std::thread([this] { drain_stderr(); });
    }
    exit_code_ = -1;
    killed_ = false;
    last_error_.clear();
    return true;
}

bool SafeProcess::running() const {
    if (!proc_) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(proc_), &code)) return false;
    return code == STILL_ACTIVE;
}

bool SafeProcess::write_stdin(const uint8_t* data, size_t size) {
    if (!child_stdin_ || !data) return false;
    size_t off = 0;
    while (off < size) {
        DWORD w = 0;
        DWORD chunk = static_cast<DWORD>((size - off) > 1 << 20 ? 1 << 20 : size - off);
        if (!WriteFile(static_cast<HANDLE>(child_stdin_), data + off, chunk, &w, nullptr))
            return false;  // broken pipe: child exited
        if (w == 0) return false;
        off += w;
    }
    return true;
}

void SafeProcess::close_stdin() {
    if (child_stdin_) {
        CloseHandle(static_cast<HANDLE>(child_stdin_));
        child_stdin_ = nullptr;
    }
}

size_t SafeProcess::read_stdout(uint8_t* out, size_t max_size) {
    if (!child_stdout_ || !out || max_size == 0) return static_cast<size_t>(-1);
    DWORD r = 0;
    DWORD ask = max_size > (1 << 20) ? (1 << 20) : static_cast<DWORD>(max_size);
    if (!ReadFile(static_cast<HANDLE>(child_stdout_), out, ask, &r, nullptr)) {
        DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE) return 0;  // EOF
        return static_cast<size_t>(-1);
    }
    return static_cast<size_t>(r);  // 0 = EOF
}

void SafeProcess::drain_stderr() {
    char buf[4096];
    for (;;) {
        DWORD r = 0;
        if (!ReadFile(static_cast<HANDLE>(child_stderr_), buf, sizeof(buf), &r, nullptr) || r == 0)
            break;
        std::lock_guard<std::mutex> lk(*static_cast<std::mutex*>(stderr_mutex_));
        stderr_buf_.append(buf, r);
    }
}

std::string SafeProcess::stderr_text() {
    if (!stderr_mutex_) return {};
    std::lock_guard<std::mutex> lk(*static_cast<std::mutex*>(stderr_mutex_));
    return stderr_buf_;
}

int SafeProcess::wait() {
    if (!proc_) return -1;
    WaitForSingleObject(static_cast<HANDLE>(proc_), INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(proc_), &code)) {
        last_error_ = "cannot read exit code";
        return -1;
    }
    exit_code_ = static_cast<int>(code);
    // Child closed stderr on exit; join the drain thread to collect the tail.
    if (err_thread_) {
        // Closing our stdout handle first unblocks any edge case where the
        // child inherited duplicates; stderr EOF arrives from child exit.
        static_cast<std::thread*>(err_thread_)->join();
        delete static_cast<std::thread*>(err_thread_);
        err_thread_ = nullptr;
    }
    return exit_code_;
}

void SafeProcess::kill() {
    if (proc_ && running()) {
        TerminateProcess(static_cast<HANDLE>(proc_), 1);
        killed_ = true;
        WaitForSingleObject(static_cast<HANDLE>(proc_), 5000);
        exit_code_ = 1;
        if (err_thread_) {
            // stderr EOF follows termination; join briefly then detach if stuck.
            static_cast<std::thread*>(err_thread_)->join();
            delete static_cast<std::thread*>(err_thread_);
            err_thread_ = nullptr;
        }
    }
}

void SafeProcess::cleanup() {
    if (child_stdin_) {
        CloseHandle(static_cast<HANDLE>(child_stdin_));
        child_stdin_ = nullptr;
    }
    if (child_stdout_) {
        CloseHandle(static_cast<HANDLE>(child_stdout_));
        child_stdout_ = nullptr;
    }
    if (child_stderr_) {
        CloseHandle(static_cast<HANDLE>(child_stderr_));
        child_stderr_ = nullptr;
    }
    if (stderr_mutex_) {
        delete static_cast<std::mutex*>(stderr_mutex_);
        stderr_mutex_ = nullptr;
    }
    if (thread_) {
        CloseHandle(static_cast<HANDLE>(thread_));
        thread_ = nullptr;
    }
    if (proc_) {
        CloseHandle(static_cast<HANDLE>(proc_));
        proc_ = nullptr;
    }
}

} // namespace Spectral
