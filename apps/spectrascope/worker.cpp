#include "worker.h"

Worker::Worker(Spectral::GenerateConfig cfg, const std::atomic<bool>* cancel,
               QObject* parent)
    : QObject(parent), cfg_(std::move(cfg)), cancel_(cancel) {}

void Worker::run() {
    Spectral::Error err = Spectral::run_job(
        cfg_,
        [this](float f, const char* stage) {
            emit progress(static_cast<int>(f * 100.0f), QString::fromUtf8(stage));
        },
        cancel_);
    if (err == Spectral::JobError::Ok) {
        emit finished(true, QStringLiteral("Done"),
                      QString::fromStdString(cfg_.output_path));
    } else if (err == Spectral::JobError::Cancelled) {
        // Cancelled is not success and not failure: no output path, no
        // preview. The pipeline already removed its temp file and kept
        // any previous output.
        emit finished(false, QStringLiteral("Cancelled"), QString());
    } else {
        emit finished(false,
                      QString::fromStdString(err.message),
                      QString());
    }
}
