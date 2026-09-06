#pragma once

// Phase 18 — Background worker: runs Spectral::run_job off the UI thread.

#include "pipeline.h"

#include <QObject>
#include <QString>

class Worker : public QObject {
    Q_OBJECT
public:
    explicit Worker(Spectral::GenerateConfig cfg, QObject* parent = nullptr);

signals:
    void progress(int percent, const QString& stage);
    void finished(bool ok, const QString& message, const QString& outputPath);

public slots:
    void run();

private:
    Spectral::GenerateConfig cfg_;
};
