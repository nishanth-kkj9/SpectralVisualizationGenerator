// tests/dsp/test_fft_plan.cpp
// Phase 7 — FFTPlan/FFTWorkspace equivalence and lifetime tests.
// The engine must be bit-identical to fft() (same butterfly, same order,
// same twiddle values via the identical expression), reusable without
// reallocation, and fail-closed on invalid input.
#include "fft.h"
#include "fft_plan.h"
#include "stft.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

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

// Deterministic pseudo-random signal (LCG; fixed seed, no <random> drift).
static std::vector<complex_f> test_signal(int n, uint32_t seed) {
    std::vector<complex_f> v(static_cast<size_t>(n));
    uint32_t s = seed;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        const float re = static_cast<float>(s >> 8) / 16777216.0f - 0.5f;
        s = s * 1664525u + 1013904223u;
        const float im = static_cast<float>(s >> 8) / 16777216.0f - 0.5f;
        v[static_cast<size_t>(i)] = complex_f(re, im);
    }
    return v;
}

static void test_validity() {
    std::printf("[validity]\n");
    for (int n = 2; n <= 32768; n <<= 1) {
        Spectral::FFTPlan p = Spectral::FFTPlan::create(n);
        CHECK(p.valid(), "pow2 plan valid");
        CHECK(p.size() == n, "plan size");
        int stages = 0;
        for (int len = 2; len <= n; len <<= 1) {
            CHECK(static_cast<int>(p.twiddles(stages).size()) == len / 2,
                  "twiddle table size");
            ++stages;
        }
        CHECK(p.stages() == stages, "stage count");
    }
    for (int n : {0, 1, 3, 5, 100, 1000, 1536, -4}) {
        CHECK(!Spectral::FFTPlan::create(n).valid(), "non-pow2 plan invalid");
    }
    // Swap sequence is an involution (applying twice restores order),
    // i.e. a genuine reversal permutation.
    for (int n : {2, 64, 1024}) {
        Spectral::FFTPlan p = Spectral::FFTPlan::create(n);
        std::vector<int> v(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = i;
        for (auto [a, b] : p.swaps()) std::swap(v[a], v[b]);
        bool perm = true;
        std::vector<char> seen(static_cast<size_t>(n), 0);
        for (int x : v) {
            if (x < 0 || x >= n || seen[x]) perm = false;
            seen[x] = 1;
        }
        CHECK(perm, "swaps form a permutation");
        for (auto [a, b] : p.swaps()) std::swap(v[a], v[b]);
        bool ident = true;
        for (int i = 0; i < n; ++i) ident = ident && (v[i] == i);
        CHECK(ident, "swaps are an involution");
    }
    // Tabled twiddles equal the loop expression, bit for bit.
    for (int n : {8, 512, 4096}) {
        Spectral::FFTPlan p = Spectral::FFTPlan::create(n);
        bool ok = true;
        int stage = 0;
        for (int len = 2; len <= n && ok; len <<= 1, ++stage) {
            for (int j = 0; j < len / 2; ++j) {
                const complex_f t = ftwiddle(n, j, n / len);
                if (p.twiddles(stage)[j] != t) {
                    ok = false;
                    break;
                }
            }
        }
        CHECK(ok, "twiddles bit-identical to loop expression");
    }
}

static void test_forward_equivalence() {
    std::printf("[forward_equivalence]\n");
    for (int n : {2, 4, 8, 16, 64, 256, 1024, 4096, 8192}) {
        Spectral::FFTPlan plan = Spectral::FFTPlan::create(n);
        Spectral::FFTWorkspace ws(plan);
        CHECK(ws.valid(), "workspace valid");
        auto input = test_signal(n, 12345u);
        std::vector<complex_f> expect = input;
        fft(expect, false);
        std::vector<complex_f>& buf = ws.buf(0);
        buf = input;
        CHECK(Spectral::fft_forward(plan, ws, 0), "forward executes");
        CHECK(buf == expect, "forward bit-identical to fft()");
    }
}

static void test_inverse() {
    std::printf("[inverse]\n");
    for (int n : {4, 256, 2048}) {
        Spectral::FFTPlan plan = Spectral::FFTPlan::create(n);
        Spectral::FFTWorkspace ws(plan);
        auto input = test_signal(n, 777u);
        std::vector<complex_f> expect = input;
        fft(expect, false);
        fft(expect, true);
        ws.buf(0) = input;
        CHECK(Spectral::fft_forward(plan, ws, 0), "fwd ok");
        CHECK(Spectral::fft_inverse(plan, ws, 0), "inv ok");
        CHECK(ws.buf(0) == expect, "plan round-trip identical to fft() path");
        // IFFT(FFT(x)) == x within float tolerance (same bar as fft()).
        float worst = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float d = std::abs(ws.buf(0)[i] - input[i]);
            if (d > worst) worst = d;
        }
        CHECK(worst <= 1e-5f, "round-trip recovers input");
    }
}

