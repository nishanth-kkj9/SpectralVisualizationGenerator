#include "main_window.h"
#include "worker.h"
#include "pipeline.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("SpectraScope"));
    setAcceptDrops(true);
    resize(760, 640);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    // 1. Input
    auto* fileRow = new QHBoxLayout();
    inputEdit_ = new QLineEdit(this);
    inputEdit_->setPlaceholderText(QStringLiteral("Audio or video file (or drop here)…"));
    auto* browseBtn = new QPushButton(QStringLiteral("Browse…"), this);
    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::browseInput);
    fileRow->addWidget(inputEdit_, 1);
    fileRow->addWidget(browseBtn);
    layout->addLayout(fileRow);

    // 2+3. Visualization + preset
    auto* form = new QFormLayout();
    vizCombo_ = new QComboBox(this);
    vizCombo_->addItems({QStringLiteral("Spectrogram"), QStringLiteral("Spectrum")});
    form->addRow(QStringLiteral("Visualization:"), vizCombo_);

    formatCombo_ = new QComboBox(this);
    formatCombo_->addItems({QStringLiteral("Image (PNG)"), QStringLiteral("Video (MP4)")});
    form->addRow(QStringLiteral("Output format:"), formatCombo_);

    presetCombo_ = new QComboBox(this);
    presetCombo_->addItems({QStringLiteral("Voice (1024, hann)"),
                            QStringLiteral("Music (2048, hann)"),
                            QStringLiteral("Detail (4096, blackman, mel)")});
    connect(presetCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyPreset);
    form->addRow(QStringLiteral("Analysis preset:"), presetCombo_);

    fftCombo_ = new QComboBox(this);
    fftCombo_->addItems({QStringLiteral("512"), QStringLiteral("1024"),
                         QStringLiteral("2048"), QStringLiteral("4096")});
    fftCombo_->setCurrentIndex(1);
    form->addRow(QStringLiteral("FFT size:"), fftCombo_);

    windowCombo_ = new QComboBox(this);
    windowCombo_->addItems({QStringLiteral("hann"), QStringLiteral("hamming"),
                            QStringLiteral("blackman"), QStringLiteral("rectangular")});
    form->addRow(QStringLiteral("Window:"), windowCombo_);

    scaleCombo_ = new QComboBox(this);
    scaleCombo_->addItems({QStringLiteral("log"), QStringLiteral("linear"),
                           QStringLiteral("mel"), QStringLiteral("bark"),
                           QStringLiteral("erb"), QStringLiteral("cqt")});
    form->addRow(QStringLiteral("Frequency scale:"), scaleCombo_);

    resCombo_ = new QComboBox(this);
    resCombo_->addItems({QStringLiteral("512x256"), QStringLiteral("1024x512"),
                         QStringLiteral("1920x1080")});
    resCombo_->setCurrentIndex(1);
    form->addRow(QStringLiteral("Resolution:"), resCombo_);

    dbSpin_ = new QSpinBox(this);
    dbSpin_->setRange(20, 120);
    dbSpin_->setValue(80);
    dbSpin_->setSuffix(QStringLiteral(" dB"));
    form->addRow(QStringLiteral("Dynamic range:"), dbSpin_);
    layout->addLayout(form);

    gpuCheck_ = new QCheckBox(QStringLiteral("GPU rendering (CPU fallback)"), this);
    layout->addWidget(gpuCheck_);
    reassignCheck_ = new QCheckBox(QStringLiteral("Time-frequency reassignment"), this);
    layout->addWidget(reassignCheck_);

    // 4. Output
    auto* outRow = new QHBoxLayout();
    outputEdit_ = new QLineEdit(this);
    outputEdit_->setPlaceholderText(QStringLiteral("Output file…"));
    auto* outBtn = new QPushButton(QStringLiteral("Browse…"), this);
    connect(outBtn, &QPushButton::clicked, this, &MainWindow::browseOutput);
    outRow->addWidget(outputEdit_, 1);
    outRow->addWidget(outBtn);
    layout->addLayout(outRow);

    // 5. Generate
    generateBtn_ = new QPushButton(QStringLiteral("Generate"), this);
    connect(generateBtn_, &QPushButton::clicked, this, &MainWindow::generate);
    layout->addWidget(generateBtn_);

    // 6. Progress
    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    layout->addWidget(progressBar_);
    statusLabel_ = new QLabel(QStringLiteral("Ready."), this);
    layout->addWidget(statusLabel_);

    // 7. Result
    previewLabel_ = new QLabel(this);
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setMinimumHeight(200);
    previewLabel_->setText(QStringLiteral("Result preview appears here."));
    previewLabel_->setScaledContents(false);
    layout->addWidget(previewLabel_, 1);

    // 8. Open location
    openBtn_ = new QPushButton(QStringLiteral("Open output location"), this);
    openBtn_->setEnabled(false);
    connect(openBtn_, &QPushButton::clicked, this, &MainWindow::openOutput);
    layout->addWidget(openBtn_);

    setCentralWidget(central);
    applyPreset(0);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty()) {
        inputEdit_->setText(urls.first().toLocalFile());
        QFileInfo fi(urls.first().toLocalFile());
        outputEdit_->setText(fi.path() + QStringLiteral("/") + fi.completeBaseName() +
                             (formatCombo_->currentIndex() == 1 ? QStringLiteral(".mp4")
                                                                : QStringLiteral(".png")));
    }
}

