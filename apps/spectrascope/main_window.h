#pragma once

// Phase 18 — SpectraScope main window. Thin client: collects GenerateConfig,
// runs it on a worker thread, shows progress + result. No DSP here.

#include <QMainWindow>

#include <atomic>

// Return type of the configForTest() hook below; defined in pipeline.h.
namespace Spectral { struct GenerateConfig; }

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
    void onRepresentationChanged(int index);
    void onProgress(int percent, const QString& stage);
    void onFinished(bool ok, const QString& message, const QString& outputPath);
    void onThreadFinished();
    void openOutput();

public slots:
    void generate();
    void cancelJob();

private:
    void setRunning(bool running);
    // Widget state -> GenerateConfig (single mapping; generate() and the
    // config test hook both use it, so the test observes what would run).
    Spectral::GenerateConfig collectConfig() const;
    // Mel band count must stay within [1, fft-bins] for the CURRENT fft
    // combo value; the spin bounds track the combo.
    void updateMelBandsBounds();
    // QComboBox has no per-item enable API; its default model carries the flag.
    void setVizOptionEnabled(int index, bool enabled);

    QLineEdit* inputEdit_;
    QLineEdit* outputEdit_;
    QComboBox* vizCombo_;
    QComboBox* reprCombo_;     // what is computed: stft | mel filterbank
    QComboBox* formatCombo_;
    QComboBox* presetCombo_;
    QComboBox* fftCombo_;
    QComboBox* windowCombo_;
    QComboBox* scaleCombo_;    // display mapping only, never the representation
    QComboBox* resCombo_;
    QComboBox* melNormCombo_;   // mel-only knob (enabled with representation=mel)
    QSpinBox* dbSpin_;
    QSpinBox* melBandsSpin_;   // mel-only knob (enabled with representation=mel)
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
    // True after a close was requested while a worker thread object was
    // still alive: the close is deferred until onThreadFinished runs, so
    // the window is never destroyed with a live worker.
    bool closing_ = false;

public:
    // Test hooks (headless lifecycle tests only; no production use).
    void setInputForTest(const QString& p);
    void setOutputForTest(const QString& p);
    // Drive the representation combo like a user would ("stft" | "mel");
    // also sets a mel band count for the mel case.
    void setRepresentationForTest(const QString& repr);
    void setMelBandsForTest(int bands);
    // The exact GenerateConfig generate() would submit right now.
    Spectral::GenerateConfig configForTest() const;
    bool isJobActiveForTest() const { return thread_ != nullptr; }
    QString statusTextForTest() const;
};
