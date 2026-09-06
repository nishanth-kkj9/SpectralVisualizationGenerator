#include "worker.h"

Worker::Worker(Spectral::GenerateConfig cfg, QObject* parent)
    : QObject(parent), cfg_(std::move(cfg)) {}

void Worker::run() {
    Spectral::Error err = Spectral::run_job(cfg_,
        [this](float f, const char* stage) {
            emit progress(static_cast<int>(f * 100.0f), QString::fromUtf8(stage));
        });
    if (err == Spectral::JobError::Ok) {
        emit finished(true, QStringLiteral("Done"),
                      QString::fromStdString(cfg_.output_path));
    } else {
        emit finished(false,
                      QString::fromStdString(err.message),
                      QString());
    }
}
