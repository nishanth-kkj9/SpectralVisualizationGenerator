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
#include <QStandardItem>
#include <QStandardItemModel>
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

    // Representation is WHAT is computed — unlike the frequency-scale combo
    // below, which only remaps the display. Mel runs a real triangular Mel
    // filterbank over STFT power; STFT keeps FFT bins (phases included).
    reprCombo_ = new QComboBox(this);
    reprCombo_->addItems({QStringLiteral("STFT (FFT bins)"),
                          QStringLiteral("Mel filterbank")});
    connect(reprCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onRepresentationChanged);
    form->addRow(QStringLiteral("Representation:"), reprCombo_);

    // Mel-only knobs; disabled until the representation combo selects Mel
    // (the pipeline defaults cover STFT runs, so they are never collected
    // for STFT).
    melBandsSpin_ = new QSpinBox(this);
    melBandsSpin_->setRange(1, 257);  // refined per FFT size in updateMelBandsBounds
    melBandsSpin_->setValue(64);
    melBandsSpin_->setSuffix(QStringLiteral(" bands"));
    melBandsSpin_->setEnabled(false);
    form->addRow(QStringLiteral("Mel bands:"), melBandsSpin_);

    melNormCombo_ = new QComboBox(this);
    melNormCombo_->addItems({QStringLiteral("none"), QStringLiteral("slaney"),
                             QStringLiteral("area")});
    melNormCombo_->setCurrentIndex(1);  // default matches the pipeline default
    melNormCombo_->setEnabled(false);
    form->addRow(QStringLiteral("Mel normalization:"), melNormCombo_);

    formatCombo_ = new QComboBox(this);
    formatCombo_->addItems({QStringLiteral("Image (PNG)"), QStringLiteral("Video (MP4)")});
    form->addRow(QStringLiteral("Output format:"), formatCombo_);

    presetCombo_ = new QComboBox(this);
    presetCombo_->addItems({QStringLiteral("Voice (1024, hann)"),
                            QStringLiteral("Music (2048, hann)"),
                            QStringLiteral("Detail (4096, blackman, mel scale)"),
                            QStringLiteral("Mel filterbank (2048, hann, 128 bands)")});
    connect(presetCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyPreset);
    form->addRow(QStringLiteral("Analysis preset:"), presetCombo_);

    fftCombo_ = new QComboBox(this);
    fftCombo_->addItems({QStringLiteral("512"), QStringLiteral("1024"),
                         QStringLiteral("2048"), QStringLiteral("4096")});
    fftCombo_->setCurrentIndex(1);
    // The mel-bands spin bounds track the FFT combo, so the GUI can never
    // build a mel-bands value outside [1, fft-bins].
    connect(fftCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::updateMelBandsBounds);
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

    // 5. Generate + Cancel (side by side; Cancel only while running)
    auto* runRow = new QHBoxLayout();
    generateBtn_ = new QPushButton(QStringLiteral("Generate"), this);
    connect(generateBtn_, &QPushButton::clicked, this, &MainWindow::generate);
    runRow->addWidget(generateBtn_, 1);
    cancelBtn_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancelBtn_->setEnabled(false);
    connect(cancelBtn_, &QPushButton::clicked, this, &MainWindow::cancelJob);
    runRow->addWidget(cancelBtn_);
    layout->addLayout(runRow);

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
    // Sync the derived widget states once after all widgets exist (the
    // signal connections above only cover later user/preset changes).
    updateMelBandsBounds();
    onRepresentationChanged(reprCombo_->currentIndex());
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
        reprCombo_->setCurrentIndex(0);
        fftCombo_->setCurrentIndex(1);  // 1024
        windowCombo_->setCurrentIndex(0);
        scaleCombo_->setCurrentIndex(0);
        dbSpin_->setValue(80);
    } else if (index == 1) {  // Music
        reprCombo_->setCurrentIndex(0);
        fftCombo_->setCurrentIndex(2);  // 2048
        windowCombo_->setCurrentIndex(0);
        scaleCombo_->setCurrentIndex(0);
        dbSpin_->setValue(90);
    } else if (index == 2) {  // Detail: mel DISPLAY scale over STFT data
        reprCombo_->setCurrentIndex(0);
        fftCombo_->setCurrentIndex(3);  // 4096
        windowCombo_->setCurrentIndex(2);
        scaleCombo_->setCurrentIndex(2);
        dbSpin_->setValue(90);
    } else {  // Mel filterbank: real Mel representation
        reprCombo_->setCurrentIndex(1);
        fftCombo_->setCurrentIndex(2);  // 2048
        windowCombo_->setCurrentIndex(0);
        scaleCombo_->setCurrentIndex(0);
        melBandsSpin_->setValue(128);
        melNormCombo_->setCurrentIndex(1);  // slaney
        dbSpin_->setValue(90);
    }
}

void MainWindow::onRepresentationChanged(int index) {
    const bool mel = (index == 1);
    melBandsSpin_->setEnabled(mel);
    melNormCombo_->setEnabled(mel);
    // Reassignment needs STFT phase and the spectrum plot renderer is
    // STFT-only; the GUI refuses both combinations up front instead of
    // surfacing a validation error only after Generate.
    reassignCheck_->setEnabled(!mel);
    if (mel) reassignCheck_->setChecked(false);
    setVizOptionEnabled(1 /* Spectrum */, !mel);
    if (mel && vizCombo_->currentIndex() == 1) vizCombo_->setCurrentIndex(0);
}

