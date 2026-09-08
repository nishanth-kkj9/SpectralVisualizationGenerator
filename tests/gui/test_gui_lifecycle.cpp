// tests/gui/test_gui_lifecycle.cpp
// Phase 5 correction — GUI worker shutdown lifecycle, driven headlessly
// (offscreen platform) against the REAL MainWindow, REAL worker thread,
// and REAL pipeline/ffmpeg. Proves: close with no job closes at once;
// close mid-job DEFERS (window stays alive, cancel requested) and the
// window only goes away after the worker thread is done; double close,
// cancel-then-close, and reuse-after-completion all behave; no crash on
// window destruction in any path.
#include "main_window.h"

#include <QApplication>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace fs = std::filesystem;

static int g_run = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        ++g_run;                                                    \
        if (cond) {                                                 \
            ++g_pass;                                               \
        } else {                                                    \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);   \
        }                                                           \
    } while (0)

static fs::path g_dir;

// Pump the event loop until fn() is true (10ms steps). Timeout fails
// honestly; success means the waited-for GUI/worker state was observed.
static bool pump_until(const std::function<bool()>& fn, int timeout_ms = 60000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (fn()) return true;
        QThread::msleep(5);
    }
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    return fn();
}

static void pump(int ms = 200) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
}

static void write_wav(const fs::path& p, int seconds, double freq_hz = 440.0) {
    const int sr = 22050, n = sr * seconds;
    std::ofstream f(p, std::ios::binary);
    const int data_size = n * 2, chunk = 36 + data_size;
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&chunk), 4);
    f.write("WAVEfmt ", 8);
    const int fmt = 16;
    f.write(reinterpret_cast<const char*>(&fmt), 4);
    const int16_t tag = 1, ch = 1;
    f.write(reinterpret_cast<const char*>(&tag), 2);
    f.write(reinterpret_cast<const char*>(&ch), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    const int br = sr * 2;
    f.write(reinterpret_cast<const char*>(&br), 4);
    const int16_t ba = 2, bps = 16;
    f.write(reinterpret_cast<const char*>(&ba), 2);
    f.write(reinterpret_cast<const char*>(&bps), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_size), 4);
    for (int i = 0; i < n; ++i) {
        const auto s = static_cast<int16_t>(
            10000.0 * std::sin(2.0 * 3.14159265 * freq_hz * i / sr));
        f.write(reinterpret_cast<const char*>(&s), 2);
    }
}

// 1. Close with no job active closes immediately.
static void test_close_idle() {
    std::printf("[close_idle]\n");
    auto* w = new MainWindow();
    w->show();
    CHECK(pump_until([&] { return w->isVisible(); }, 5000), "window shown");
    CHECK(!w->isJobActiveForTest(), "no worker when idle");
    w->close();
    pump();
    CHECK(!w->isVisible(), "idle close accepted at once");
    delete w;  // must not crash with no worker ever started
}

// 2/3. Close mid-job defers: window stays alive, cancel is requested, the
// window only goes away after the worker thread is done. Repeated rounds
// raise confidence against shutdown races (a premature destroy aborts).
static void test_deferred_close(int rounds, bool double_close) {
    std::printf("[deferred_close x%d%s]\n", rounds, double_close ? " double" : "");
    write_wav(g_dir / "long.wav", 60);  // many seconds of analysis
    for (int r = 0; r < rounds; ++r) {
        const fs::path out = g_dir / ("dc" + std::to_string(r) + ".png");
        fs::remove(out);
        auto* w = new MainWindow();
        w->show();
        CHECK(pump_until([&] { return w->isVisible(); }, 5000), "window shown");
        w->setInputForTest(QString::fromStdString((g_dir / "long.wav").string()));
        w->setOutputForTest(QString::fromStdString(out.string()));
        w->generate();
        // Job provably active before the close request lands.
        CHECK(pump_until([&] { return w->isJobActiveForTest(); }, 15000),
              "worker active before close");
        w->close();
        if (double_close) w->close();  // second attempt while deferred: safe
        // closeEvent runs synchronously inside close(): an ignored event
        // leaves the window visible with no event pumping required. (A
        // pump here would let the fast cancel+deferred-close complete
        // first, which is also correct but unobservable this way.)
        CHECK(w->isVisible(), "close deferred while worker alive");
        // The deferred close fires by itself once the thread is done.
        CHECK(pump_until([&] { return !w->isVisible(); }, 90000),
              "window closes after worker done");
        CHECK(!w->isJobActiveForTest(), "no running worker after deferred close");
        CHECK(w->statusTextForTest() == QStringLiteral("Cancelled"),
              "final state is cancelled, not success");
        CHECK(!fs::exists(out), "no partial output after deferred close");
        delete w;  // destruction after worker completion: must not crash
        pump();
    }
}

// 4. Cancel, then close once the worker is done: immediate close.
static void test_cancel_then_close() {
    std::printf("[cancel_then_close]\n");
    write_wav(g_dir / "med.wav", 30);
    const fs::path out = g_dir / "ctc.png";
    fs::remove(out);
    auto* w = new MainWindow();
    w->show();
    w->setInputForTest(QString::fromStdString((g_dir / "med.wav").string()));
    w->setOutputForTest(QString::fromStdString(out.string()));
    w->generate();
    CHECK(pump_until([&] { return w->isJobActiveForTest(); }, 15000), "worker active");
    w->cancelJob();
    CHECK(pump_until([&] { return !w->isJobActiveForTest(); }, 90000),
          "worker stopped after cancel");
    CHECK(w->statusTextForTest() == QStringLiteral("Cancelled"), "cancelled state shown");
    w->close();
    pump();
    CHECK(!w->isVisible(), "close after cancel accepted at once");
    CHECK(!fs::exists(out), "no partial output");
    delete w;
}

// 5. A job can still run correctly after a previous completion (state
// reset), then the window closes normally.
static void test_reuse_after_completion() {
    std::printf("[reuse_after_completion]\n");
    write_wav(g_dir / "short.wav", 2);
    auto* w = new MainWindow();
    w->show();
    for (int r = 0; r < 2; ++r) {
        const fs::path out = g_dir / ("re" + std::to_string(r) + ".png");
        fs::remove(out);
        w->setInputForTest(QString::fromStdString((g_dir / "short.wav").string()));
        w->setOutputForTest(QString::fromStdString(out.string()));
        w->generate();
        CHECK(pump_until([&] { return !w->isJobActiveForTest(); }, 90000),
              "job completes");
        CHECK(w->statusTextForTest() == QStringLiteral("Done"), "success shown");
        CHECK(fs::exists(out), "output produced");
    }
    w->close();
    pump();
    CHECK(!w->isVisible(), "close after jobs accepted");
    delete w;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
#ifdef GUI_QPA_PLUGIN_PATH
    // Plugin DLLs live in the vcpkg tree, not next to the test binary.
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", QByteArray(GUI_QPA_PLUGIN_PATH));
#endif
    QApplication app(argc, argv);
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / "svg_phase5_gui_test";
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FAIL: cannot create test dir\n");
        return 1;
    }
    test_close_idle();
    test_deferred_close(3, false);
    test_deferred_close(1, true);
    test_cancel_then_close();
    test_reuse_after_completion();
    fs::remove_all(g_dir, ec);
    std::printf("\n=== gui_lifecycle: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
