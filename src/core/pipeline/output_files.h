#pragma once

// Phase 4 — Production-safe output file handling (single authority).
//
// Format/extension contract (image encoder is PNG-only; ffmpeg sniffs the
// video container from the extension):
//   explicit image -> ".png" (case-insensitive) or no extension.
//   explicit video -> ".mp4" | ".webm" | ".mkv" (case-insensitive).
//   inferred       -> video containers above resolve to video, anything
//                     else must be ".png" or extensionless (image).
// Anything else is a configuration error. The user's path is never
// silently renamed: contradictions fail, they do not get rewritten.
//
// Replacement contract (Windows): MoveFileWithProgress with
// MOVEFILE_REPLACE_EXISTING performs the replacement without ever
// deleting the destination first, so a failed replacement leaves the
// previous valid output intact. New bytes only ever go to a uniquely
// named temp file in the SAME directory (same volume) as the final path.

#include <atomic>
#include <filesystem>
#include <string>

namespace Spectral {

// Resolve + validate the output location. `format` is "image"|"video"
// (already checked for spelling upstream); `explicit_format` is true when
// the user passed --output-format / the GUI combo decided (vs. inferred
// from the extension). Returns "" when valid, else a BadConfig message.
std::string validate_output_location(const std::string& output_path,
                                     const std::string& format,
                                     bool explicit_format);

// Effective format after the inference rule: explicit format always wins;
// otherwise a video-container extension means video, anything else image.
// Call only with a location that validate_output_location accepted.
std::string effective_output_format(const std::string& output_path,
                                    const std::string& format,
                                    bool explicit_format);
// Collision-safe temp path in the destination's own directory:
// "<stem>.<pid>.<counter>.part<ext>". Same volume as dst (rename never
// crosses filesystems), unique per process + counter, and the ".part."
// infix keeps temps identifiable (never valid final outputs, never
// ingested by batch scans). Skips names that already exist.
std::string make_temp_path(const std::string& dst);

// Replacement without remove-first. Never touches dst on failure; removes
// the owned temp file on failure (safe: uniquely named, current process).
// Returns "" on success, else a human-readable reason (shares the OS
// error, e.g. sharing violation when dst is open elsewhere).
std::string commit_output(const std::string& tmp, const std::string& dst);

// True for temp names produced by make_temp_path (also matches the older
// "<stem>.part<ext>" scheme so stale crash leftovers stay recognizable).
// Used to keep directory scans from ingesting temp files.
bool is_temp_name(const std::string& filename);

// RAII temp cleanup: removes path_ unless dismissed. Use around the
// render/encode-then-commit window so every failure path leaves no temp.
class TempGuard {
public:
    explicit TempGuard(std::string path) : path_(std::move(path)) {}
    TempGuard(const TempGuard&) = delete;
    TempGuard& operator=(const TempGuard&) = delete;
    ~TempGuard() {
        if (dismissed_) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    void dismiss() { dismissed_ = true; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    bool dismissed_ = false;
};

} // namespace Spectral
