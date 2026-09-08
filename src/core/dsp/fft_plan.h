#pragma once

// Phase 7 — reusable FFT execution architecture.
//
// The hot analysis path used to pay, per FFT: bit-reversal index work,
// one sin+cos pair per butterfly ((N/2)*log2(N) trig calls), and a fresh
// N-element complex scratch vector per STFT frame (plus two more per
// reassigned frame). This header splits that into:
//
//   FFTPlan      — immutable, size-specific, freely shareable execution
//                  data: the bit-reversal swap sequence and one twiddle
//                  table per stage, computed once.
//   FFTWorkspace — mutable, per-analysis-context reusable scratch: three
//                  N-element complex buffers (main spectrum + the two
//                  reassignment transforms). Owned by the caller (e.g. one
//                  per analyze_dataset call), never global, so concurrent
//                  analyses stay independent.
//
// NUMERICAL CONTRACT: the engine replays the exact algorithm of fft()
// (same butterfly function, same stage order, same swap sequence), and
// every tabled twiddle is computed with the identical ftwiddle(N, j,
// N/len) expression the loop used — so results are bit-identical to
// fft(), not merely close. fft()/stft_frame() keep their original
// implementations as the independent reference and compatibility path;
// production code uses this engine (see pipeline.cpp, multiband).

#include "fft.h"

#include <utility>
#include <vector>

namespace Spectral {

class FFTPlan {
public:
    FFTPlan() = default;

    // Precompute execution data for a power-of-two N >= 2. Anything else
    // yields an invalid plan (executors refuse it; no UB).
    static FFTPlan create(int n) {
        FFTPlan p;
        if (!is_valid_fft_size(n)) return p;
        p.n_ = n;
        // Same swap sequence fft_bit_reverse() performs, recorded once.
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) p.swaps_.emplace_back(i, j);
        }
        // One twiddle table per stage, each entry computed with the exact
        // expression the per-frame loop used: W_len^j = exp(-2pi*i*j/len).
        for (int len = 2; len <= n; len <<= 1) {
            std::vector<complex_f> t(static_cast<size_t>(len / 2));
            for (int j = 0; j < len / 2; ++j) t[static_cast<size_t>(j)] = ftwiddle(n, j, n / len);
            p.twiddles_.push_back(std::move(t));
            ++p.stages_;
        }
        return p;
    }

    bool valid() const { return n_ >= 2; }
    int size() const { return n_; }
    int stages() const { return stages_; }
    const std::vector<std::pair<int, int>>& swaps() const { return swaps_; }
    // Twiddle table for stage s (len = 2<<(s)): len/2 entries.
    const std::vector<complex_f>& twiddles(int stage) const { return twiddles_[stage]; }

private:
    int n_ = 0;
    int stages_ = 0;
    std::vector<std::pair<int, int>> swaps_;
    std::vector<std::vector<complex_f>> twiddles_;
};

class FFTWorkspace {
public:
    FFTWorkspace() = default;
    explicit FFTWorkspace(const FFTPlan& plan) { assign(plan); }

    // (Re)size the three scratch buffers for plan. Keeps existing storage
    // when the size already matches (the hot reuse path: no allocation).
    void assign(const FFTPlan& plan) {
        if (!plan.valid()) {
            n_ = 0;
            bufs_.clear();
            return;
        }
        if (n_ == plan.size() && bufs_.size() == 3) return;
        n_ = plan.size();
        bufs_.assign(3, std::vector<complex_f>(static_cast<size_t>(n_)));
    }

    bool valid() const { return n_ >= 2 && bufs_.size() == 3; }
    int size() const { return n_; }
    std::vector<complex_f>& buf(int i = 0) { return bufs_[i]; }
    const std::vector<complex_f>& buf(int i = 0) const { return bufs_[i]; }

private:
    int n_ = 0;
    std::vector<std::vector<complex_f>> bufs_;
};

// In-place forward transform of ws.buf(which). False (buffer untouched)
// when the plan/workspace is invalid, the sizes disagree, or which is
// out of range. Bit-identical to fft(x, false).
inline bool fft_forward(const FFTPlan& plan, FFTWorkspace& ws, int which = 0) {
    if (!plan.valid() || !ws.valid() || plan.size() != ws.size()) return false;
    if (which < 0 || which > 2) return false;
    const int n = plan.size();
    std::vector<complex_f>& x = ws.buf(which);
    if (static_cast<int>(x.size()) != n) return false;
    for (const auto& sw : plan.swaps()) std::swap(x[sw.first], x[sw.second]);
    int stage = 0;
    for (int len = 2; len <= n; len <<= 1, ++stage) {
        const std::vector<complex_f>& wtab = plan.twiddles(stage);
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < len / 2; ++j)
                fft_butterfly_std(x[i + j], x[i + j + len / 2], wtab[j]);
        }
    }
    return true;
}

// In-place inverse (conjugation method, scaled 1/N) of ws.buf(which).
// Same contract as fft_forward; bit-identical to fft(x, true).
inline bool fft_inverse(const FFTPlan& plan, FFTWorkspace& ws, int which = 0) {
    if (!plan.valid() || !ws.valid() || plan.size() != ws.size()) return false;
    if (which < 0 || which > 2) return false;
    const int n = plan.size();
    std::vector<complex_f>& x = ws.buf(which);
    if (static_cast<int>(x.size()) != n) return false;
    for (auto& v : x) v = std::conj(v);
    if (!fft_forward(plan, ws, which)) return false;
    const float inv_n = 1.0f / static_cast<float>(n);
    for (auto& v : x) v = std::conj(v) * inv_n;
    return true;
}

} // namespace Spectral
