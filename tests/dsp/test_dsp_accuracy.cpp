// tests/dsp/test_dsp_accuracy.cpp
// S4 independent DSP validation. The reference is a deliberately naive
// O(N^2) double-precision DFT plus closed-form signal expectations —
// never production helpers, never copied butterfly logic.
// Tolerances are justified inline from float eps and FFT error growth.
#define NOMINMAX
#include "fft.h"
#include "stft.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace Spectral;

const double PI_D = 3.14159265358979323846;

int g_run = 0;
int g_pass = 0;
double g_max_fft_err = 0.0;
double g_max_roundtrip_err = 0.0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_run;                                                             \
        if (cond) {                                                          \
            ++g_pass;                                                        \
        } else {                                                             \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);            \
        }                                                                    \
    } while (0)

// Fixed-seed uniform generator: deterministic across runs/platforms.
struct Lcg {
    uint32_t s = 0x12345678u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) / 16777216.0 * 2.0 - 1.0);
    }
};

// Obviously-correct reference DFT (double). Forward: exp(-2pi i).
// Inverse: exp(+2pi i) with 1/N.
using cd = std::complex<double>;

std::vector<cd> dft_ref(const std::vector<cd>& x, bool inverse) {
    const int N = static_cast<int>(x.size());
    std::vector<cd> X(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k) {
        cd s(0.0, 0.0);
        for (int n = 0; n < N; ++n) {
            const double a = 2.0 * PI_D * k * n / N * (inverse ? 1.0 : -1.0);
            s += x[n] * cd(std::cos(a), std::sin(a));
        }
        X[static_cast<size_t>(k)] = inverse ? s / static_cast<double>(N) : s;
    }
    return X;
}

std::vector<complex_f> to_f(const std::vector<cd>& x) {
    std::vector<complex_f> o(x.size());
    for (size_t i = 0; i < x.size(); ++i)
        o[i] = complex_f(static_cast<float>(x[i].real()), static_cast<float>(x[i].imag()));
    return o;
}

double max_abs_err(const std::vector<complex_f>& a, const std::vector<cd>& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double e = std::abs(cd(a[i].real(), a[i].imag()) - b[i]);
        if (e > m) m = e;
    }
    return m;
}

// --- 1. FFT vs independent DFT -------------------------------------------
void test_fft_vs_dft() {
    std::printf("[fft_vs_dft]\n");
    // Float FFT error grows ~eps*log2(N)*|X|; with |X|<=N the absolute
    // bound at N=64 is ~1e-4. Assert well above it, tight enough to catch
    // any structural bug (broken inverse gave errors ~1.0).
    for (int N : {8, 16, 32, 64}) {
        for (int kind = 0; kind < 3; ++kind) {
            std::vector<cd> x(static_cast<size_t>(N));
            Lcg rng;
            rng.s = 0x1000u + static_cast<uint32_t>(N * 97 + kind);
            for (int n = 0; n < N; ++n) {
                if (kind == 0)
                    x[static_cast<size_t>(n)] = cd(rng.next(), rng.next());  // complex
                else if (kind == 1)
                    x[static_cast<size_t>(n)] = cd(n == 3 ? 1.0 : 0.0, 0.0);  // impulse
                else
                    x[static_cast<size_t>(n)] = cd(0.5, 0.0);                 // DC
            }
            std::vector<complex_f> got = to_f(x);
            fft(got, false);
            const double e = max_abs_err(got, dft_ref(x, false));
            if (e > g_max_fft_err) g_max_fft_err = e;
            CHECK(e < 1e-4, "forward matches DFT");
            std::vector<complex_f> back = got;
            fft(back, true);
            const double er = max_abs_err(back, x);
            if (er > g_max_roundtrip_err) g_max_roundtrip_err = er;
            CHECK(er < 1e-4, "inverse recovers input");
        }
    }
}

