#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <numeric>

using complex_f = std::complex<float>;

// Pi constant
const float PI = static_cast<float>(std::acos(-1.0));

// ---------------------------------------------------------------------
// Radix-2 Cooley-Tukey FFT (in-place, complex)
// ---------------------------------------------------------------------
// CONTRACT (see docs/dsp/fft-stft.md for the full statement):
// - Representation: std::complex<float> vectors, float arithmetic.
// - Forward: X[k] = sum_{n=0}^{N-1} x[n] * exp(-2j*pi*k*n/N).
// - Inverse: conjugation method (conjugate, forward, conjugate, scale
//   1/N), so IFFT(FFT(x)) == x. No normalization is applied inside fft:
//   callers scale explicitly (STFT divides by N, coherent gain, one-sided
//   doubling — see stft.h).
// - Sizes: power of two, N >= 2 (see is_valid_fft_size). Anything else is
//   left untouched by fft(); use fft_checked() for an explicit verdict.
// - Bins: bin 0 = DC (sum), bin N/2 = Nyquist (real for real input).
//   Real-input symmetry X[N-k] == conj(X[k]) holds up to float error.

// Bit-reverse permutation
static void fft_bit_reverse(std::vector<complex_f>& x) {
    int N = static_cast<int>(x.size());
    for (int i = 1, j = 0; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
}

// Twiddle factor butterflies
static complex_f ftwiddle(int N, int k, int n) {
    float angle = -2.0f * PI * static_cast<float>(k * n) / static_cast<float>(N);
    return complex_f(std::cos(angle), std::sin(angle));
}

// The one authoritative butterfly: t = w*b; u = a; a = u+t; b = u-t.
// (Two earlier broken variants lived here; removed in S4. The tests in
// tests/dsp/test_dsp_accuracy.cpp pin this against an independent DFT.)
static void fft_butterfly_std(complex_f& a, complex_f& b, const complex_f& w) {
    complex_f t = w * b;
    complex_f u = a;
    a = u + t;
    b = u - t;
}

// Power-of-two contract: N >= 2. (N == 1 is a degenerate length the
// STFT never requests; it is rejected so callers notice bad config.)
inline bool is_valid_fft_size(int N) {
    return N >= 2 && (N & (N - 1)) == 0;
}

// Main FFT function.
// inverse == false: forward transform (see contract above).
// inverse == true: mathematical inverse via conjugation, scaled 1/N, so
// IFFT(FFT(x)) == x up to float rounding.
// Invalid sizes are left untouched; use fft_checked for an explicit verdict.
inline void fft(std::vector<complex_f>& x, bool inverse = false) {
    int N = static_cast<int>(x.size());

    if (!is_valid_fft_size(N)) return;
    if (inverse) {
        for (auto& v : x) v = std::conj(v);
    }

    // Bit-reversal permutation
    fft_bit_reverse(x);

    // Cooley-Tukey butterfly stages
    for (int len = 2; len <= N; len <<= 1) {
        for (int i = 0; i < N; i += len) {
            for (int j = 0; j < len / 2; ++j) {
                complex_f w = ftwiddle(N, j, N / len);  // twiddle: W_len^j = exp(-2πi·j/len)
                fft_butterfly_std(x[i + j], x[i + j + len / 2], w);
            }
        }
    }

    if (inverse) {
        float invN = 1.0f / static_cast<float>(N);
        for (auto& v : x) v = std::conj(v) * invN;
    }
}

// Checked entry point: false (input untouched) on invalid sizes.
inline bool fft_checked(std::vector<complex_f>& x, bool inverse = false) {
    if (!is_valid_fft_size(static_cast<int>(x.size()))) return false;
    fft(x, inverse);
    return true;
}

// ---------------------------------------------------------------------
// FFT utility: convert float real/imag to magnitude
// ---------------------------------------------------------------------

// Magnitude of spectrum (already normalized by FFT plan)
static std::vector<float> fft_magnitude(const std::vector<complex_f>& X) {
    int N = static_cast<int>(X.size());
    std::vector<float> mag(N);
    for (int k = 0; k < N; ++k) {
        mag[k] = static_cast<float>(std::sqrt(X[k].real() * X[k].real() + X[k].imag() * X[k].imag()));
    }
    return mag;
}

// Power spectrum (magnitude squared)
static std::vector<float> fft_power(const std::vector<complex_f>& X) {
    int N = static_cast<int>(X.size());
    std::vector<float> pow(N);
    for (int k = 0; k < N; ++k) {
        float mag = static_cast<float>(std::sqrt(X[k].real() * X[k].real() + X[k].imag() * X[k].imag()));
        pow[k] = mag * mag;
    }
    return pow;
}

// Combined magnitude+power in a single pass. Avoids redundant sqrt.
// ponytail: returns {mag, power} to skip a second alloc+loop.
static std::pair<std::vector<float>, std::vector<float>>
fft_magnitude_power(const std::vector<complex_f>& X) {
    int N = static_cast<int>(X.size());
    std::vector<float> mag(N);
    std::vector<float> pow(N);
    for (int k = 0; k < N; ++k) {
        float re = X[k].real();
        float im = X[k].imag();
        float m = static_cast<float>(std::sqrt(re * re + im * im));
        mag[k] = m;
        pow[k] = m * m;
    }
    return {std::move(mag), std::move(pow)};
}

// ---------------------------------------------------------------------
// Frequency / time bin calculations
// ---------------------------------------------------------------------

// Frequency of bin k given FFT size N and sample rate sr
static float freq_from_bin(int k, int N, int sr) {
    return static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(N);
}

// Bin index for a given frequency
static int bin_from_freq(float freq, int N, int sr) {
    return static_cast<int>(std::round(static_cast<float>(freq) * static_cast<float>(N) / static_cast<float>(sr)));
}

// Time resolution: duration of one sample at given sample rate
static float sample_duration(int sr) {
    return 1.0f / static_cast<float>(sr);
}

// ---------------------------------------------------------------------
// Window function generators (return std::vector<float> of length N)
// ---------------------------------------------------------------------

// Rectangular window: w[n] = 1 for all n
static std::vector<float> window_rectangular(int N) {
    return std::vector<float>(N, 1.0f);
}

// Hann window: w[n] = 0.5 * (1 - cos(2*pi*n/(N-1))), n = 0..N-1
static std::vector<float> window_hann(int N) {
    std::vector<float> w(N);
    if (N <= 1) { w.assign(N, 1.0f); return w; }
    for (int n = 0; n < N; ++n) {
        w[n] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(N - 1)));
    }
    return w;
}

