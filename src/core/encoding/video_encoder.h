#pragma once

// Phase 10 — Video encoder via ffmpeg subprocess.
// Pipes raw RGBA frames to ffmpeg stdin for encoding.

#include "process/safe_process.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

struct VideoEncoderConfig {
    int width = 1024;
    int height = 512;
    int fps = 30;
    std::string codec = "libx264";    // libx264, libx265, libvpx-vp9
    int crf = 18;                     // 0-51, lower = better quality
    std::string pixel_format = "rgba"; // input pixel format
    std::string extra_args = "";       // additional ffmpeg args
};

enum class VideoEncoderError {
    Ok = 0,
    PipeFailure,
    WriteFailure,
    NotOpen,
    CloseFailure,
};

class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    // Non-copyable
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Open ffmpeg subprocess and begin encoding to output_path.
    VideoEncoderError open(const std::string& output_path,
                           const VideoEncoderConfig& cfg);

    // Write a single RGBA frame. pixels must be width*height*4 bytes.
    VideoEncoderError write_frame(const uint8_t* pixels, int64_t frame_index);

    // Finalize and close the ffmpeg process.
    VideoEncoderError close();

    bool is_open() const;

private:
    SafeProcess proc_;
    VideoEncoderConfig cfg_;
    int64_t frames_written_ = 0;
};

} // namespace Spectral
