#pragma once

// Phase 6 — bounded overlap-aware mono buffer for streaming STFT.
//
// Given FFT size N and hop H<=N, the analyzer needs N contiguous mono
// samples per frame. After a frame is consumed the buffer advances by H
// and retains only the N-H overlap samples for the next frame, so the
// raw-audio working set stays bounded by ~N + one decoder chunk instead
// of growing with source duration. (The SpectralDataset itself still
// stores one entry per spectral frame by design.)
//
// Audio-domain only: no dataset, renderer, or config knowledge. The
// pipeline feeds mono chunks and pops complete frames; chunk boundaries
// never affect frame contents (frames are slices of one logical stream).
//
// Lifetime rule: a popped span stays valid until the next append() call.
// Retention trimming happens inside append() (never inside pop_frame),
// so consuming a frame can never invalidate the span being consumed.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Spectral {

class StreamingMonoBuffer {
public:
    StreamingMonoBuffer(int fft_size, int hop_size)
        : fft_(fft_size), hop_(hop_size),
          valid_(fft_size > 0 && hop_size > 0 && hop_size <= fft_size) {
        if (valid_) buf_.reserve(static_cast<size_t>(fft_size) * 2);
    }

    bool valid() const { return valid_; }

    // Append decoded mono samples (absolute order). First discards samples
    // that precede the retained overlap window, then inserts. Peak tracks
    // the largest live size for tests.
    void append(const float* mono, size_t n) {
        if (!valid_) return;
        // Retain only overlap needed before next_start_: everything older
        // is unreachable by any future frame.
        const int64_t keep_from = next_start_ - (fft_ - hop_);
        const int64_t drop = keep_from - base_;
        if (drop > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<size_t>(drop));
            base_ += drop;
        }
        if (mono && n > 0) {
            buf_.insert(buf_.end(), mono, mono + n);
            delivered_ += static_cast<int64_t>(n);
        }
        if (buf_.size() > peak_) peak_ = buf_.size();
    }

    // Pop the next complete frame when [next_start, next_start+fft) is
    // fully buffered. span points into internal storage and stays valid
    // until the next append() call (this function itself never mutates
    // storage). Returns false when more input is needed (caller appends,
    // or stops at decoder EOF).
    bool pop_frame(const float*& span, int64_t& abs_start) {
        if (!valid_) return false;
        if (next_start_ + fft_ > delivered_) return false;
        const int64_t off = next_start_ - base_;
        span = buf_.data() + off;
        abs_start = next_start_;
        next_start_ += hop_;
        return true;
    }

    int64_t delivered() const { return delivered_; }
    int64_t next_start() const { return next_start_; }
    size_t buffered() const { return buf_.size(); }
    size_t peak_buffered() const { return peak_; }

private:
    int fft_ = 0;
    int hop_ = 0;
    bool valid_ = false;
    std::vector<float> buf_;  // buf_[0] is absolute sample base_
    int64_t base_ = 0;        // absolute index of buf_[0]
    int64_t next_start_ = 0;  // absolute index of next frame start
    int64_t delivered_ = 0;   // total samples appended ever
    size_t peak_ = 0;         // max buf_.size() observed
};

} // namespace Spectral