// --- 2. Round trip over sizes and signals ---------------------------------
std::vector<float> make_signal(int kind, int N, Lcg& rng) {
    std::vector<float> x(static_cast<size_t>(N));
    for (int n = 0; n < N; ++n) {
        const double t = static_cast<double>(n) / N;
        switch (kind) {
            case 0: x[static_cast<size_t>(n)] = (n == 0) ? 1.0f : 0.0f; break;  // impulse
            case 1: x[static_cast<size_t>(n)] = 0.7f; break;                    // DC
            case 2: x[static_cast<size_t>(n)] = (n % 2 == 0) ? 1.0f : -1.0f; break;  // Nyquist
            case 3: x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * 5 * t)); break;
            case 4: x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * 5.5 * t)); break;  // off-bin
            case 5: x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * 3 * t) + 0.5 * std::sin(2 * PI_D * 9 * t)); break;
            case 6: x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * (2 + 30 * t) * t)); break;  // chirp
            default: x[static_cast<size_t>(n)] = rng.next(); break;  // noise
        }
    }
    return x;
}

void test_roundtrip() {
    std::printf("[roundtrip]\n");
    Lcg rng;
    for (int N : {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192}) {
        for (int kind = 0; kind < 8; ++kind) {
            auto r = make_signal(kind, N, rng);
            std::vector<complex_f> x(static_cast<size_t>(N));
            for (int n = 0; n < N; ++n) x[static_cast<size_t>(n)] = complex_f(r[static_cast<size_t>(n)], 0.0f);
            // complex random variant for kind 7 on larger sizes
            if (kind == 7) {
                for (int n = 0; n < N; ++n)
                    x[static_cast<size_t>(n)] = complex_f(rng.next(), rng.next());
            }
            std::vector<complex_f> orig = x;
            fft(x, false);
            fft(x, true);
            double m = 0.0, scale = 0.0;
            for (int n = 0; n < N; ++n) {
                const double e = std::abs(cd(x[static_cast<size_t>(n)]) - cd(orig[static_cast<size_t>(n)]));
                if (e > m) m = e;
                const double a = std::abs(cd(orig[static_cast<size_t>(n)]));
                if (a > scale) scale = a;
            }
            if (m > g_max_roundtrip_err) g_max_roundtrip_err = m;
            // Tolerance: relative 1e-4 (N<=1024) / 5e-3 (larger); float FFT
            // roundoff is ~1e-6 relative, so this is 100-1000x headroom while
            // any structural defect (e.g. missing conjugation) errs ~1.0.
            const double tol = (N <= 1024 ? 1e-4 : 5e-3) * (scale > 0.0 ? scale : 1.0);
            CHECK(m <= tol, "IFFT(FFT(x))==x");
        }
    }
}

// --- 3. Known signals, Fs=48000, N=4096 -----------------------------------
void test_known_signals() {
    std::printf("[known_signals]\n");
    const int sr = 48000, N = 4096;
    const double bin_hz = static_cast<double>(sr) / N;  // 11.71875
    auto spectrum = [&](const std::vector<float>& x) {
        std::vector<complex_f> X(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n) X[static_cast<size_t>(n)] = complex_f(x[static_cast<size_t>(n)], 0.0f);
        fft(X, false);
        std::vector<float> m(static_cast<size_t>(N));
        for (int k = 0; k < N; ++k) m[static_cast<size_t>(k)] = std::abs(X[static_cast<size_t>(k)]);
        return m;
    };
    auto argmax = [](const std::vector<float>& m, int lo, int hi) {
        int b = lo;
        for (int k = lo + 1; k <= hi; ++k)
            if (m[static_cast<size_t>(k)] > m[static_cast<size_t>(b)]) b = k;
        return b;
    };

    // exact-bin sine, k=410 -> 4804.6875 Hz, amplitude 1
    {
        const int k = 410;
        std::vector<float> x(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n)
            x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * k * n / N));
        auto m = spectrum(x);
        CHECK(argmax(m, 1, N / 2 - 1) == k, "exact-bin peak location");
        CHECK(std::fabs(m[static_cast<size_t>(k)] - N / 2.0f) < N * 0.01f, "exact-bin peak height N/2");
    }
    // off-bin sine at k+0.5: peak adjacent, lower than on-bin case
    {
        std::vector<float> x(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n)
            x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * 410.5 * n / N));
        auto m = spectrum(x);
        const int p = argmax(m, 1, N / 2 - 1);
        CHECK(p == 410 || p == 411, "off-bin peak adjacent");
        CHECK(m[static_cast<size_t>(p)] < N / 2.0f, "off-bin peak leaks below N/2");
        CHECK(m[static_cast<size_t>(p)] > N * 0.2f, "off-bin peak still dominant");
    }
    // two tones
    {
        std::vector<float> x(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n)
            x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * 100 * n / N) +
                                                           0.5 * std::sin(2 * PI_D * 300 * n / N));
        auto m = spectrum(x);
        CHECK(argmax(m, 90, 110) == 100, "tone 1 at bin 100");
        CHECK(argmax(m, 290, 310) == 300, "tone 2 at bin 300");
    }
    // DC: energy at bin 0 only
    {
        std::vector<float> x(static_cast<size_t>(N), 0.25f);
        auto m = spectrum(x);
        CHECK(std::fabs(m[0] - 0.25f * N) < N * 0.001f, "DC bin holds the sum");
        CHECK(m[static_cast<size_t>(N / 2)] < 1e-3f * N, "DC leaks nothing to Nyquist");
    }
    // Nyquist: alternating signal peaks at bin N/2
    {
        std::vector<float> x(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n) x[static_cast<size_t>(n)] = (n % 2 == 0) ? 1.0f : -1.0f;
        auto m = spectrum(x);
        CHECK(argmax(m, 0, N - 1) == N / 2, "Nyquist bin peaks");
        CHECK(std::fabs(m[static_cast<size_t>(N / 2)] - N) < N * 0.01f, "Nyquist height N");
        (void)bin_hz;
    }
    // real-input symmetry X[N-k] == conj(X[k])
    {
        Lcg rng;
        std::vector<complex_f> X(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n) X[static_cast<size_t>(n)] = complex_f(rng.next(), 0.0f);
        fft(X, false);
        double e = 0.0;
        for (int k = 1; k < N; ++k) {
            const double d = std::abs(X[static_cast<size_t>(k)] - std::conj(X[static_cast<size_t>(N - k)]));
            if (d > e) e = d;
        }
        CHECK(e < 1e-2f, "real-input symmetry");
    }
}

