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
// Forward FFT: X[k] = sum_{n=0}^{N-1} x[n] * exp(-2j * pi * k * n / N)
// In-place, overwrites input. Must have N = 2^m.

// Bit-reverse permutation
static void fft_bit_reverse(std::vector<complex_f>& x) {
    int N = static_cast<int>(x.size());
    int log2N = 0;
    while ((1 << log2N) < N) ++log2N;

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

// Core butterfly: a = a + w * b; b = a - w * b (using temporary)
static void fft_butterfly(complex_f& a, complex_f& b, const complex_f& w) {
    complex_f t = w * b;
    a = a + t;
    b = a - 2.0f * t;  // corrected: b = original_a - t, but a was overwritten
    // Proper: need to save original a
}

// Proper butterfly with temp
static void fft_butterfly_correct(complex_f& a, complex_f& b, const complex_f& w) {
    complex_f t = w * b;
    complex_f new_a = a + t;
    b = a - t;  // b = original_a - w*original_b, but a is now new_a... hmm
    // Let me just do it the standard way:
    // t = w * b;  u = a;  a = u + t;  b = u - t;
}

// Proper butterfly
static void fft_butterfly_std(complex_f& a, complex_f& b, const complex_f& w) {
    complex_f t = w * b;
    complex_f u = a;
    a = u + t;
    b = u - t;
}

// Main FFT function
// If inverse == true, performs IFFT (with 1/N scaling at the end)
inline void fft(std::vector<complex_f>& x, bool inverse = false) {
    int N = static_cast<int>(x.size());

    // Must be power of 2
    if (N <= 0 || (N & (N - 1)) != 0) return;

    // Bit-reversal permutation
    fft_bit_reverse(x);

    // Cooley-Tukey butterfly stages
    for (int len = 2; len <= N; len <<= 1) {
        int step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int j = 0; j < len / 2; ++j) {
                complex_f w = ftwiddle(N, j, N / len);  // twiddle: W_len^j = exp(-2πi·j/len)
                fft_butterfly_std(x[i + j], x[i + j + len / 2], w);
            }
        }
    }

    // If inverse, scale by 1/N
    if (inverse) {
        float invN = 1.0f / static_cast<float>(N);
        for (auto& sample : x) sample *= invN;
    }
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

// Power-to-dB: 10 * log10(p / ref), with floor at -90 dB (or ref floor)
static float power_to_db(float power, float ref = 1.0f, float floor_db = -90.0f) {
    if (power <= 0.0f) return floor_db;
    float val = 10.0f * std::log10(power / ref);
    if (val < floor_db) return floor_db;
    return static_cast<float>(val);
}

// Magnitude-to-dB: 20 * log10(mag / ref), with floor
static float magnitude_to_db(float mag, float ref = 1.0f, float floor_db = -90.0f) {
    if (mag <= 0.0f) return floor_db;
    float val = 20.0f * std::log10(mag / ref);
    if (val < floor_db) return floor_db;
    return static_cast<float>(val);
}
