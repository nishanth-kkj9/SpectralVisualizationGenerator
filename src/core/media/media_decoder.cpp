#include "media_decoder.h"

#include "process/safe_process.h"

#include <cmath>
#include <filesystem>
#include <sstream>

// S3 — streaming decode. ffmpeg stdout (s16le PCM pipe) is parsed in
// bounded chunks; at most one chunk + one pipe buffer is ever in RAM.

namespace {

constexpr size_t kPipeReadBytes = 65536;

struct StreamInfo {
    int index = -1;
    std::string codec_type;
    std::string codec;
    int sample_rate = 0;
    int channels = 0;
    double duration = 0.0;
    bool has_duration = false;
    double start = 0.0;
};

// Minimal parser for ffprobe flat output (order-independent key=value,
// no JSON dependency). Lines look like:
//   streams.stream.0.codec_type="audio"
static std::string unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        return v.substr(1, v.size() - 2);
    return v;
}

bool parse_stream_flat(const std::string& out, std::vector<StreamInfo>& streams) {
    const std::string pre = "streams.stream.";
    std::istringstream lines(out);
    std::string line;
    auto ensure = [&](int idx) -> StreamInfo& {
        while (static_cast<int>(streams.size()) <= idx) streams.push_back(StreamInfo{});
        streams[static_cast<size_t>(idx)].index = idx;
        return streams[static_cast<size_t>(idx)];
    };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, pre.size(), pre) != 0) continue;
        size_t dot = line.find('.', pre.size());
        size_t eq = line.find('=', dot == std::string::npos ? 0 : dot);
        if (dot == std::string::npos || eq == std::string::npos) continue;
        int idx = -1;
        try {
            idx = std::stoi(line.substr(pre.size(), dot - pre.size()));
        } catch (...) {
            continue;
        }
        if (idx < 0 || idx > 64) continue;
        std::string key = line.substr(dot + 1, eq - dot - 1);
        std::string val = unquote(line.substr(eq + 1));
        StreamInfo& si = ensure(idx);
        // Nested keys (e.g. disposition.default) land here whole; only
        // top-level stream attributes are consumed.
        if (key == "codec_type") si.codec_type = val;
        else if (key == "codec_name") si.codec = val;
        else if (key == "sample_rate") {
            try { si.sample_rate = std::stoi(val); } catch (...) {}
        } else if (key == "channels") {
            try { si.channels = std::stoi(val); } catch (...) {}
        } else if (key == "duration" && val != "N/A") {
            try {
                si.duration = std::stod(val);
                si.has_duration = std::isfinite(si.duration) && si.duration > 0.0;
            } catch (...) {
            }
        } else if (key == "start_time" && val != "N/A") {
            try {
                si.start = std::stod(val);
                if (!std::isfinite(si.start)) si.start = 0.0;
            } catch (...) {
            }
        }
    }
    return true;
}

bool run_capture(Spectral::SafeProcess& proc, const std::string& exe,
                 const std::vector<std::string>& argv, std::string& out) {
    Spectral::SafeProcess::Options opts;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    if (!proc.spawn(exe, argv, opts)) return false;
    std::vector<uint8_t> buf(65536);
    for (;;) {
        size_t n = proc.read_stdout(buf.data(), buf.size());
        if (n == static_cast<size_t>(-1)) return false;
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    return proc.wait() == 0;
}

} // namespace