void MainWindow::updateMelBandsBounds() {
    // Validation caps mel-bands at fft-bins (fft/2 + 1); track the combo so
    // the spin can never hold an out-of-range value.
    const int fft = fftCombo_->currentText().toInt();
    const int max_bands = fft / 2 + 1;
    melBandsSpin_->setMaximum(max_bands);
    if (melBandsSpin_->value() > max_bands) melBandsSpin_->setValue(max_bands);
}

void MainWindow::setVizOptionEnabled(int index, bool enabled) {
    // QComboBox exposes per-item flags through its (default item) model;
    // a disabled item is shown greyed and cannot be selected.
    auto* model = qobject_cast<QStandardItemModel*>(vizCombo_->model());
    if (!model) return;
    if (QStandardItem* item = model->item(index)) item->setEnabled(enabled);
}

Spectral::GenerateConfig MainWindow::collectConfig() const {
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
    // Representation is what gets computed; the frequency-scale combo above
    // stays a display mapping. Mel knobs are collected only for a Mel run.
    cfg.representation = reprCombo_->currentIndex() == 1 ? "mel" : "stft";
    if (cfg.representation == "mel") {
        cfg.mel_bands = melBandsSpin_->value();
        cfg.mel_norm = melNormCombo_->currentText().toStdString();
    }
    return cfg;
}

void MainWindow::setRunning(bool running) {
    generateBtn_->setEnabled(!running);
    cancelBtn_->setEnabled(running);
    openBtn_->setEnabled(!running && !lastOutput_.isEmpty());
    if (running) progressBar_->setValue(0);
}

void MainWindow::generate() {
    // A deferred close is pending: the window is going away, so no new
    // jobs may start (this also keeps closing_ meaningful).
    if (closing_) return;
    Spectral::GenerateConfig cfg = collectConfig();

    const std::string err = Spectral::validate_config(cfg);
    if (!err.empty()) {
        statusLabel_->setText(QStringLiteral("Error: ") + QString::fromStdString(err));
        return;
    }

    setRunning(true);
    statusLabel_->setText(QStringLiteral("Starting…"));
    cancel_.store(false);

    thread_ = new QThread(this);
    auto* worker = new Worker(std::move(cfg), &cancel_);
    worker->moveToThread(thread_);
    connect(thread_, &QThread::started, worker, &Worker::run);
    connect(worker, &Worker::progress, this, &MainWindow::onProgress);
    connect(worker, &Worker::finished, this, &MainWindow::onFinished);
    connect(worker, &Worker::finished, thread_, &QThread::quit);
    connect(thread_, &QThread::finished, this, &MainWindow::onThreadFinished);
    connect(thread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
    thread_->start();
}

void MainWindow::cancelJob() {
    // Request only: the worker observes the flag at the next stage
    // boundary, cleans up (temp removed, child terminated), and reports.
    // The button disables immediately so a second click cannot re-arm.
    if (!thread_) return;
    cancel_.store(true);
    cancelBtn_->setEnabled(false);
    statusLabel_->setText(QStringLiteral("Cancelling…"));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (thread_) {
        // A worker thread object is still alive (running or finishing):
        // request cancellation and DEFER the close. The window stays alive
        // until onThreadFinished clears thread_ and re-issues close().
        // Never blocks, so no deadlock with the worker (which never waits
        // on the GUI thread); a second close simply defers again.
        cancel_.store(true);
        closing_ = true;
        statusLabel_->setText(QStringLiteral("Cancelling…"));
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::onProgress(int percent, const QString& stage) {
    progressBar_->setValue(percent);
    statusLabel_->setText(QStringLiteral("[%1] %2%").arg(stage).arg(percent));
}

void MainWindow::onFinished(bool ok, const QString& message, const QString& outputPath) {
    // NOTE: thread_ is NOT cleared here. The worker-finished signal is
    // queued ahead of the thread-quit, so the QThread object may still be
    // alive (even running its event loop) at this point. Clearing here
    // would let closeEvent accept while the thread lives. Ownership moves
    // to onThreadFinished, which is the only place thread_ becomes null.
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

void MainWindow::onThreadFinished() {
    // The only place thread_ becomes null: the QThread object has emitted
    // finished, so no worker code can still be running on it. sender() is
    // checked so a stale signal from a previous job can never clear (or
    // close over) a newer thread object.
    auto* s = qobject_cast<QThread*>(sender());
    if (s && s == thread_) thread_ = nullptr;
    if (closing_ && !thread_) close();
}

void MainWindow::setInputForTest(const QString& p) {
    inputEdit_->setText(p);
}

void MainWindow::setOutputForTest(const QString& p) {
    outputEdit_->setText(p);
}

void MainWindow::setRepresentationForTest(const QString& repr) {
    // Drive the combo exactly like a user selection (signals included), so
    // the lifecycle test exercises the real enable/disable contract too.
    reprCombo_->setCurrentIndex(
        repr.compare(QStringLiteral("mel"), Qt::CaseInsensitive) == 0 ? 1 : 0);
}

void MainWindow::setMelBandsForTest(int bands) {
    melBandsSpin_->setValue(bands);
}

Spectral::GenerateConfig MainWindow::configForTest() const {
    return collectConfig();
}

QString MainWindow::statusTextForTest() const {
    return statusLabel_->text();
}

void MainWindow::openOutput() {
    if (lastOutput_.isEmpty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QFileInfo(lastOutput_).absolutePath()));
}
