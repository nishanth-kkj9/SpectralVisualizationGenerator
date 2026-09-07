#include "video_encoder.h"

#include <cctype>
#include <sstream>

namespace Spectral {

namespace {

// Codec/pixel-format tokens must be plain ffmpeg names; anything else is
// rejected rather than passed to a command line (no shell exists anymore,
// but strict tokens also catch typos early with a clear error).
bool is_token(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char ch : s) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (!(std::isalnum(u) || ch == '_' || ch == '-' || ch == '.')) return false;
    }
    return true;
}

// extra_args stays a power feature (multiple flags), but it is split on
// whitespace into separate argv elements — each passed literally, so no
// embedded quotes/metacharacters can form new syntax.
std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

} // namespace

VideoEncoder::~VideoEncoder() {
    close();
}

bool VideoEncoder::is_open() const {
    return proc_.running();
}

bool VideoEncoder::ffmpeg_available() {
    SafeProcess probe;
    SafeProcess::Options opts;
    opts.capture_stdout = true;
    if (!probe.spawn(resolve_tool("ffmpeg"), {"-version"}, opts)) return false;
    return probe.wait() == 0;
}

VideoEncoderError VideoEncoder::open(const std::string& output_path,
                                     const VideoEncoderConfig& cfg) {
    close();
    tool_missing_ = false;
    if (output_path.empty() || cfg.width <= 0 || cfg.height <= 0) return VideoEncoderError::PipeFailure;
    if (cfg.fps <= 0 || cfg.fps > 120 || cfg.crf < 0 || cfg.crf > 51)
        return VideoEncoderError::PipeFailure;
    if (!is_token(cfg.codec) || !is_token(cfg.pixel_format)) return VideoEncoderError::PipeFailure;
    cfg_ = cfg;
    frames_written_ = 0;

    const std::string exe = resolve_tool("ffmpeg");
    std::vector<std::string> argv = {
        "-y", "-v", "error",
        "-f", "rawvideo", "-pix_fmt", cfg_.pixel_format,
        "-s", std::to_string(cfg_.width) + "x" + std::to_string(cfg_.height),
        "-r", std::to_string(cfg_.fps), "-i", "-",
        "-c:v", cfg_.codec, "-crf", std::to_string(cfg_.crf),
        "-pix_fmt", "yuv420p",
    };
    for (const auto& a : split_args(cfg_.extra_args)) argv.push_back(a);
    argv.push_back(output_path);

    SafeProcess::Options opts;
    opts.provide_stdin = true;
    opts.capture_stderr = true;
    if (!proc_.spawn(exe, argv, opts)) {
        // Argv is fixed and valid here; spawn failure means ffmpeg itself
        // could not start.
        tool_missing_ = true;
        return VideoEncoderError::PipeFailure;
    }
    return VideoEncoderError::Ok;
}

VideoEncoderError VideoEncoder::write_frame(const uint8_t* pixels,
                                            int64_t frame_index) {
    (void)frame_index;
    if (!proc_.running()) return VideoEncoderError::NotOpen;
    const size_t frame_bytes =
        static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * 4;
    if (!pixels || !proc_.write_stdin(pixels, frame_bytes)) return VideoEncoderError::WriteFailure;
    frames_written_++;
    return VideoEncoderError::Ok;
}

VideoEncoderError VideoEncoder::close() {
    if (!proc_.running() && proc_.exit_code() == -1) return VideoEncoderError::Ok;
    proc_.close_stdin();
    const int code = proc_.wait();
    if (code != 0) return VideoEncoderError::CloseFailure;
    return VideoEncoderError::Ok;
}

} // namespace Spectral
