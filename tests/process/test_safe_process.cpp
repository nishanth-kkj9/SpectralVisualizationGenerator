// tests/process/test_safe_process.cpp
// Phase 1 lifecycle tests: deterministic child = this binary re-executed
// with --child-* flags (no shell, no external tools, no timing flakes;
// waits poll for events with generous bounds, never assume timing).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "process/safe_process.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

static int g_run = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_run;                                                             \
        if (cond) {                                                          \
            ++g_pass;                                                        \
        } else {                                                             \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);            \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Child modes (argv[1]). Return value is the test's exit code.
// ---------------------------------------------------------------------------
static int child_main(int argc, char* argv[]) {
    const std::string mode = argv[1];
    if (mode == "--child-exit") return argc > 2 ? std::atoi(argv[2]) : 0;
    if (mode == "--child-err") {
        std::fputs("ERR-MARKER-123\n", stderr);
        return 0;
    }
    if (mode == "--child-echo") {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        std::fputs("OUT-MARKER\n", stderr);
        char buf[8192];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
            if (fwrite(buf, 1, n, stdout) != n) return 2;
        }
        return 0;
    }
    if (mode == "--child-big") {
        // 2MB stderr forces real drain-thread pressure; small stdout too.
        std::string chunk(65536, 'E');
        for (int i = 0; i < 32; ++i) fputs(chunk.c_str(), stderr);
        std::fputs("BIG-TAIL\n", stderr);
        std::fputs("small-out\n", stdout);
        return 5;
    }
    if (mode == "--child-sleep") {
        Sleep(argc > 2 ? static_cast<DWORD>(std::atoi(argv[2])) : 30000);
        return 0;
    }
    return 99;
}

static std::string self_exe() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return std::string(path);
}