// --- 4. Size validation ------------------------------------------------------
void test_size_validation() {
    std::printf("[size_validation]\n");
    for (int bad : {0, 1, -4, 3, 6, 100, 1000, 4097}) {
        CHECK(!is_valid_fft_size(bad), "rejected size");
        std::vector<complex_f> x;
        if (bad > 0) x.assign(static_cast<size_t>(bad), complex_f(1.0f, 0.5f));
        std::vector<complex_f> keep = x;
        CHECK(!fft_checked(x, false), "fft_checked refuses");
        CHECK(x == keep, "input untouched on refusal");
    }
    for (int good : {2, 8, 1024}) {
        CHECK(is_valid_fft_size(good), "accepted size");
        std::vector<complex_f> x(static_cast<size_t>(good), complex_f(1.0f, 0.0f));
        CHECK(fft_checked(x, false), "fft_checked accepts");
    }
}

// --- 5. STFT frames ----------------------------------------------------------
void test_stft_frames() {
    std::printf("[stft_frames]\n");
    CHECK(stft_frame_count(100, 1024, 512) == 0, "shorter than N -> 0");
    CHECK(stft_frame_count(1024, 1024, 512) == 1, "exactly one frame");
    CHECK(stft_frame_count(1024 + 512, 1024, 512) == 2, "N+hop -> 2");
    CHECK(stft_frame_count(1024 + 2 * 512 + 3, 1024, 512) == 3, "floor policy");
    CHECK(stft_frame_count(0, 1024, 512) == 0, "empty -> 0");

    const int sr = 16000, N = 512, hop = 128;
    const auto win = window_hann(N);
    const float cg = window_coherent_gain(win);
    // impulse at sample 1000: frame floor((1000-N+1..1000)/hop)... first frame
    // starting at <= 1000 with start+N > 1000 -> start=512 (frame 4)
    {
        std::vector<float> x(4000, 0.0f);
        x[1000] = 1.0f;
        auto frames = stft_all(x.data(), 4000, N, hop, sr, win, cg);
        CHECK(static_cast<int>(frames.size()) == stft_frame_count(4000, N, hop), "count matches policy");
        double best = -1.0;
        int best_f = -1;
        for (size_t f = 0; f < frames.size(); ++f) {
            double e = 0;
            for (float v : frames[f].power) e += v;
            if (e > best) {
                best = e;
                best_f = static_cast<int>(f);
            }
        }
        // Frames covering sample 1000 are 4..7; Hann weights peak at local
        // index 232 (frame 6, w=0.979 vs 0.642/0.356/0.020), so frame 6 wins.
        CHECK(best_f == 6, "impulse lands in the max-weight frame");
        CHECK(std::fabs(frames[static_cast<size_t>(best_f)].timestamp - 768.0 / sr) < 1e-9,
              "frame timestamp exact");
    }
    // silence: all magnitudes ~0, deterministic rerun
    {
        std::vector<float> x(2048, 0.0f);
        auto a = stft_all(x.data(), 2048, N, hop, sr, win, cg);
        auto b = stft_all(x.data(), 2048, N, hop, sr, win, cg);
        CHECK(a.size() == b.size() && !a.empty(), "silence frames exist");
        bool zero = true, same = true;
        for (size_t f = 0; f < a.size(); ++f) {
            for (size_t k = 0; k < a[f].magnitudes.size(); ++k) {
                if (a[f].magnitudes[k] > 1e-6f) zero = false;
                if (a[f].magnitudes[k] != b[f].magnitudes[k]) same = false;
            }
        }
        CHECK(zero, "silence is silent");
        CHECK(same, "deterministic rerun");
    }
    // exact-bin sine through stft_all peaks at the right bin every frame
    {
        const int k = 40;
        std::vector<float> x(4096);
        for (int n = 0; n < 4096; ++n)
            x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * k * n / N));
        auto frames = stft_all(x.data(), 4096, N, hop, sr, win, cg);
        bool ok = !frames.empty();
        for (const auto& fr : frames) {
            int p = 1;
            for (int b = 2; b < N / 2; ++b)
                if (fr.magnitudes[static_cast<size_t>(b)] > fr.magnitudes[static_cast<size_t>(p)]) p = b;
            if (p != k) ok = false;
        }
        CHECK(ok, "sine peaks every frame");
    }
    // bad params fail, never crash
    {
        StftFrame fr;
        std::vector<float> x(100, 0.1f);
        CHECK(!stft_frame(nullptr, 100, 0, N, sr, win, cg, fr), "null audio refused");
        CHECK(!stft_frame(x.data(), 100, 0, N, sr, win, cg, fr), "short input refused");
        CHECK(!stft_frame(x.data(), 2048, 0, 1000, sr, win, cg, fr), "non-pow2 refused");
    }
}

