#pragma once

// Phase 18 — SpectraScope main window. Thin client: collects GenerateConfig,
// runs it on a worker thread, shows progress + result. No DSP here.

#include <QMainWindow>

#include <atomic>

class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QThread;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    // A close during an active job requests cancellation and waits for the
    // worker to finish cleanup, so no thread outlives the window.
    void closeEvent(QCloseEvent* event) override;

private slots:
    void browseInput();
    void browseOutput();
    void applyPreset(int index);
    void generate();
    void cancelJob();
    void onProgress(int percent, const QString& stage);
    void onFinished(bool ok, const QString& message, const QString& outputPath);
    void openOutput();

private:
    void setRunning(bool running);

    QLineEdit* inputEdit_;
    QLineEdit* outputEdit_;
    QComboBox* vizCombo_;
    QComboBox* formatCombo_;
    QComboBox* presetCombo_;
    QComboBox* fftCombo_;
    QComboBox* windowCombo_;
    QComboBox* scaleCombo_;
    QComboBox* resCombo_;
    QSpinBox* dbSpin_;
    QCheckBox* gpuCheck_;
    QCheckBox* reassignCheck_;
    QPushButton* generateBtn_;
    QPushButton* cancelBtn_;
    QPushButton* openBtn_;
    QProgressBar* progressBar_;
    QLabel* statusLabel_;
    QLabel* previewLabel_;
    QString lastOutput_;
    QThread* thread_ = nullptr;
    // Cancellation flag: owned here, observed by the worker thread.
    // Outlives the worker (reset per job, never destroyed mid-job).
    std::atomic<bool> cancel_{false};
};
