#include "batch.h"
#include "thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace Spectral {

static const char* kMediaExts[] = {
    ".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".wma",
    ".mp4", ".mkv", ".webm", ".mov", ".avi",
};

static std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

static bool ext_ok(const fs::path& p, const std::vector<std::string>& exts) {
    std::string e = lower_ext(p);
    if (exts.empty()) {
        for (auto* m : kMediaExts)
            if (e == m) return true;
        return false;
    }
    for (const auto& x : exts) {
        std::string xe = x;
        if (!xe.empty() && xe[0] != '.') xe = "." + xe;
        for (auto& c : xe) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e == xe) return true;
    }
    return false;
}

std::vector<std::string> collect_inputs(const std::string& input, const BatchOptions& opts) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path p(input);
    if (!fs::exists(p, ec)) return out;
    if (fs::is_regular_file(p, ec)) {
        if (ext_ok(p, opts.extensions)) out.push_back(p.string());
        return out;
    }
    if (!fs::is_directory(p, ec)) return out;
    auto push_dir = [&](const fs::path& dir) {
        for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && ext_ok(it->path(), opts.extensions))
                out.push_back(it->path().string());
        }
    };
    if (opts.recursive) {
        for (auto it = fs::recursive_directory_iterator(p, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && ext_ok(it->path(), opts.extensions))
                out.push_back(it->path().string());
        }
    } else {
        push_dir(p);
    }
    std::sort(out.begin(), out.end());  // ponytail: deterministic order
    return out;
}

std::string batch_output_path(const std::string& input_root, const std::string& input_file,
                              const std::string& output_dir, const std::string& output_format) {
    fs::path in(input_file);
    std::string ext = (output_format == "video") ? ".mp4" : ".png";
    fs::path out_dir(output_dir);
    std::error_code ec;
    // Mirror relative tree when input sits under input_root (folder mode)
    fs::path root(input_root);
    if (fs::is_directory(root, ec)) {
        fs::path rel = fs::relative(in, root, ec);
        if (!ec && !rel.empty() && rel.generic_string().rfind("..", 0) != 0) {
            fs::path mirrored = out_dir / rel;
            mirrored.replace_extension(ext);
            return mirrored.string();
        }
    }
    return (out_dir / in.stem().concat(ext)).string();
}

std::vector<FileResult> run_batch(const GenerateConfig& template_cfg,
                                  const std::vector<std::string>& files,
                                  const std::string& input_root,
                                  const std::string& output_dir,
                                  const BatchOptions& opts,
                                  const std::atomic<bool>& cancel,
                                  BatchProgressFn progress) {
    std::vector<FileResult> results(files.size());
    if (files.empty()) return results;

    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw < 1) hw = 1;
    int jobs = opts.jobs <= 0 ? hw : std::min(opts.jobs, hw);
    if (jobs < 1) jobs = 1;

    std::error_code ec;
    fs::create_directories(output_dir, ec);

    std::mutex prog_mtx;
    int done = 0;

    {
        ThreadPool pool(jobs);
        for (size_t i = 0; i < files.size(); ++i) {
            pool.submit([&, i] {
                FileResult r;
                r.input = files[i];
                r.output = batch_output_path(input_root, files[i], output_dir,
                                             template_cfg.output_format);
                // Ensure mirrored parent dirs exist (threads share output tree)
                std::error_code ec2;
                fs::create_directories(fs::path(r.output).parent_path(), ec2);

                if (cancel.load()) {
                    r.error = "cancelled";
                } else {
                    GenerateConfig cfg = template_cfg;
                    cfg.input_path = files[i];
                    cfg.output_path = r.output;
                    int max_attempts = 1 + std::max(0, opts.retries);
                    for (int a = 1; a <= max_attempts; ++a) {
                        if (cancel.load()) {
                            r.error = "cancelled";
                            break;
                        }
                        r.attempts = a;
                        auto t0 = std::chrono::steady_clock::now();
                        Error err = Error::success();
                        try {
                            err = run_job(cfg);
                        } catch (const std::exception& ex) {
                            err = Error::make(Subsystem::Pipeline, JobError::AnalysisError,
                                              std::string("pipeline: ") + ex.what());
                        } catch (...) {
                            err = Error::make(Subsystem::Pipeline, JobError::AnalysisError,
                                              "pipeline: unknown exception");
                        }
                        auto t1 = std::chrono::steady_clock::now();
                        r.elapsed_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                        if (err == JobError::Ok) {
                            r.ok = true;
                            r.error.clear();
                            break;
                        }
                        if (r.error.empty())
                            r.error = err.message;
                    }
                }
                results[i] = r;
                if (progress) {
                    std::lock_guard lk(prog_mtx);
                    progress(++done, static_cast<int>(files.size()), files[i].c_str());
                }
            });
        }
    }  // pool joins here; all results written

    return results;
}

} // namespace Spectral