// --- 6. Windows vs independent double formulas --------------------------------
void test_windows() {
    std::printf("[windows]\n");
    const int N = 4096;
    auto indep = [&](const char* name) {
        std::vector<double> w(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n) {
            const double a = 2 * PI_D * n / (N - 1);
            if (std::strcmp(name, "hann") == 0) w[static_cast<size_t>(n)] = 0.5 * (1 - std::cos(a));
            if (std::strcmp(name, "hamming") == 0) w[static_cast<size_t>(n)] = 0.54 - 0.46 * std::cos(a);
            if (std::strcmp(name, "blackman") == 0)
                w[static_cast<size_t>(n)] = 0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2 * a);
            if (std::strcmp(name, "rect") == 0) w[static_cast<size_t>(n)] = 1.0;
        }
        return w;
    };
    const struct {
        const char* name;
        std::vector<float> impl;
        double cg_exact;
    } cases[] = {
        {"hann", window_hann(N), 0.5 * (1 - 1.0 / N)},
        {"hamming", window_hamming(N), 0.54 - 0.46 / N},
        {"blackman", window_blackman(N), 0.42 - 0.5 / N + 0.08 / N},
        {"rect", window_rectangular(N), 1.0},
    };
    for (const auto& c : cases) {
        auto ref = indep(c.name);
        double maxe = 0.0, sum = 0.0;
        bool range = true;
        for (int n = 0; n < N; ++n) {
            const double e = std::fabs(c.impl[static_cast<size_t>(n)] - ref[static_cast<size_t>(n)]);
            if (e > maxe) maxe = e;
            sum += c.impl[static_cast<size_t>(n)];
            if (c.impl[static_cast<size_t>(n)] < -1e-6f || c.impl[static_cast<size_t>(n)] > 1.0f + 1e-6f)
                range = false;
        }
        // Tolerance rationale: production uses float32 PI, the oracle
        // float64 PI; that single-constant gap dominates at ~1e-6, so 1e-5
        // is tight yet honest (a wrong formula errs ~1e-1 or more).
        CHECK(maxe < 1e-5, "window matches independent formula");
        CHECK(std::fabs(sum / N - c.cg_exact) < 1e-6, "coherent gain exact");
        CHECK(range, "window range [0,1]");
    }
    // symmetric endpoints: Hann/Hamming/Blackman start and end at ~0
    CHECK(window_hann(N).front() < 1e-6f && window_hann(N).back() < 1e-6f, "hann endpoints");
    CHECK(window_rectangular(N).front() == 1.0f, "rect endpoints");
}