static DWORD handle_count() {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

// Wait (bounded) until not running; fails the test on timeout.
static bool wait_gone(Spectral::SafeProcess& p) {
    for (int i = 0; i < 500; ++i) {
        if (!p.running()) return true;
        Sleep(10);
    }
    return false;
}

static void test_launch_and_exit() {
    std::printf("[launch_and_exit]\n");
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    CHECK(p.spawn(self_exe(), {"--child-exit", "7"}, o), "spawn");
    CHECK(p.wait() == 7, "exit code 7");
    CHECK(!p.running(), "not running after wait");
}

static void test_exit_before_wait() {
    std::printf("[exit_before_wait]\n");
    Spectral::SafeProcess p;
    CHECK(p.spawn(self_exe(), {"--child-exit", "3"}, {}), "spawn");
    CHECK(wait_gone(p), "child exits on its own");
    CHECK(p.wait() == 3, "wait after exit gives code");
}

static void test_exit_while_draining() {
    std::printf("[exit_while_draining]\n");
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    o.capture_stdout = true;
    o.capture_stderr = true;
    CHECK(p.spawn(self_exe(), {"--child-big"}, o), "spawn");
    // Never touch stderr here: the drain thread must handle 2MB alone.
    CHECK(p.wait() == 5, "exit code through drain pressure");
    const std::string err = p.stderr_text();
    CHECK(err.size() >= 2u * 1024u * 1024u, "full stderr captured");
    CHECK(err.find("BIG-TAIL") != std::string::npos, "stderr tail present");
}

static void test_stderr_output() {
    std::printf("[stderr_output]\n");
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    o.capture_stderr = true;
    CHECK(p.spawn(self_exe(), {"--child-err"}, o), "spawn");
    CHECK(p.wait() == 0, "exit 0");
    CHECK(p.stderr_text().find("ERR-MARKER-123") != std::string::npos, "stderr exact");
}

static void test_stdout_and_stderr() {
    std::printf("[stdout_and_stderr]\n");
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    o.provide_stdin = true;
    o.capture_stdout = true;
    o.capture_stderr = true;
    CHECK(p.spawn(self_exe(), {"--child-echo"}, o), "spawn");
    const std::string payload(100000, 'Q');
    CHECK(p.write_stdin(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
          "stdin written");
    p.close_stdin();
    std::string out;
    std::vector<uint8_t> buf(16384);
    for (;;) {
        size_t n = p.read_stdout(buf.data(), buf.size());
        CHECK(n != static_cast<size_t>(-1), "no read error");
        if (n == 0 || n == static_cast<size_t>(-1)) break;
        out.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    CHECK(out == payload, "stdout bytes exact");
    CHECK(p.wait() == 0, "exit 0");
    CHECK(p.stderr_text().find("OUT-MARKER") != std::string::npos, "stderr alongside");
}

static void test_respawn_cycles() {
    std::printf("[respawn_cycles]\n");
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    o.capture_stderr = true;
    for (int i = 0; i < 10; ++i) {
        CHECK(p.spawn(self_exe(), {"--child-exit", std::to_string(i)}, o), "respawn");
        CHECK(p.wait() == i, "cycle code");
        CHECK(p.stderr_text().empty(), "no stale stderr between runs");
        p.cleanup();
    }
}

static void test_kill_running() {
    std::printf("[kill_running]\n");
    Spectral::SafeProcess p;
    CHECK(p.spawn(self_exe(), {"--child-sleep", "30000"}, {}), "spawn sleeper");
    CHECK(p.running(), "sleeper running");
    p.kill();
    CHECK(!p.running(), "dead after kill");
    const int code = p.wait();
    CHECK(code != 0, "killed exit nonzero");
}

static void test_kill_after_exit() {
    std::printf("[kill_after_exit]\n");
    Spectral::SafeProcess p;
    CHECK(p.spawn(self_exe(), {"--child-exit", "11"}, {}), "spawn");
    CHECK(wait_gone(p), "exits alone");
    CHECK(p.wait() == 11, "code first");
    p.kill();  // must be a safe no-op w.r.t. resources
    CHECK(p.wait() == 11, "code preserved after kill");
}

static void test_repeated_wait() {
    std::printf("[repeated_wait]\n");
    Spectral::SafeProcess p;
    CHECK(p.spawn(self_exe(), {"--child-exit", "4"}, {}), "spawn");
    CHECK(p.wait() == 4, "first");
    CHECK(p.wait() == 4, "second");
    CHECK(p.wait() == 4, "third");
}

static void test_repeated_cleanup() {
    std::printf("[repeated_cleanup]\n");
    Spectral::SafeProcess p;
    p.cleanup();
    p.cleanup();
    CHECK(p.spawn(self_exe(), {"--child-exit", "0"}, {}), "spawn after cleanups");
    CHECK(p.wait() == 0, "exit 0");
    p.cleanup();
    p.cleanup();
}

static void test_dtor_running() {
    std::printf("[dtor_running]\n");
    const DWORD before = handle_count();
    {
        Spectral::SafeProcess p;
        CHECK(p.spawn(self_exe(), {"--child-sleep", "30000"}, {}), "spawn sleeper");
        CHECK(p.running(), "running at scope exit");
    }  // destructor must terminate + release without hanging
    CHECK(handle_count() == before, "no leaked handles");
}

static void test_dtor_after_exit() {
    std::printf("[dtor_after_exit]\n");
    const DWORD before = handle_count();
    {
        Spectral::SafeProcess p;
        CHECK(p.spawn(self_exe(), {"--child-exit", "0"}, {}), "spawn");
        CHECK(p.wait() == 0, "wait");
    }
    CHECK(handle_count() == before, "no leaked handles");
}

static void test_failed_launch() {
    std::printf("[failed_launch]\n");
    Spectral::SafeProcess p;
    CHECK(!p.spawn("Z:/definitely/not/here/nope.exe", {}, {}), "spawn fails");
    CHECK(!p.last_error().empty(), "diagnostic present");
    CHECK(p.exit_code() == -1, "no exit code");
    CHECK(!p.running(), "not running");
    // destructor after failure must be safe (covered on scope exit too)
    p.cleanup();
}

static void test_no_leak_on_failure() {
    std::printf("[no_leak_on_failure]\n");
    const DWORD before = handle_count();
    for (int i = 0; i < 5; ++i) {
        Spectral::SafeProcess p;
        CHECK(!p.spawn("Z:/definitely/not/here/nope.exe", {}, {}), "fails");
        p.cleanup();
    }
    CHECK(handle_count() == before, "no leaked handles");
}

static void test_launch_after_failure() {
    std::printf("[launch_after_failure]\n");
    Spectral::SafeProcess p;
    CHECK(!p.spawn("Z:/definitely/not/here/nope.exe", {}, {}), "first fails");
    CHECK(p.spawn(self_exe(), {"--child-exit", "9"}, {}), "second works");
    CHECK(p.wait() == 9, "code correct after failure");
    CHECK(p.stderr_text().empty(), "no stale state");
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::strncmp(argv[1], "--child-", 8) == 0) {
        return child_main(argc, argv);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_launch_and_exit();
    test_exit_before_wait();
    test_exit_while_draining();
    test_stderr_output();
    test_stdout_and_stderr();
    test_respawn_cycles();
    test_kill_running();
    test_kill_after_exit();
    test_repeated_wait();
    test_repeated_cleanup();
    test_dtor_running();
    test_dtor_after_exit();
    test_failed_launch();
    test_no_leak_on_failure();
    test_launch_after_failure();
    std::printf("\n=== safe_process: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