bool MediaDecoder::open(const std::string& filepath, const DecodeOptions& opts) {
    close();
    open_status_ = OpenStatus::Ok;
    filepath_ = filepath;
    opts_ = opts;
    if (opts_.chunk_frames == 0) opts_.chunk_frames = 4096;
    // Existence first: a missing path is MissingInput even if the tools
    // are also absent. No subprocess is launched for a missing file.
    std::error_code fec;
    if (!std::filesystem::is_regular_file(filepath, fec) || fec) {
        open_status_ = OpenStatus::MissingInput;
        last_error_ = "media: input not found '" + filepath + "'";
        return false;
    }
    if (!probe(filepath)) return false;  // status set inside probe()

    ffmpeg_path_ = Spectral::resolve_tool("ffmpeg");
    const int out_rate = opts_.target_rate > 0 ? opts_.target_rate : sample_rate_;
    // Global stream index (0:N), NOT the audio-relative ordinal:
    // 0:a:1 would mean the *second* audio stream.
    std::vector<std::string> argv = {
        "-v", "error", "-i", filepath_,
        "-map", "0:" + std::to_string(audio_stream_idx_),
    };
    if (opts_.target_rate > 0) {
        argv.push_back("-ar");
        argv.push_back(std::to_string(opts_.target_rate));
    }
    argv.insert(argv.end(), {"-f", "s16le", "-acodec", "pcm_s16le", "-vn", "-"});

    Spectral::SafeProcess::Options popts;
    popts.capture_stdout = true;
    popts.capture_stderr = true;
    if (!proc_.spawn(ffmpeg_path_, argv, popts)) {
        // Argv here is fixed and valid; spawn failure means the ffmpeg
        // executable itself could not start. close() resets diagnostics,
        // so preserve both across it.
        const std::string why =
            "media: cannot start decoder (" + proc_.last_error() + ")";
        close();
        open_status_ = OpenStatus::DecoderStartFailed;
        last_error_ = why;
        return false;
    }
    effective_rate_ = out_rate;
    format_open_ = true;
    return true;
}

bool MediaDecoder::probe(const std::string& filepath) {
    ffprobe_path_ = Spectral::resolve_tool("ffprobe");
    Spectral::SafeProcess proc;
    std::vector<std::string> argv = {
        "-v", "error",
        "-show_entries", "stream=index,codec_type,codec_name,sample_rate,channels,duration,start_time",
        "-of", "flat", filepath,
    };
    std::string out;
    if (!run_capture(proc, ffprobe_path_, argv, out)) {
        // Spawn never ran (exit code untouched at -1) => the ffprobe
        // executable itself is missing. Otherwise ffprobe ran and the
        // file is unreadable.
        if (out.empty() && proc.exit_code() == -1) {
            open_status_ = OpenStatus::ToolMissing;
            last_error_ = "media: cannot start ffprobe (" +
                          proc.last_error() + ")";
        } else {
            open_status_ = OpenStatus::ProbeFailed;
            last_error_ = "media: ffprobe exited nonzero for '" + filepath +
                          "'; stderr: " + proc.stderr_text();
        }
        return false;
    }
    // Deterministic policy: lowest-index audio stream wins. Documented in
    // docs/media-ingestion.md; multi-stream files report their index.
    std::vector<StreamInfo> streams;
    parse_stream_flat(out, streams);
    bool found = false;
    StreamInfo best;
    for (const auto& si : streams) {
        if (si.codec_type != "audio") continue;
        if (si.sample_rate <= 0 || si.channels <= 0) continue;
        if (!found || si.index < best.index) {
            best = si;
            found = true;
        }
    }
    if (!found) {
        open_status_ = OpenStatus::NoAudioStream;
        last_error_ = "media: no audio stream in '" + filepath + "'";
        return false;
    }
    audio_stream_idx_ = best.index;
    codec_ = best.codec;
    sample_rate_ = best.sample_rate;
    channel_count_ = best.channels;
    start_time_ = best.start;
    if (best.has_duration) {
        duration_sec_ = best.duration;
        duration_known_ = true;
    } else {
        // Fall back to container duration before giving up on timing.
        Spectral::SafeProcess fproc;
        std::vector<std::string> fargv = {"-v", "error",
                                          "-show_entries", "format=duration",
                                          "-of", "flat", filepath};
        std::string fout;
        if (run_capture(fproc, ffprobe_path_, fargv, fout)) {
            const std::string key = "format.duration=";
            size_t pos = fout.find(key);
            if (pos != std::string::npos) {
                std::string val = fout.substr(pos + key.size());
                size_t end = val.find_first_of("\r\n");
                if (end != std::string::npos) val.resize(end);
                // strip quotes
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                    val = val.substr(1, val.size() - 2);
                try {
                    double d = std::stod(val);
                    if (std::isfinite(d) && d > 0.0) {
                        duration_sec_ = d;
                        duration_known_ = true;
                    }
                } catch (...) {
                }
            }
        }
    }
    return true;
}

void MediaDecoder::close() {
    proc_.kill();
    open_status_ = OpenStatus::Ok;
    filepath_.clear();
    format_open_ = false;
    audio_stream_idx_ = -1;
    codec_.clear();
    sample_rate_ = 0;
    channel_count_ = 0;
    effective_rate_ = 0;
    start_time_ = 0.0;
    duration_sec_ = 0.0;
    duration_known_ = false;
    delivered_frames_ = 0;
    eof_seen_ = false;
    eof_clean_ = false;
    failed_ = false;
    cancelled_ = false;
    last_error_.clear();
    staging_.clear();
}