// --- 7. Normalization: exact-bin unit sine reads 1.0 --------------------------
void test_normalization() {
    std::printf("[normalization]\n");
    const int sr = 48000;
    const char* wins[] = {"hann", "hamming", "blackman", "rectangular"};
    for (int N : {512, 2048, 8192}) {
        for (const char* wname : wins) {
            const auto win = stft_window(wname, N);
            const float cg = window_coherent_gain(win);
            const int k = N / 8;  // exact bin, away from DC/Nyquist
            std::vector<float> x(static_cast<size_t>(N));
            for (int n = 0; n < N; ++n)
                x[static_cast<size_t>(n)] = static_cast<float>(std::sin(2 * PI_D * k * n / N));
            Spectral::StftFrame fr;
            CHECK(stft_frame(x.data(), N, 0, N, sr, win, cg, fr), "frame ok");
            const float got = fr.magnitudes[static_cast<size_t>(k)];
            CHECK(std::fabs(got - 1.0f) < 0.02f, "unit sine reads 1.0 across N/windows");
            const float p = fr.power[static_cast<size_t>(k)];
            CHECK(std::fabs(p - got * got) < 1e-6f, "power == mag^2");
            const float mdb = magnitude_to_db(got);
            const float pdb = power_to_db(p);
            CHECK(std::fabs(mdb - pdb) < 1e-3f, "20log10(m) == 10log10(p)");
        }
    }
    // dB edges: zero/negative/NaN floor, +Inf passes to renderer ceiling
    const float nanv = std::numeric_limits<float>::quiet_NaN();
    const float infv = std::numeric_limits<float>::infinity();
    CHECK(power_to_db(0.0f) == -90.0f, "zero power floors");
    CHECK(power_to_db(-2.0f) == -90.0f, "negative floors");
    CHECK(power_to_db(nanv) == -90.0f, "NaN power floors");
    CHECK(magnitude_to_db(nanv) == -90.0f, "NaN magnitude floors");
    CHECK(magnitude_to_db(-1.0f) == -90.0f, "negative magnitude floors");
    CHECK(power_to_db(infv) > 300.0f, "+Inf passes through");
    CHECK(std::fabs(power_to_db(0.01f) + 20.0f) < 1e-4f, "10*log10(0.01) == -20");
    CHECK(std::fabs(magnitude_to_db(0.1f) + 20.0f) < 1e-4f, "20*log10(0.1) == -20");
}

// --- 8. Phase sanity -----------------------------------------------------------
void test_phase() {
    std::printf("[phase]\n");
    const int N = 1024;
    // DC-positive constant: bin 0 phase must be 0
    {
        std::vector<complex_f> X(static_cast<size_t>(N), complex_f(1.0f, 0.0f));
        fft(X, false);
        CHECK(std::fabs(std::arg(X[0])) < 1e-4f, "DC phase is 0");
    }
    // sin(2*pi*k*n/N) has DFT -i*N/2 at bin k: phase -pi/2
    {
        const int k = 64;
        std::vector<complex_f> X(static_cast<size_t>(N));
        for (int n = 0; n < N; ++n)
            X[static_cast<size_t>(n)] = complex_f(static_cast<float>(std::sin(2 * PI_D * k * n / N)), 0.0f);
        fft(X, false);
        CHECK(std::fabs(std::arg(X[static_cast<size_t>(k)]) + PI_D / 2) < 1e-3, "sine bin phase -pi/2");
    }
}

} // namespace

int main() {
    std::printf("S4 DSP accuracy: independent double-DFT oracle + analytic signals.\n");
    test_fft_vs_dft();
    test_roundtrip();
    test_known_signals();
    test_size_validation();
    test_stft_frames();
    test_windows();
    test_normalization();
    test_phase();
    std::printf("\nmax_abs_fft_error = %.3g\n", g_max_fft_err);
    std::printf("max_abs_roundtrip_error = %.3g\n", g_max_roundtrip_err);
    std::printf("=== dsp_accuracy: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
