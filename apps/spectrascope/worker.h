#pragma once

// Phase 18 — Background worker: runs Spectral::run_job off the UI thread.

#include "pipeline.h"

#include <QObject>
#include <QString>

#include <atomic>

class Worker : public QObject {
    Q_OBJECT
public:
    // cancel is not owned; MainWindow keeps it alive longer than the worker.
    explicit Worker(Spectral::GenerateConfig cfg, const std::atomic<bool>* cancel,
                    QObject* parent = nullptr);

signals:
    void progress(int percent, const QString& stage);
    void finished(bool ok, const QString& message, const QString& outputPath);

public slots:
    void run();

private:
    Spectral::GenerateConfig cfg_;
    const std::atomic<bool>* cancel_ = nullptr;
};
