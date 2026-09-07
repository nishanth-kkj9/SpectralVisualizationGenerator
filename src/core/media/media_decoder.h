#pragma once

// S3 — Streaming FFmpeg audio decoder.
// No shell, no temp files: ffmpeg is spawned via SafeProcess with an argv
// vector and its stdout PCM pipe is parsed in bounded chunks, so decode
// memory stays flat regardless of source duration.
//
// Channel policy: the decoder exposes the SOURCE layout natively
// (no -ac forcing). Downmix decisions belong to the analysis layer.
// Timestamps are double seconds on the stream time base.
// Counts from the probe are estimates until a clean EOF; see *_known().

#include "process/safe_process.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AudioFrame {
    std::vector<float> samples;  // interleaved float, chunk_frames * num_channels max
    int sample_rate = 0;
    int num_channels = 0;
    double timestamp = 0.0;  // chunk start, seconds (stream time base)
};

// Why open() failed. Bool open() stays source-compatible; inspect this
// for the precise cause instead of guessing from the message text.
// Phase 2: the pipeline maps each value to a distinct JobError.
enum class OpenStatus {
    Ok = 0,
    MissingInput,      // path does not exist / is not a regular file
    ToolMissing,       // ffprobe/ffmpeg executable could not start
    ProbeFailed,       // ffprobe ran but the file is unreadable
    NoAudioStream,     // valid probe, no usable audio stream
    DecoderStartFailed,  // ffmpeg executable could not start for decode
};

class MediaDecoder {
public:
    struct DecodeOptions {
        int target_rate = 0;       // 0 = native source rate; else libswresample -ar
        size_t chunk_frames = 4096;  // bounded PCM chunk per read_frame
    };

    virtual ~MediaDecoder() = default;

    // Probe + select audio stream + spawn decoder. False = usable last_error()
    // plus a machine-readable open_status().
    bool open(const std::string& filepath, const DecodeOptions& opts = DecodeOptions{});
    OpenStatus open_status() const { return open_status_; }

    void close();

    // Next bounded chunk. False at clean EOF or on error (see failed()).
    bool read_frame(AudioFrame& frame);

    // Effective rate: native probe rate, or DecodeOptions::target_rate once open.
    int sample_rate() const { return format_open_ ? effective_rate_ : sample_rate_; }
    int num_channels() const { return channel_count_; }
    int stream_index() const { return audio_stream_idx_; }
    std::string codec_name() const { return codec_; }

    // Stream start offset, seconds (0 when unknown).
    double start_time() const { return start_time_; }

    // Probe duration, seconds. known=false when the container hid it.
    double duration() const { return duration_sec_; }
    bool duration_known() const { return duration_known_; }

    // Frame estimate (duration*rate) until a clean EOF, then exact.
    int64_t total_frames() const;
    bool total_frames_known() const;

    // True if the decode process failed (nonzero exit, truncated/misaligned
    // output). Delivered chunks stay valid; the caller must not treat a
    // failed decode as a complete result.
    bool failed() const { return failed_; }
    std::string last_error() const { return last_error_; }

    // Resolved tool paths (override -> env -> PATH), for diagnostics.
    std::string ffmpeg_path() const { return ffmpeg_path_; }
    std::string ffprobe_path() const { return ffprobe_path_; }

private:
    bool probe(const std::string& filepath);

    std::string filepath_;
    DecodeOptions opts_;
    OpenStatus open_status_ = OpenStatus::Ok;
    bool format_open_ = false;
    int audio_stream_idx_ = -1;
    std::string codec_;
    int sample_rate_ = 0;
    int channel_count_ = 0;
    double start_time_ = 0.0;
    double duration_sec_ = 0.0;
    bool duration_known_ = false;
    int64_t delivered_frames_ = 0;
    bool eof_seen_ = false;
    bool eof_clean_ = false;
    bool failed_ = false;
    std::string last_error_;
    std::string ffmpeg_path_;
    std::string ffprobe_path_;
    std::vector<uint8_t> staging_;
    int effective_rate_ = 0;
    Spectral::SafeProcess proc_;
};
