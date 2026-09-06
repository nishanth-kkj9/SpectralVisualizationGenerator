#pragma once

// Phase 18 — SpectraScope main window. Thin client: collects GenerateConfig,
// runs it on a worker thread, shows progress + result. No DSP here.

#include <QMainWindow>

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

private slots:
    void browseInput();
    void browseOutput();
    void applyPreset(int index);
    void generate();
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
    QPushButton* openBtn_;
    QProgressBar* progressBar_;
    QLabel* statusLabel_;
    QLabel* previewLabel_;
    QString lastOutput_;
    QThread* thread_ = nullptr;
};
