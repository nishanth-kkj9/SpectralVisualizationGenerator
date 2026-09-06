#include "video_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace Spectral {

VideoEncoder::~VideoEncoder() {
    if (pipe_) close();
}

VideoEncoderError VideoEncoder::open(const std::string& output_path,
                                     const VideoEncoderConfig& cfg) {
    if (pipe_) close();
    cfg_ = cfg;
    frames_written_ = 0;

    // Build ffmpeg command
    std::ostringstream cmd;
    cmd << "ffmpeg -y"
        << " -f rawvideo"
        << " -pix_fmt " << cfg_.pixel_format
        << " -s " << cfg_.width << "x" << cfg_.height
        << " -r " << cfg_.fps
        << " -i -"                          // read from stdin
        << " -c:v " << cfg_.codec
        << " -crf " << cfg_.crf
        << " -pix_fmt yuv420p"              // output pixel format (compatible)
        << " " << cfg_.extra_args
        << " \"" << output_path << "\"";

    std::string cmd_str = cmd.str();
    pipe_ = _popen(cmd_str.c_str(), "wb");
    if (!pipe_) return VideoEncoderError::PipeFailure;
    return VideoEncoderError::Ok;
}

VideoEncoderError VideoEncoder::write_frame(const uint8_t* pixels,
                                            int64_t frame_index) {
    if (!pipe_) return VideoEncoderError::NotOpen;

    size_t frame_bytes = static_cast<size_t>(cfg_.width) * cfg_.height * 4;
    size_t written = fwrite(pixels, 1, frame_bytes, pipe_);
    if (written != frame_bytes) return VideoEncoderError::WriteFailure;

    frames_written_++;
    return VideoEncoderError::Ok;
}

VideoEncoderError VideoEncoder::close() {
    if (!pipe_) return VideoEncoderError::Ok;
    int rc = _pclose(pipe_);
    pipe_ = nullptr;
    return (rc == 0) ? VideoEncoderError::Ok : VideoEncoderError::CloseFailure;
}

} // namespace Spectral
