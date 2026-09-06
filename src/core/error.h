#pragma once

// Core error/result model (S2): one envelope for the whole pipeline.
// - JobError codes are stable (pinned by tests) — do not renumber.
// - Every failure names its subsystem + reason (+ input path where known).
// - Error compares against JobError so existing Ok-checks keep working.

#include <string>
#include <utility>

namespace Spectral {

enum class Subsystem {
    Unknown = 0,
    Media,
    Audio,
    Dsp,
    Dataset,
    Render,
    Encode,
    Project,
    Pipeline,
    Batch,
};

// Canonical pipeline result codes. Values are pinned — do not renumber.
enum class JobError {
    Ok = 0,
    FileNotFound,
    DecodeError,
    AnalysisError,
    RenderError,
    BadConfig,
};

struct Error {
    Subsystem subsystem = Subsystem::Unknown;
    JobError code = JobError::Ok;
    std::string message;

    bool ok() const { return code == JobError::Ok; }
    static Error success() { return Error{}; }
    static Error make(Subsystem s, JobError c, std::string m) {
        Error e;
        e.subsystem = s;
        e.code = c;
        e.message = std::move(m);
        return e;
    }

    bool operator==(JobError c) const { return code == c; }
    bool operator!=(JobError c) const { return code != c; }
};

inline const char* subsystem_name(Subsystem s) noexcept {
    switch (s) {
        case Subsystem::Media:    return "media";
        case Subsystem::Audio:    return "audio";
        case Subsystem::Dsp:      return "dsp";
        case Subsystem::Dataset:  return "dataset";
        case Subsystem::Render:   return "render";
        case Subsystem::Encode:   return "encode";
        case Subsystem::Project:  return "project";
        case Subsystem::Pipeline: return "pipeline";
        case Subsystem::Batch:    return "batch";
        case Subsystem::Unknown:  return "unknown";
    }
    return "unknown";
}

inline const char* job_error_name(JobError e) noexcept {
    switch (e) {
        case JobError::Ok:            return "ok";
        case JobError::FileNotFound:  return "file-not-found";
        case JobError::DecodeError:   return "decode-error";
        case JobError::AnalysisError: return "analysis-error";
        case JobError::RenderError:   return "render-error";
        case JobError::BadConfig:     return "bad-config";
    }
    return "unknown";
}

} // namespace Spectral