bool MediaDecoder::read_frame(AudioFrame& frame) {
    frame = AudioFrame{};
    if (!format_open_ || failed_ || eof_seen_ || cancelled_) return false;
    // A preset request aborts before touching the pipe: no chunk is
    // delivered, the child is terminated, callers see cancelled().
    if (cancel_ && cancel_->load(std::memory_order_acquire)) {
        proc_.kill();
        cancelled_ = true;
        last_error_ = "media: decode cancelled for '" + filepath_ + "'";
        return false;
    }

    const size_t want_frames = opts_.chunk_frames;
    const size_t want_bytes = want_frames * static_cast<size_t>(channel_count_) * 2;
    std::vector<uint8_t> buf(65536);
    bool eof = false;
    while (staging_.size() < want_bytes && !eof) {
        bool was_cancelled = false;
        size_t n = proc_.read_stdout_cancelable(buf.data(), buf.size(), cancel_,
                                               was_cancelled);
        if (was_cancelled) {
            // The child keeps running after a poll abort: terminate it so
            // no ffmpeg survives cancellation, then report. Staged bytes
            // are discarded; delivered chunks stay valid.
            proc_.kill();
            staging_.clear();
            cancelled_ = true;
            last_error_ = "media: decode cancelled for '" + filepath_ + "'";
            return false;
        }
        if (n == static_cast<size_t>(-1)) {
            failed_ = true;
            last_error_ = "media: decode pipe error for '" + filepath_ + "'";
            return false;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        staging_.insert(staging_.end(), buf.begin(), buf.begin() + n);
    }
    if (eof) {
        eof_seen_ = true;
        int code = proc_.wait();
        if (code != 0) {
            failed_ = true;
            std::string tail = proc_.stderr_text();
            if (tail.size() > 500) tail = tail.substr(tail.size() - 500);
            last_error_ = "media: ffmpeg exited with code " + std::to_string(code) +
                          " for '" + filepath_ + "'; stderr: " + tail;
        } else {
            eof_clean_ = true;
        }
        // Misaligned tail (not a whole sample) is dropped, never fabricated.
        size_t complete = (staging_.size() / 2) * 2;
        if (staging_.size() != complete) {
            staging_.resize(complete);
            if (code == 0)
                last_error_ = "media: dropped truncated trailing sample";
        }
    }
    const size_t frame_bytes = 2 * static_cast<size_t>(channel_count_);
    size_t have_frames = frame_bytes ? staging_.size() / frame_bytes : 0;
    if (have_frames > want_frames) have_frames = want_frames;
    if (have_frames == 0) {
        // Clean EOF with nothing left, or a failed decode: no more data.
        // A nonzero exit with zero delivered samples is a hard failure
        // (failed_ already set above); clean EOF just ends the stream.
        return false;
    }
    frame.sample_rate = effective_rate_;
    frame.num_channels = channel_count_;
    frame.timestamp = start_time_ + static_cast<double>(delivered_frames_) /
                                        static_cast<double>(effective_rate_ > 0 ? effective_rate_ : 1);
    frame.samples.resize(have_frames * static_cast<size_t>(channel_count_));
    const uint8_t* raw = staging_.data();
    for (size_t i = 0; i < have_frames * static_cast<size_t>(channel_count_); ++i) {
        int sval = static_cast<int>(raw[2 * i]) | (static_cast<int>(raw[2 * i + 1]) << 8);
        if (sval >= 32768) sval -= 65536;
        float fval = static_cast<float>(sval) / 32768.0f;
        if (fval > 1.0f) fval = 1.0f;
        if (fval < -1.0f) fval = -1.0f;
        frame.samples[i] = fval;
    }
    staging_.erase(staging_.begin(),
                   staging_.begin() + have_frames * frame_bytes);
    delivered_frames_ += static_cast<int64_t>(have_frames);
    return true;
}

int64_t MediaDecoder::total_frames() const {
    if (eof_clean_) return delivered_frames_;
    if (duration_known_ && effective_rate_ > 0)
        return static_cast<int64_t>(duration_sec_ * effective_rate_);
    return delivered_frames_;
}

bool MediaDecoder::total_frames_known() const {
    return eof_clean_;
}
