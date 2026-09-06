#include "media_decoder.h"

#include <vector>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <thread>

// Helper: run a command via _popen and capture stdout (Windows)
static std::string run_popen(const std::vector<std::string>& cmd, int /*timeout_sec*/ = 30) {
    std::string result;

    // Build command line string
    std::string cmdline;
    for (const auto& arg : cmd) {
        if (arg.find(' ') != std::string::npos ||
            arg.find('"') != std::string::npos ||
            arg.find('&') != std::string::npos) {
            cmdline += "\"";
            for (const auto& ch : arg) {
                if (ch == '"') cmdline += "\"";
                else cmdline += ch;
            }
            cmdline += "\" ";
        }
        else {
            cmdline += arg + " ";
        }
    }
    if (!cmdline.empty()) cmdline.pop_back();

    FILE* pipe = _popen(cmdline.c_str(), "r");
    if (!pipe) { return result; }

    char buf[4096];
    while (fgets(buf, (int)sizeof(buf), pipe) != nullptr) {
        result += buf;
    }
    _pclose(pipe);
    return result;
}

// FFmpeg probe wrapper (subprocess via _popen)
static std::string ffprobe(const std::string& filepath,
    const std::string& arg,
    const std::string& binary = "ffprobe") {
    std::vector<std::string> cmd = { binary, "-v", "quiet", "-print_format", "json",
        "-show_entries", arg, filepath };
    return run_popen(cmd);
}

// FFmpeg decode wrapper (subprocess via _popen)
static std::string ffmpeg(const std::string& filepath,
    const std::string& args,
    const std::string& binary = "ffmpeg") {
    std::vector<std::string> cmd = { binary, "-v", "quiet", "-i", filepath, args };
    return run_popen(cmd);
}

// ====================================================================
// MediaDecoder implementation: FFmpeg subprocess-based
// ====================================================================

bool MediaDecoder::open(const std::string& filepath) {
    filepath_ = filepath;
    format_open_ = false;
    audio_stream_idx_ = -1;
    sample_rate_ = 0;
    channel_count_ = 2;
    frame_count_ = 0;
    duration_sec_ = 0.0;

    // Use ffprobe to check file and find audio streams
    std::string probe = ffprobe(filepath, "streams");
    if (probe.empty()) { return false; }

    // Look for audio stream marker (ffprobe outputs with or without spaces around colon)
    size_t amarker = probe.find("\"codec_type\"");
    if (amarker == std::string::npos) { return false; }
    size_t amarker_val = probe.find("audio", amarker);
    if (amarker_val == std::string::npos) { return false; }  // No audio stream

    // Look for duration
    size_t dpos = probe.find("\"duration\"");
    if (dpos != std::string::npos) {
        size_t colon = probe.find(':', dpos);
        size_t vpos = probe.find('"', colon + 1);
        size_t vpos2 = probe.find('"', vpos + 1);
        if (vpos2 != std::string::npos) {
            std::string dur_str = probe.substr(vpos + 1, vpos2 - vpos - 1);
            try { duration_sec_ = std::stod(dur_str); } catch (...) {}
        }
    }

    // Look for sample rate
    size_t spos = probe.find("\"sample_rate\"");
    if (spos != std::string::npos) {
        size_t colon = probe.find(':', spos);
        size_t vpos = probe.find('"', colon + 1);
        size_t vpos2 = probe.find('"', vpos + 1);
        if (vpos2 != std::string::npos) {
            std::string sr_str = probe.substr(vpos + 1, vpos2 - vpos - 1);
            try { sample_rate_ = std::stoi(sr_str); } catch (...) {}
        }
    }

    // Audio stream confirmed by ffprobe
    audio_stream_idx_ = 0;
    format_open_ = true;
    return true;
}

void MediaDecoder::close() {
    format_open_ = false;
    audio_stream_idx_ = -1;
    sample_rate_ = 0;
    channel_count_ = 2;
    frame_count_ = 0;
    duration_sec_ = 0.0;
}

bool MediaDecoder::read_frame(AudioFrame& frame) {
    if (!format_open_ || audio_stream_idx_ == -1) { return false; }

    // Only decode once — after that, return false
    if (frame_count_ > 0) { return false; }

    // Write ffmpeg output to temp file (avoids _popen text-mode corruption of binary data)
    // ponytail: unique per call — parallel batch decodes shared one fixed name and clobbered it
    static std::atomic<unsigned long long> decode_seq{0};
    std::ostringstream tmp_name;
    tmp_name << "spectragen_decode_" << std::this_thread::get_id() << "_"
             << decode_seq.fetch_add(1) << ".tmp";
    auto tmp = std::filesystem::temp_directory_path() / tmp_name.str();
    std::string tmp_str = tmp.string();

    // Build ffmpeg command to write s16le to temp file
    std::string cmdline = "ffmpeg -v quiet -y -i \"" + filepath_ +
        "\" -map 0:a:0 -f s16le -ac 2 -vn \"" + tmp_str + "\"";
    std::system(cmdline.c_str());

    // Read binary temp file
    std::ifstream ifs(tmp, std::ios::binary);
    if (!ifs.is_open()) { return false; }

    ifs.seekg(0, std::ios::end);
    auto file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    if (file_size <= 0) {
        std::filesystem::remove(tmp);
        return false;
    }

    std::string raw(static_cast<size_t>(file_size), '\0');
    ifs.read(raw.data(), file_size);
    ifs.close();
    std::filesystem::remove(tmp);

    if (raw.empty()) { return false; }

    // Parse 16-bit signed interleaved samples, convert to float [-1, 1]
    // Interleaved: s0_ch0, s0_ch1, s1_ch0, s1_ch1, ...
    size_t num_samples = raw.size() / (2 * channel_count_);  // 2 bytes per sample
    frame.samples.resize(num_samples * channel_count_);
    frame.sample_rate = sample_rate_;
    frame.num_channels = channel_count_;
    frame.timestamp = duration_sec_ > 0.0 ? (frame_count_ * duration_sec_ / std::max(frame_count_, 1)) : 0.0;

    for (size_t ch = 0; ch < (size_t)channel_count_; ++ch) {
        for (size_t i = 0; i < num_samples; ++i) {
            short sval = 0;
            const uint8_t* byte_ptr = reinterpret_cast<const uint8_t*>(raw.data()) +
                (ch * num_samples + i) * 2;
            // Little-endian on Windows
            sval = (short)((byte_ptr[1] << 8) | byte_ptr[0]);

            float fval = static_cast<float>(sval) / 32768.0f;
            if (fval > 1.0f) fval = 1.0f;
            if (fval < -1.0f) fval = -1.0f;

            frame.samples[ch * num_samples + i] = fval;
        }
    }

    frame_count_++;
    return true;
}

int64_t MediaDecoder::total_frames() const {
    return duration_sec_ > 0.0 ? (int64_t)(duration_sec_ * sample_rate_) : 0;
}

double MediaDecoder::duration() const { return duration_sec_; }

int MediaDecoder::sample_rate() const { return sample_rate_; }

int MediaDecoder::num_channels() const { return channel_count_; }