#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct AudioFrame {
    std::vector<float> samples;  // interleaved, always float
    int sample_rate;
    int num_channels;
    int64_t timestamp;  // in seconds
};

// Media decoder using FFmpeg subprocess (ffprobe/ffmpeg).
// No direct FFmpeg library linking; runs ffmpeg/ffprobe as external processes.
// Binary paths can be overridden via FFMPEG_BINARY and FFPROBE_BINARY env vars.
class MediaDecoder {
public:
    virtual ~MediaDecoder() = default;

    // Open a media file. Returns true on success.
    bool open(const std::string& filepath);

    // Close and release resources.
    void close();

    // Read the next audio frame. Returns false at EOF or error.
    bool read_frame(AudioFrame& frame);

    // Get total number of frames in the file.
    int64_t total_frames() const;

    // Get duration in seconds.
    double duration() const;

    // Get sample rate.
    int sample_rate() const;

    // Get number of channels.
    int num_channels() const;

private:
    std::string filepath_;
    bool format_open_ = false;
    int audio_stream_idx_ = -1;
    int sample_rate_ = 0;
    int channel_count_ = 2;
    int frame_count_ = 0;
    double duration_sec_ = 0.0;
};