// Hamming window: w[n] = 0.54 - 0.46*cos(2*pi*n/(N-1)), n = 0..N-1
static std::vector<float> window_hamming(int N) {
    std::vector<float> w(N);
    if (N <= 1) { w.assign(N, 1.0f); return w; }
    for (int n = 0; n < N; ++n) {
        w[n] = 0.54f - 0.46f * std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(N - 1));
    }
    return w;
}

// Blackman window: w[n] = 0.42 - 0.5*cos(2*pi*n/(N-1)) + 0.08*cos(4*pi*n/(N-1))
static std::vector<float> window_blackman(int N) {
    std::vector<float> w(N);
    if (N <= 1) { w.assign(N, 1.0f); return w; }
    for (int n = 0; n < N; ++n) {
        w[n] = 0.42f - 0.5f * std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(N - 1))
            + 0.08f * std::cos(4.0f * PI * static_cast<float>(n) / static_cast<float>(N - 1));
    }
    return w;
}

// ---------------------------------------------------------------------
// Coherent gain of a window
// ---------------------------------------------------------------------

// Coherent gain: sum(w) / N — measures amplitude preservation
static float window_coherent_gain(const std::vector<float>& w) {
    int N = static_cast<int>(w.size());
    if (N <= 0) return 0.0f;
    float sum = std::accumulate(w.begin(), w.end(), 0.0f);
    return sum / static_cast<float>(N);
}

// ---------------------------------------------------------------------
// dB conversion
// ---------------------------------------------------------------------

// Power-to-dB: 10 * log10(p / ref), with floor at -90 dB (or ref floor).
// NaN and non-positive inputs map to the floor (never NaN out); +Inf
// passes through and is clamped to the display ceiling by renderers.
static float power_to_db(float power, float ref = 1.0f, float floor_db = -90.0f) {
    if (!(power > 0.0f)) return floor_db;
    float val = 10.0f * std::log10(power / ref);
    if (val < floor_db) return floor_db;
    return static_cast<float>(val);
}

// Magnitude-to-dB: 20 * log10(mag / ref), with floor.
// Same NaN contract as power_to_db. Consistent by construction:
// magnitude_to_db(m) == power_to_db(m*m) up to float rounding.
static float magnitude_to_db(float mag, float ref = 1.0f, float floor_db = -90.0f) {
    if (!(mag > 0.0f)) return floor_db;
    float val = 20.0f * std::log10(mag / ref);
    if (val < floor_db) return floor_db;
    return static_cast<float>(val);
}