void MainWindow::browseInput() {
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select audio or video"), QString(),
        QStringLiteral("Media (*.wav *.mp3 *.flac *.ogg *.mp4 *.mkv *.webm *.mov *.m4a);;All (*)"));
    if (!path.isEmpty()) {
        inputEdit_->setText(path);
        QFileInfo fi(path);
        outputEdit_->setText(fi.path() + QStringLiteral("/") + fi.completeBaseName() +
                             (formatCombo_->currentIndex() == 1 ? QStringLiteral(".mp4")
                                                                : QStringLiteral(".png")));
    }
}

void MainWindow::browseOutput() {
    bool video = formatCombo_->currentIndex() == 1;
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Output file"), outputEdit_->text(),
        video ? QStringLiteral("Video (*.mp4)") : QStringLiteral("Image (*.png)"));
    if (!path.isEmpty()) outputEdit_->setText(path);
}

void MainWindow::applyPreset(int index) {
    // ponytail: presets just set widget values; single source of truth stays in widgets
    if (index == 0) {      // Voice
        fftCombo_->setCurrentIndex(1);  // 1024
        windowCombo_->setCurrentIndex(0);
        scaleCombo_->setCurrentIndex(0);
        dbSpin_->setValue(80);
    } else if (index == 1) {  // Music
        fftCombo_->setCurrentIndex(2);  // 2048
        windowCombo_->setCurrentIndex(0);
        scaleCombo_->setCurrentIndex(0);
        dbSpin_->setValue(90);
    } else {  // Detail
        fftCombo_->setCurrentIndex(3);  // 4096
        windowCombo_->setCurrentIndex(2);
        scaleCombo_->setCurrentIndex(2);
        dbSpin_->setValue(90);
    }
}

void MainWindow::setRunning(bool running) {
    generateBtn_->setEnabled(!running);
    openBtn_->setEnabled(!running && !lastOutput_.isEmpty());
    if (running) progressBar_->setValue(0);
}

void MainWindow::generate() {
    Spectral::GenerateConfig cfg;
    cfg.input_path = inputEdit_->text().toStdString();
    cfg.output_path = outputEdit_->text().toStdString();
    cfg.visualization = vizCombo_->currentIndex() == 1 ? "spectrum" : "spectrogram";
    cfg.output_format = formatCombo_->currentIndex() == 1 ? "video" : "image";
    // The combo always decides the format: a typed path with a mismatched
    // extension (e.g. Image + .mp4) fails closed in validate_config below.
    cfg.output_format_explicit = true;
    cfg.fft_size = fftCombo_->currentText().toInt();
    cfg.window = windowCombo_->currentText().toStdString();
    cfg.freq_scale = scaleCombo_->currentText().toStdString();
    cfg.db_range = static_cast<float>(dbSpin_->value());
    const QString res = resCombo_->currentText();
    const int x = res.indexOf(QLatin1Char('x'));
    cfg.width = res.left(x).toInt();
    cfg.height = res.mid(x + 1).toInt();
    cfg.use_gpu = gpuCheck_->isChecked();
    cfg.reassigned = reassignCheck_->isChecked();

    const std::string err = Spectral::validate_config(cfg);
    if (!err.empty()) {
        statusLabel_->setText(QStringLiteral("Error: ") + QString::fromStdString(err));
        return;
    }

    setRunning(true);
    statusLabel_->setText(QStringLiteral("Starting…"));

    thread_ = new QThread(this);
    auto* worker = new Worker(std::move(cfg));
    worker->moveToThread(thread_);
    connect(thread_, &QThread::started, worker, &Worker::run);
    connect(worker, &Worker::progress, this, &MainWindow::onProgress);
    connect(worker, &Worker::finished, this, &MainWindow::onFinished);
    connect(worker, &Worker::finished, thread_, &QThread::quit);
    connect(thread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
    thread_->start();
}

void MainWindow::onProgress(int percent, const QString& stage) {
    progressBar_->setValue(percent);
    statusLabel_->setText(QStringLiteral("[%1] %2%").arg(stage).arg(percent));
}

void MainWindow::onFinished(bool ok, const QString& message, const QString& outputPath) {
    thread_ = nullptr;
    lastOutput_ = outputPath;
    setRunning(false);
    progressBar_->setValue(ok ? 100 : 0);
    statusLabel_->setText(message);
    if (ok && !outputPath.isEmpty()) {
        if (outputPath.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
            QPixmap pm(outputPath);
            if (!pm.isNull())
                previewLabel_->setPixmap(pm.scaled(previewLabel_->size(), Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation));
        } else {
            previewLabel_->setText(QStringLiteral("Video saved. Open output to view."));
        }
    }
}

void MainWindow::openOutput() {
    if (lastOutput_.isEmpty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QFileInfo(lastOutput_).absolutePath()));
}
