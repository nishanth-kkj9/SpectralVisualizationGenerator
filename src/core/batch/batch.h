#pragma once

// Phase 19 — Batch processing over the shared pipeline.
// One GenerateConfig template applied to many files; each file runs run_job
// independently so results are verifiable per file. Thread count bounded.

#include "pipeline.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace Spectral {

struct BatchOptions {
    bool recursive = false;
    int jobs = 0;        // 0 = hardware_concurrency, clamped to [1, hw]
    int retries = 1;     // extra attempts after the first failure
    std::vector<std::string> extensions;  // empty = known media exts
};

struct FileResult {
    std::string input;
    std::string output;
    bool ok = false;
    std::string error;
    int attempts = 0;
    double elapsed_ms = 0.0;
};

// Progress: done/total files finished, current file path.
using BatchProgressFn = std::function<void(int, int, const char*)>;

// Collect input files: file -> itself (if ext matches), dir -> scan.
std::vector<std::string> collect_inputs(const std::string& input,
                                        const BatchOptions& opts);

// Output path: mirror input's relative path under output_dir, new extension.
std::string batch_output_path(const std::string& input_root,
                              const std::string& input_file,
                              const std::string& output_dir,
                              const std::string& output_format);

// Run all files. Never throws; per-file failures recorded in results.
// cancel checked before each file starts and between retries.
std::vector<FileResult> run_batch(const GenerateConfig& template_cfg,
                                  const std::vector<std::string>& files,
                                  const std::string& input_root,
                                  const std::string& output_dir,
                                  const BatchOptions& opts,
                                  const std::atomic<bool>& cancel,
                                  BatchProgressFn progress = {});

} // namespace Spectral
