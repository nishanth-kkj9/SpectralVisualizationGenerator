// Phase 4 — output file handling implementation.

#include "output_files.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace Spectral {

namespace {

// Lowercase ASCII copy for extension comparison (extensions are ASCII;
// the rest of the path is passed through untouched for Unicode/spaces).
std::string lower_ascii(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_video_container_ext(const std::string& ext_lower) {
    return ext_lower == ".mp4" || ext_lower == ".webm" || ext_lower == ".mkv";
}

std::string win_error_text(DWORD code) {
    char* buf = nullptr;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg = (n && buf) ? std::string(buf, n) : "unknown error";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
        msg.pop_back();
    return msg;
}

} // namespace

std::string validate_output_location(const std::string& output_path,
                                     const std::string& format,
                                     bool explicit_format) {
    if (output_path.empty()) return "no output file";
    std::error_code ec;
    if (fs::is_directory(output_path, ec) && !ec)
        return "output path is a directory, file required";
    const std::string ext = lower_ascii(fs::path(output_path).extension().string());
    const bool is_video = (format == "video");
    const bool known_video_ext = is_video_container_ext(ext);
    if (is_video) {
        // ffmpeg sniffs the container from the extension: an extensionless
        // or foreign video path would fail mid-encode, so reject it here.
        if (!known_video_ext)
            return "video output requires .mp4, .webm, or .mkv extension (got '" +
                   fs::path(output_path).extension().string() + "')";
    } else {
        // The image encoder is PNG-only: never write PNG bytes under a
        // misleading extension, and never silently rename the user path.
        if (!ext.empty() && ext != ".png" && !known_video_ext)
            return "image output requires .png extension (got '" +
                   fs::path(output_path).extension().string() + "')";
        if (known_video_ext) {
            // .mp4 etc. with an explicit image format is a contradiction
            // (fail). Without an explicit format the extension infers video
            // (same rule the CLI documents): valid, resolved downstream.
            if (explicit_format)
                return "output-format image contradicts video extension '" + ext + "'";
            return "";
        }
    }
    // Parent must either be absent (cwd), missing (write fails cleanly at
    // render with a typed error), or an existing directory — never an
    // existing non-directory, which no creation could fix.
    const fs::path parent = fs::path(output_path).parent_path();
    if (!parent.empty()) {
        std::error_code pec;
        if (fs::exists(parent, pec) && !pec && !fs::is_directory(parent, pec) && !pec)
            return "output parent path is not a directory";
    }
    return "";
}

bool is_temp_name(const std::string& filename) {
    return filename.find(".part.") != std::string::npos;
}

std::string effective_output_format(const std::string& output_path,
                                    const std::string& format,
                                    bool explicit_format) {
    if (explicit_format || format == "video") return format;
    if (is_video_container_ext(lower_ascii(fs::path(output_path).extension().string())))
        return "video";
    return "image";
}

std::string make_temp_path(const std::string& dst) {
    static std::atomic<unsigned long long> counter{0};
    const DWORD pid = GetCurrentProcessId();
    const fs::path p(dst);
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    const fs::path parent = p.parent_path();
    // Skip names that already exist (stale crash leftover or a user's own
    // file): the counter moves on instead of colliding with it.
    for (int attempt = 0; attempt < 10000; ++attempt) {
        const unsigned long long n = counter.fetch_add(1);
        char infix[64];
        std::snprintf(infix, sizeof(infix), ".%lu.%llu.part", pid, n);
        fs::path cand = parent.empty() ? fs::path(stem + infix + ext)
                                       : parent / (stem + infix + ext);
        std::error_code ec;
        if (!fs::exists(cand, ec) && !ec) return cand.string();
    }
    // Practically unreachable; the caller reports the write failure.
    return (parent / (stem + ".part" + ext)).string();
}

std::string commit_output(const std::string& tmp, const std::string& dst) {
    std::error_code ec;
    if (!fs::is_regular_file(tmp, ec) || ec)
        return "temporary output '" + tmp + "' missing, nothing to commit";
    // MOVEFILE_REPLACE_EXISTING performs the replacement atomically from
    // the caller's view: the destination is never deleted first, so a
    // failure (sharing violation, permissions, disk) leaves the previous
    // valid output intact. WRITE_THROUGH flushes file data before return.
    if (MoveFileWithProgressA(tmp.c_str(), dst.c_str(), nullptr, nullptr,
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return "";
    const DWORD code = GetLastError();
    // The temp is uniquely owned by this process: remove it so a failed
    // replacement leaves no residue. Best-effort; the primary error wins.
    std::error_code rec;
    fs::remove(tmp, rec);
    return "cannot replace output '" + dst + "': " + win_error_text(code);
}

} // namespace Spectral