static void test_reuse_and_independence() {
    std::printf("[reuse_and_independence]\n");
    const int n = 1024;
    Spectral::FFTPlan plan = Spectral::FFTPlan::create(n);
    Spectral::FFTWorkspace a(plan), b(plan);
    // Same plan shared by two workspaces; interleaved executions stay
    // correct and independent.
    auto in_a = test_signal(n, 11u);
    auto in_b = test_signal(n, 22u);
    auto exp_a = in_a, exp_b = in_b;
    fft(exp_a, false);
    fft(exp_b, false);
    a.buf(0) = in_a;
    b.buf(0) = in_b;
    CHECK(Spectral::fft_forward(plan, a, 0), "a executes");
    CHECK(Spectral::fft_forward(plan, b, 0), "b executes");
    CHECK(a.buf(0) == exp_a && b.buf(0) == exp_b, "workspaces independent");
    // 200 consecutive executions: same addresses, same capacity (no
    // reallocation), correct results every time.
    const complex_f* addr0 = a.buf(0).data();
    const size_t cap0 = a.buf(0).capacity();
    bool ok = true;
    for (int r = 0; r < 200; ++r) {
        auto in = test_signal(n, 1000u + r);
        auto exp = in;
        fft(exp, false);
        a.buf(0) = in;
        if (!Spectral::fft_forward(plan, a, 0) || a.buf(0) != exp) {
            ok = false;
            break;
        }
        if (a.buf(0).data() != addr0 || a.buf(0).capacity() != cap0) {
            ok = false;
            break;
        }
    }
    CHECK(ok, "200 reuses: stable storage, exact results");
    // assign() with the same size keeps storage too.
    a.assign(plan);
    CHECK(a.buf(0).data() == addr0 && a.buf(0).capacity() == cap0,
          "assign same size keeps storage");
}

static void test_refusals() {
    std::printf("[refusals]\n");
    Spectral::FFTPlan bad = Spectral::FFTPlan::create(1000);
    Spectral::FFTPlan good = Spectral::FFTPlan::create(256);
    Spectral::FFTWorkspace ws(good);
    ws.buf(0) = test_signal(256, 5u);
    const auto snapshot = ws.buf(0);
    CHECK(!Spectral::fft_forward(bad, ws, 0), "invalid plan refused");
    CHECK(!Spectral::fft_inverse(bad, ws, 0), "invalid plan refused (inv)");
    CHECK(ws.buf(0) == snapshot, "buffer untouched on refusal");
    Spectral::FFTWorkspace other(Spectral::FFTPlan::create(512));
    CHECK(!Spectral::fft_forward(good, other, 0), "size mismatch refused");
    CHECK(!Spectral::fft_forward(good, ws, 3), "buffer index refused");
    CHECK(!Spectral::fft_forward(good, ws, -1), "negative index refused");
    Spectral::FFTWorkspace empty;
    CHECK(!empty.valid(), "default workspace invalid");
    CHECK(!Spectral::fft_forward(good, empty, 0), "empty workspace refused");
}

static void test_windowed_frame_equivalence() {
    std::printf("[windowed_frame_equivalence]\n");
    // The exact fill+normalize sequence the pipeline uses, checked against
    // Spectral::stft_frame() across windows and sizes.
    for (int n : {256, 1024, 4096}) {
        for (const char* wname : {"hann", "hamming", "blackman", "rectangular"}) {
            Spectral::FFTPlan plan = Spectral::FFTPlan::create(n);
            Spectral::FFTWorkspace ws(plan);
            const auto win = Spectral::stft_window(wname, n);
            const float cg = window_coherent_gain(win);
            auto audio = test_signal(n * 2, 999u);
            std::vector<float> af(n * 2);
            for (int i = 0; i < n * 2; ++i) af[i] = audio[i].real();
            Spectral::StftFrame ref;
            CHECK(Spectral::stft_frame(af.data(), n * 2, n / 2, n, 22050, win, cg,
                                       ref),
                  "reference frame ok");
            std::vector<complex_f>& w0 = ws.buf(0);
            for (int j = 0; j < n; ++j)
                w0[j] = complex_f(af[n / 2 + j] * win[j], 0.0f);
            CHECK(Spectral::fft_forward(plan, ws, 0), "planned frame ok");
            bool ok = true;
            const float norm = 1.0f / (static_cast<float>(n) * cg);
            for (int k = 0; k < n / 2 + 1 && ok; ++k) {
                const float re = w0[k].real(), im = w0[k].imag();
                float mag = std::sqrt(re * re + im * im) * norm;
                if (k > 0 && k < n / 2) mag *= 2.0f;
                ok = mag == ref.magnitudes[k] &&
                     std::atan2(im, re) == ref.phases[k] && mag * mag == ref.power[k];
            }
            CHECK(ok, "windowed frame bit-identical to Spectral::stft_frame()");
        }
    }
}

int main() {
    test_validity();
    test_forward_equivalence();
    test_inverse();
    test_reuse_and_independence();
    test_refusals();
    test_windowed_frame_equivalence();
    std::printf("\n=== fft_plan: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
