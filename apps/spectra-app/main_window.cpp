// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.hpp"

#include <cmath>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "spectra/engine/product.hpp"
#ifdef SPECTRA_HAVE_GPU
#include "spectra/gpu/context.hpp"
#endif
#ifdef SPECTRA_HAVE_IO
#include "spectra/io/raw.hpp"
#endif

namespace spectra::app {

using nlohmann::json;

namespace {

const char* kFileFilter =
    "Images (*.cr2 *.cr3 *.nef *.nrw *.arw *.srf *.sr2 *.raf *.rw2 *.orf *.dng *.pef *.srw *.3fr *.iiq *.erf *.mrw "
    "*.x3f *.tif *.tiff *.png *.jpg *.jpeg *.exr);;All files (*)";

QString dataDir() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(d);
    return d;
}

QString readResource(const QString& name) {
    QFile f(name);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

}  // namespace

void AppSettings::load() {
    QSettings s;
    gpu_device = s.value("engine/gpu_device").toString();
    force_cpu = s.value("engine/force_cpu", false).toBool();
    match_python = s.value("engine/match_python", false).toBool();
    budget_mb = s.value("engine/budget_mb", 0).toInt();
    tile_size = s.value("engine/tile_size", 0).toInt();
    preview_edge = s.value("engine/preview_edge", 0).toInt();
    advanced = s.value("ui/advanced", false).toBool();
}

void AppSettings::save() const {
    QSettings s;
    s.setValue("engine/gpu_device", gpu_device);
    s.setValue("engine/force_cpu", force_cpu);
    s.setValue("engine/match_python", match_python);
    s.setValue("engine/budget_mb", budget_mb);
    s.setValue("engine/tile_size", tile_size);
    s.setValue("engine/preview_edge", preview_edge);
    s.setValue("ui/advanced", advanced);
}

MainWindow::MainWindow(std::shared_ptr<const Pack> pack, const QString& pack_path, bool force_cpu, QWidget* parent)
    : QMainWindow(parent), pack_(std::move(pack)), pack_path_(pack_path) {
    colour_ = std::make_shared<const ColourData>(pack_);
    settings_.load();
    if (force_cpu) settings_.force_cpu = true;
    setWindowTitle(product::kName);
    setAcceptDrops(true);
    resize(1500, 950);

    auto* central = new QWidget;
    auto* col = new QVBoxLayout(central);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    banner_ = new QLabel;
    banner_->setObjectName("banner");
    banner_->setWordWrap(true);
    banner_->setVisible(false);
    preview_ = new PreviewWidget;
    col->addWidget(banner_);
    col->addWidget(preview_, 1);
    setCentralWidget(central);

    buildDocks();
    buildMenus();

    status_device_ = new QLabel(tr("Starting…"));
    status_render_ = new QLabel;
    status_memory_ = new QLabel;
    statusBar()->addWidget(status_render_, 1);
    statusBar()->addPermanentWidget(status_memory_);
    statusBar()->addPermanentWidget(status_device_);

    notice_timer_ = new QTimer(this);
    notice_timer_->setSingleShot(true);
    notice_timer_->setInterval(400);
    connect(notice_timer_, &QTimer::timeout, this, [this] {
        if (!have_image_ || image_is_proxy_) return;
        json p = panel_->params();
        post([c = controller_, p] { c->checkFullRenderNotice(p); });
    });

    connect(preview_, &PreviewWidget::viewChanged, this, &MainWindow::requestPreview);
    connect(preview_, &PreviewWidget::filesDropped, this, &MainWindow::openFiles);
    connect(preview_, &PreviewWidget::actualSizeChanged, this, [this](bool on) { act_actual_->setChecked(on); });
    connect(panel_, &ParamPanel::changed, this, &MainWindow::onParamsChanged);
    connect(panel_, &ParamPanel::stockChanged, this, &MainWindow::onStockChanged);
    connect(panel_, &ParamPanel::rawChanged, this, [this] {
        if (FileEntry* e = current()) {
            e->raw = panel_->rawSettings();
            if (e->is_raw) startDecode();
        }
    });

    QSettings s;
    restoreGeometry(s.value("ui/geometry").toByteArray());
    restoreState(s.value("ui/state").toByteArray());
    startEngine();
}

MainWindow::~MainWindow() {
    if (last_request_) last_request_->store(true);
    controller_->abort = true;
    QMetaObject::invokeMethod(controller_, [c = controller_] { c->shutdown(); }, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
    delete controller_;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (FileEntry* f = current()) saveEdits(*f);
    QSettings s;
    s.setValue("ui/geometry", saveGeometry());
    s.setValue("ui/state", saveState());
    settings_.save();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    openFiles(paths);
}

// ---- construction -------------------------------------------------------------------

void MainWindow::buildDocks() {
    auto* side = new QWidget;
    auto* col = new QVBoxLayout(side);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(4);
    histogram_ = new HistogramWidget;
    panel_ = new ParamPanel(pack_);
    panel_->setAdvanced(settings_.advanced);
    col->addWidget(histogram_);
    col->addWidget(panel_, 1);
    auto* dock = new QDockWidget(tr("Adjustments"), this);
    dock->setObjectName("adjustments");
    dock->setWidget(side);
    dock->setMinimumWidth(380);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    strip_ = new QListWidget;
    strip_->setViewMode(QListView::IconMode);
    strip_->setFlow(QListView::LeftToRight);
    strip_->setWrapping(false);
    strip_->setMovement(QListView::Static);
    strip_->setIconSize(QSize(120, 80));
    strip_->setGridSize(QSize(140, 110));
    strip_->setFixedHeight(134);
    strip_->setSelectionMode(QAbstractItemView::SingleSelection);
    strip_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* sdock = new QDockWidget(tr("Images"), this);
    sdock->setObjectName("images");
    sdock->setWidget(strip_);
    sdock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, sdock);
    connect(strip_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row != current_) selectFile(row);
    });
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open…"), QKeySequence::Open, this, [this] {
        QSettings s;
        QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open images"), s.value("ui/open_dir").toString(),
                                                          tr(kFileFilter));
        if (paths.isEmpty()) return;
        s.setValue("ui/open_dir", QFileInfo(paths.first()).absolutePath());
        openFiles(paths);
    });
    act_export_ = file->addAction(tr("&Export…"), QKeySequence(Qt::CTRL | Qt::Key_E), this, &MainWindow::exportCurrent);
    act_batch_ = file->addAction(tr("Export &all images…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this,
                                 &MainWindow::exportBatch);
    act_lut_ = file->addAction(tr("Export &LUT (.cube)…"), this, &MainWindow::exportLut);
    file->addSeparator();
    file->addAction(tr("&Close image"), QKeySequence(Qt::CTRL | Qt::Key_W), this, [this] {
        if (current_ < 0) return;
        saveEdits(files_[current_]);
        int row = current_;
        files_.removeAt(row);
        current_ = -1;
        delete strip_->takeItem(row);
        if (files_.isEmpty()) {
            have_image_ = false;
            ++decode_token_;
            preview_->setImageSize(0, 0);
            preview_->clearResults();
            post([c = controller_] { c->setImage(nullptr, QString(), false, 0); });
            updateStatus();
        } else {
            strip_->setCurrentRow(std::min(row, static_cast<int>(files_.size()) - 1));
        }
    });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("&Reset all adjustments"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] {
        if (FileEntry* e = current()) {
            e->params = e->baseline;
            panel_->setParams(e->params, e->baseline);
            requestPreview();
        }
    });
    edit->addAction(tr("&Copy adjustments"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this,
                    [this] { clipboard_ = panel_->params(); });
    edit->addAction(tr("&Paste adjustments"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this, [this] {
        if (!clipboard_.is_object() || !current()) return;
        try {
            applyParams(params_from_json(clipboard_));
        } catch (const std::exception& e) {
            QMessageBox::warning(this, product::kName, e.what());
        }
    });
    edit->addSeparator();
    edit->addAction(tr("&Settings…"), QKeySequence::Preferences, this, &MainWindow::showSettings);

    preset_menu_ = menuBar()->addMenu(tr("&Presets"));
    rebuildPresetMenu();

    QMenu* view = menuBar()->addMenu(tr("&View"));
    act_actual_ = view->addAction(tr("&100 % (double-click the image)"));
    act_actual_->setCheckable(true);
    act_actual_->setShortcut(QKeySequence(Qt::Key_Z));
    connect(act_actual_, &QAction::triggered, preview_, &PreviewWidget::setActualSize);
    act_before_ = view->addAction(tr("Show &original (before)"));
    act_before_->setCheckable(true);
    act_before_->setShortcut(QKeySequence(Qt::Key_Backslash));
    connect(act_before_, &QAction::toggled, this, [this](bool) { requestPreview(); });
    QAction* adv = view->addAction(tr("&Advanced parameters"));
    adv->setCheckable(true);
    adv->setChecked(settings_.advanced);
    connect(adv, &QAction::toggled, this, [this](bool on) {
        settings_.advanced = on;
        panel_->setAdvanced(on);
    });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About and licences…"), this, &MainWindow::showAbout);
}

void MainWindow::startEngine() {
    controller_ = new RenderController;
    controller_->moveToThread(&thread_);
    connect(controller_, &RenderController::engineReady, this,
            [this](const QString& device, const QString& id, const QString& note, bool gpu, bool calibrated) {
                engine_ready_ = true;
                gpu_ = gpu;
                device_ = device;
                device_id_ = id;
                engine_note_ = note;
                post([c = controller_, s = settings_] {
                    c->setOverrides(s.match_python, quint64(s.budget_mb) << 20, s.tile_size);
                    if (s.preview_edge > 0) c->setPreviewSizes(s.preview_edge, 0);
                });
                if (gpu && !calibrated) {
                    // First launch on this GPU: measure it once and remember the result.
                    calibrating_ = true;
                    post([c = controller_] { c->calibrate(); });
                }
                setBanner(QString());
                updateStatus();
                Q_EMIT engineStarted();
                requestPreview();
            });
    connect(controller_, &RenderController::engineFailed, this, [this](const QString& m) {
        setBanner(tr("The rendering engine could not start: %1").arg(m));
    });
    connect(controller_, &RenderController::previewReady, this, &MainWindow::onPreviewReady);
    connect(controller_, &RenderController::previewFailed, this, [this](const QString& m, quint64) {
        status_render_->setText(tr("Render failed: %1").arg(m));
    });
    connect(controller_, &RenderController::calibrationProgress, this,
            [this](const QString& line) { status_render_->setText(tr("Calibrating: %1").arg(line)); });
    connect(controller_, &RenderController::calibrationDone, this, [this](const QString& id, const QString& js) {
        calibrating_ = false;
        if (!id.isEmpty()) {
            QSettings s;
            json all = json::parse(s.value("engine/calibration", "{}").toString().toStdString(), nullptr, false);
            if (!all.is_object()) all = json::object();
            all[id.toStdString()] = json::parse(js.toStdString(), nullptr, false);
            s.setValue("engine/calibration", QString::fromStdString(all.dump()));
            if (settings_.preview_edge > 0)
                post([c = controller_, e = settings_.preview_edge] { c->setPreviewSizes(e, 0); });
        }
        updateStatus();
        Q_EMIT calibrationFinished();
        requestPreview();
    });
    connect(controller_, &RenderController::noticeChanged, this, [this](const QString& text) {
        notice_ = text;
        setBanner(QString());
    });
    connect(controller_, &RenderController::exportProgress, this,
            [this](int file, int files, int step, int steps, const QString& what) {
                if (!progress_) return;
                progress_->setMaximum(files * 100);
                progress_->setValue(file * 100 + (steps > 0 ? step * 100 / steps : 0));
                progress_->setLabelText(files > 1 ? tr("Exporting %1 (%2 of %3)…").arg(what).arg(file + 1).arg(files)
                                                  : tr("Exporting %1…").arg(what));
            });
    connect(controller_, &RenderController::exportFinished, this, [this](int ok, int failed, const QString& message) {
        if (progress_) {
            progress_->reset();
            progress_->deleteLater();
            progress_ = nullptr;
        }
        QString text = failed == 0 && message.isEmpty() ? tr("Exported %n file(s).", nullptr, ok) : message;
        status_render_->setText(text.section('\n', 0, 0));
        if (failed > 0 && isVisible() && QApplication::platformName() != "offscreen")
            QMessageBox::warning(this, product::kName, text);
        Q_EMIT exportDone(ok, failed, message);
        requestPreview();  // the full render released the preview caches
    });
    thread_.setObjectName("render");
    thread_.start();

    engine::EngineConfig cfg;
    cfg.gpu_device = settings_.gpu_device.toStdString();
    cfg.force_cpu = settings_.force_cpu;
    cfg.match_python = settings_.match_python;
    QString cached = QSettings().value("engine/calibration").toString();
    post([c = controller_, path = pack_path_, cfg, cached] { c->init(path, cfg, cached); });
}

// ---- files --------------------------------------------------------------------------

QString MainWindow::editsPath(const QString& file) const {
    QByteArray h = QCryptographicHash::hash(QFileInfo(file).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex();
    QString dir = dataDir() + "/edits";
    QDir().mkpath(dir);
    return dir + "/" + QString::fromLatin1(h) + ".json";
}

void MainWindow::saveEdits(const FileEntry& e) const {
    if (e.path.isEmpty()) return;
    try {
        json doc = {{"file", e.path.toStdString()},
                    {"params", engine::params_diff(params_from_json(e.params))},
                    {"raw", {{"white_balance", e.raw.white_balance.toStdString()},
                             {"temperature", e.raw.temperature},
                             {"tint", e.raw.tint}}}};
        QFile f(editsPath(e.path));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QByteArray::fromStdString(doc.dump(1)));
    } catch (const std::exception&) {
    }
}

bool MainWindow::loadEdits(FileEntry& e) const {
    QFile f(editsPath(e.path));
    if (!f.open(QIODevice::ReadOnly)) return false;
    try {
        json doc = json::parse(f.readAll().toStdString());
        Params p = engine::params_from_preset(*pack_, doc.at("params"));
        e.params = params_to_json(p);
        e.baseline = params_to_json(engine::default_user_params(*pack_, p.film, p.print));
        const json& r = doc.at("raw");
        e.raw.white_balance = QString::fromStdString(r.value("white_balance", "as_shot"));
        e.raw.temperature = r.value("temperature", 5500.0);
        e.raw.tint = r.value("tint", 1.0);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void MainWindow::openFiles(const QStringList& paths) {
    int first_new = -1;
    for (const QString& path : paths) {
        QFileInfo fi(path);
        if (!fi.isFile()) continue;
        int existing = -1;
        for (int i = 0; i < files_.size(); ++i)
            if (QFileInfo(files_[i].path) == fi) existing = i;
        if (existing >= 0) {
            if (first_new < 0) first_new = existing;
            continue;
        }
        FileEntry e;
        e.path = fi.absoluteFilePath();
#ifdef SPECTRA_HAVE_IO
        e.is_raw = is_raw_file(e.path.toStdString());
#endif
        if (!loadEdits(e)) {
            // New images start from the settings of the image being worked on.
            if (FileEntry* cur = current()) {
                e.params = panel_->params();
                e.baseline = cur->baseline;
            } else {
                Params def;
                e.params = params_to_json(engine::default_user_params(*pack_, def.film, def.print));
                e.baseline = e.params;
            }
        }
        files_.push_back(e);
        auto* item = new QListWidgetItem(fi.fileName());
        item->setToolTip(e.path);
        item->setSizeHint(QSize(136, 106));
        strip_->addItem(item);
        if (first_new < 0) first_new = static_cast<int>(files_.size()) - 1;
    }
    if (first_new >= 0) strip_->setCurrentRow(first_new);
}

void MainWindow::selectFile(int index) {
    if (FileEntry* prev = current()) {
        prev->params = panel_->params();
        saveEdits(*prev);
    }
    current_ = index;
    FileEntry* e = current();
    if (!e) return;
    panel_->setParams(e->params, e->baseline);
    panel_->setRawSettings(e->raw);
    panel_->setRawEnabled(e->is_raw);
    setWindowTitle(QString("%1 - %2").arg(QFileInfo(e->path).fileName(), product::kName));
    startDecode();
}

void MainWindow::startDecode() {
    FileEntry* e = current();
    if (!e || e->path.isEmpty()) return;
#ifdef SPECTRA_HAVE_IO
    quint64 token = ++decode_token_;
    have_image_ = false;
    preview_->setMessage(tr("Loading %1…").arg(QFileInfo(e->path).fileName()));
    QString path = e->path;
    RawSettings raw = e->raw;
    bool is_raw = e->is_raw;
    auto colour = colour_;
    // Decoding runs on a pool thread and overlaps the GPU work: a fast
    // half-size decode gives the first preview while the full decode runs.
    QThreadPool::globalInstance()->start([this, token, path, raw, is_raw, colour] {
        auto deliver = [this, token](std::shared_ptr<const Image> img, const QString& space, bool proxy, const QString& info) {
            QMetaObject::invokeMethod(this, [=, this] { onDecoded(token, img, space, proxy, info); }, Qt::QueuedConnection);
        };
        try {
            RawLoadOptions ro;
            if (raw.white_balance == "daylight") ro.white_balance = RawWhiteBalance::Daylight;
            else if (raw.white_balance == "tungsten") ro.white_balance = RawWhiteBalance::Tungsten;
            else if (raw.white_balance == "custom") ro.white_balance = RawWhiteBalance::Custom;
            ro.temperature = raw.temperature;
            ro.tint = raw.tint;
            std::string space;
            RawInfo info;
            auto describe = [&] {
                if (info.make.empty()) return QString();
                return QString("%1 %2, ISO %3").arg(QString::fromStdString(info.make), QString::fromStdString(info.model))
                    .arg(info.iso, 0, 'f', 0);
            };
            if (is_raw) {
                ro.half_size = true;
                auto proxy = std::make_shared<const Image>(load_any(*colour, path.toStdString(), ro, &space, &info));
                deliver(proxy, QString::fromStdString(space), true, describe());
                ro.half_size = false;
            }
            auto full = std::make_shared<const Image>(load_any(*colour, path.toStdString(), ro, &space, &info));
            deliver(full, QString::fromStdString(space), false, describe());
        } catch (const std::exception& ex) {
            QString msg = QString::fromUtf8(ex.what());
            QMetaObject::invokeMethod(this, [this, token, msg, path] {
                if (token != decode_token_) return;
                preview_->setMessage(tr("Could not open %1: %2").arg(QFileInfo(path).fileName(), msg));
            }, Qt::QueuedConnection);
        }
    });
#else
    preview_->setMessage(tr("This build has no image file support."));
#endif
}

void MainWindow::onDecoded(quint64 token, std::shared_ptr<const Image> image, const QString& space, bool proxy,
                           const QString& info) {
    if (token != decode_token_ || !image) return;
    have_image_ = true;
    image_is_proxy_ = proxy;
    decode_info_ = info;
    preview_->setMessage(proxy ? tr("Loading full resolution…") : QString());
    preview_->setImageSize(image->w, image->h);
    post([c = controller_, image, space, proxy, token] { c->setImage(image, space, proxy, token); });
    notice_.clear();
    setBanner(QString());
    requestPreview();
    updateStatus();
}

void MainWindow::loadSynthetic(int w, int h) {
    auto img = std::make_shared<Image>(h, w, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            double base = 0.184 * std::pow(2.0, 6.0 * (u - 0.5));
            double tex = 0.85 + 0.3 * std::sin(x * 0.11) * std::cos(y * 0.07);
            img->at(y, x, 0) = base * tex * (0.6 + 0.8 * v);
            img->at(y, x, 1) = base * tex;
            img->at(y, x, 2) = base * tex * (1.4 - 0.8 * v);
        }
    FileEntry e;
    Params def;
    e.params = params_to_json(engine::default_user_params(*pack_, def.film, def.print));
    e.baseline = e.params;
    files_.push_back(e);
    strip_->blockSignals(true);
    strip_->addItem(tr("Test picture"));
    strip_->setCurrentRow(static_cast<int>(files_.size()) - 1);
    strip_->blockSignals(false);
    current_ = static_cast<int>(files_.size()) - 1;
    panel_->setParams(e.params, e.baseline);
    panel_->setRawEnabled(false);
    onDecoded(++decode_token_, img, QString(), false, tr("test picture"));
}

// ---- parameters and preview -------------------------------------------------------------

void MainWindow::setParameter(const std::string& pointer, const json& value) {
    json doc = panel_->params();
    doc[json::json_pointer(pointer)] = value;
    if (FileEntry* e = current()) {
        e->params = doc;
        panel_->setParams(doc, e->baseline);
    }
    requestPreview();
}

void MainWindow::applyParams(const Params& p) {
    FileEntry* e = current();
    if (!e) return;
    e->params = params_to_json(p);
    e->baseline = params_to_json(engine::default_user_params(*pack_, p.film, p.print));
    panel_->setParams(e->params, e->baseline);
    requestPreview();
}

void MainWindow::onStockChanged() {
    // As in the reference GUI: choosing a stock loads that stock's grain,
    // halation and coupler settings; every other adjustment is kept.
    try {
        Params p = params_from_json(panel_->params());
        engine::apply_stock_specifics(*pack_, p);
        applyParams(p);
    } catch (const std::exception& e) {
        status_render_->setText(QString::fromUtf8(e.what()));
    }
}

void MainWindow::onParamsChanged() {
    if (FileEntry* e = current()) e->params = panel_->params();
    requestPreview();
}

void MainWindow::requestPreview() {
    if (!engine_ready_ || !have_image_) return;
    if (last_request_) last_request_->store(true);  // the render thread drops or cuts short the older one
    PreviewRequest rq;
    rq.params = panel_->params();
    rq.view = preview_->currentView();
    rq.viewport = preview_->viewportLongEdge();
    rq.generation = ++generation_;
    rq.show_input = act_before_->isChecked();
    last_request_ = rq.superseded;
    post([c = controller_, rq] { c->requestPreview(rq); });
    notice_timer_->start();
}

void MainWindow::onPreviewReady(const QImage& image, const PreviewInfo& info) {
    if (info.generation < shown_generation_) return;  // older than what is on screen
    shown_generation_ = info.generation;
    preview_->showResult(image, info.rect);
    histogram_->setData(info.histogram);
    static const char* stages[] = {"everything", "film development on", "grain on", "print on", "scan on", "cached"};
    QString text;
    if (info.input_view) {
        text = tr("Original (unprocessed)");
    } else {
        text = tr("%1 %2×%3 in %4 ms").arg(info.draft ? tr("Draft") : tr("Preview")).arg(image.width())
                   .arg(image.height()).arg(info.ms, 0, 'f', 0);
        if (info.gpu) text += tr(" (re-rendered: %1)").arg(tr(stages[std::clamp(info.first_stage, 0, 5)]));
        if (!info.notes.isEmpty()) text += "  ·  " + info.notes.join("; ");
    }
    if (!calibrating_) status_render_->setText(text);
    status_memory_->setText(info.gpu ? tr("GPU memory %1 MiB").arg(info.gpu_bytes >> 20) : QString());
    if (!info.draft && info.rect.empty() && !info.input_view && current_ >= 0 && current_ < strip_->count())
        strip_->item(current_)->setIcon(QIcon(QPixmap::fromImage(
            image.scaled(240, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
    Q_EMIT previewShown(info.draft);
}

void MainWindow::setBanner(const QString& text) {
    QStringList lines;
    if (!text.isEmpty()) lines << text;
    if (!engine_note_.isEmpty()) lines << engine_note_;
    if (!notice_.isEmpty()) lines << notice_;
    banner_->setText(lines.join("\n"));
    banner_->setVisible(!lines.isEmpty());
}

void MainWindow::updateStatus() {
    status_device_->setText(engine_ready_ ? device_ : tr("Starting…"));
    bool can_export = have_image_ && !image_is_proxy_;
    act_export_->setEnabled(can_export);
    act_batch_->setEnabled(!files_.isEmpty() && engine_ready_);
    act_lut_->setEnabled(engine_ready_);
}

// ---- export -----------------------------------------------------------------------------

namespace {

// Format, bit depth, size and quality of an export.
struct ExportChoice {
    QString format = "tif";
    engine::ExportOptions out;
    bool accepted = false;
};

ExportChoice askExportOptions(QWidget* parent, const QString& fixed_format, bool hdr) {
    QDialog d(parent);
    d.setWindowTitle(QObject::tr("Export options"));
    auto* form = new QFormLayout(&d);
    auto* format = new QComboBox;
    format->addItem("TIFF (.tif)", "tif");
    format->addItem("PNG (.png)", "png");
    format->addItem("JPEG (.jpg)", "jpg");
    format->addItem("OpenEXR (.exr)", "exr");
    if (!fixed_format.isEmpty()) {
        int i = format->findData(fixed_format);
        format->setCurrentIndex(i < 0 ? 0 : i);
        format->setEnabled(false);
    }
    auto* depth = new QComboBox;
    auto* quality = new QSpinBox;
    quality->setRange(50, 100);
    quality->setValue(95);
    auto* edge = new QSpinBox;
    edge->setRange(0, 30000);
    edge->setSingleStep(100);
    edge->setSpecialValueText(QObject::tr("Full size"));
    edge->setSuffix(QObject::tr(" px"));
    auto refresh = [&] {
        QString f = format->currentData().toString();
        depth->clear();
        if (f == "tif") {
            depth->addItem(QObject::tr("16-bit"), 16);
            depth->addItem(QObject::tr("8-bit"), 8);
            depth->addItem(QObject::tr("32-bit float"), 32);
        } else if (f == "png") {
            depth->addItem(QObject::tr("16-bit"), 16);
            depth->addItem(QObject::tr("8-bit"), 8);
        } else if (f == "jpg") {
            depth->addItem(QObject::tr("8-bit"), 8);
        } else {
            depth->addItem(QObject::tr("16-bit half float"), 16);
            depth->addItem(QObject::tr("32-bit float"), 32);
        }
        quality->setEnabled(f == "jpg");
    };
    QObject::connect(format, &QComboBox::currentIndexChanged, &d, refresh);
    refresh();
    form->addRow(QObject::tr("Format"), format);
    form->addRow(QObject::tr("Bit depth"), depth);
    form->addRow(QObject::tr("JPEG quality"), quality);
    form->addRow(QObject::tr("Long edge"), edge);
    auto* note = new QLabel(hdr ? QObject::tr("HDR export is on: use 16-bit or float output.")
                                : QObject::tr("The colour space is set under \"Input and output colour\". Files are "
                                              "written without an embedded ICC profile."));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    ExportChoice c;
    if (d.exec() != QDialog::Accepted) return c;
    c.accepted = true;
    c.format = format->currentData().toString();
    int bits = depth->currentData().toInt();
    c.out.bits = bits;
    c.out.is_float = (c.format == "tif" && bits == 32);
    c.out.jpeg_quality = quality->value();
    c.out.long_edge = edge->value();
    return c;
}

}  // namespace

void MainWindow::runExport(const QVector<ExportJob>& jobs) {
    if (jobs.isEmpty()) return;
    if (QApplication::platformName() != "offscreen") {
        progress_ = new QProgressDialog(tr("Exporting…"), tr("Cancel"), 0, jobs.size() * 100, this);
        progress_->setWindowModality(Qt::WindowModal);
        progress_->setMinimumDuration(300);
        progress_->setAutoClose(false);
        progress_->setAutoReset(false);
        connect(progress_, &QProgressDialog::canceled, this, [this] { controller_->abort = true; });
    }
    if (last_request_) last_request_->store(true);
    post([c = controller_, jobs] { c->exportFiles(jobs); });
}

void MainWindow::exportCurrentTo(const QString& path) {
    ExportJob job;
    job.out_path = path;
    job.params = panel_->params();
    runExport({job});
}

void MainWindow::exportLutTo(const QString& path) {
    engine::LutOptions lo;
    lo.size = 17;
    json p = panel_->params();
    post([c = controller_, p, lo, path] { c->exportLut(p, lo, path); });
}

void MainWindow::exportCurrent() {
    FileEntry* e = current();
    if (!e || !have_image_) return;
    if (!notice_.isEmpty() && notice_.contains("diffusion")) {
        QMessageBox::information(this, product::kName, notice_);
        return;
    }
    bool hdr = panel_->params().at("extended").at("hdr_output").at("active").get<bool>();
    ExportChoice c = askExportOptions(this, QString(), hdr);
    if (!c.accepted) return;
    QSettings s;
    QString base = e->path.isEmpty() ? "export" : QFileInfo(e->path).completeBaseName();
    QString dir = s.value("ui/export_dir", e->path.isEmpty() ? QDir::homePath() : QFileInfo(e->path).absolutePath()).toString();
    QString path = QFileDialog::getSaveFileName(this, tr("Export"), dir + "/" + base + "." + c.format,
                                                QString("*.%1").arg(c.format));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += "." + c.format;
    s.setValue("ui/export_dir", QFileInfo(path).absolutePath());
    ExportJob job;
    job.out_path = path;
    job.params = panel_->params();
    job.out = c.out;
    runExport({job});
}

void MainWindow::exportBatch() {
    if (files_.isEmpty()) return;
    if (FileEntry* cur = current()) cur->params = panel_->params();
    QDialog d(this);
    d.setWindowTitle(tr("Export all images"));
    auto* form = new QFormLayout(&d);
    auto* which = new QComboBox;
    which->addItem(tr("Each image with its own adjustments"), 0);
    which->addItem(tr("All images with the current adjustments"), 1);
    form->addRow(tr("Adjustments"), which);
    auto* note = new QLabel(tr("%n image(s) will be rendered at full resolution, one after another.", nullptr,
                               static_cast<int>(files_.size())));
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    bool hdr = panel_->params().at("extended").at("hdr_output").at("active").get<bool>();
    ExportChoice c = askExportOptions(this, QString(), hdr);
    if (!c.accepted) return;
    QSettings s;
    QString dir = QFileDialog::getExistingDirectory(this, tr("Export folder"), s.value("ui/export_dir").toString());
    if (dir.isEmpty()) return;
    s.setValue("ui/export_dir", dir);
    QVector<ExportJob> jobs;
    for (const FileEntry& e : files_) {
        if (e.path.isEmpty()) continue;
        ExportJob job;
        job.in_path = e.path;
        job.out_path = dir + "/" + QFileInfo(e.path).completeBaseName() + "." + c.format;
        job.params = which->currentData().toInt() == 1 ? panel_->params() : e.params;
        job.white_balance = e.raw.white_balance;
        job.temperature = e.raw.temperature;
        job.tint = e.raw.tint;
        job.out = c.out;
        jobs.push_back(job);
    }
    runExport(jobs);
}

void MainWindow::exportLut() {
    QDialog d(this);
    d.setWindowTitle(tr("Export LUT"));
    auto* form = new QFormLayout(&d);
    auto* size = new QComboBox;
    for (int n : {17, 33, 65}) size->addItem(QString("%1 × %1 × %1").arg(n), n);
    size->setCurrentIndex(1);
    auto* input = new QComboBox;
    input->addItem(tr("sRGB (display-referred)"), "srgb");
    input->addItem(tr("Rec.709, gamma 2.4"), "rec709");
    input->addItem(tr("Display P3"), "display_p3");
    input->addItem(tr("ACEScct (scene-referred)"), "acescct");
    form->addRow(tr("Size"), size);
    form->addRow(tr("LUT input"), input);
    auto* note = new QLabel(tr("A LUT holds the colour and tone of the current film, print and adjustments. Grain, "
                               "halation, blur, glare and autoexposure cannot be part of a LUT and are left out. The "
                               "output uses the export colour space."));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QSettings s;
    json p = panel_->params();
    QString name = QString::fromStdString(p.at("film").get<std::string>()) + ".cube";
    QString path = QFileDialog::getSaveFileName(this, tr("Export LUT"),
                                                s.value("ui/export_dir", QDir::homePath()).toString() + "/" + name, "*.cube");
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".cube";
    engine::LutOptions lo;
    lo.size = size->currentData().toInt();
    lo.input = input->currentData().toString().toStdString();
    status_render_->setText(tr("Writing LUT…"));
    post([c = controller_, p, lo, path] { c->exportLut(p, lo, path); });
}

// ---- presets ------------------------------------------------------------------------------

QString MainWindow::presetsDir() const {
    QString d = dataDir() + "/presets";
    QDir().mkpath(d);
    return d;
}

void MainWindow::rebuildPresetMenu() {
    preset_menu_->clear();
    preset_menu_->addAction(tr("&Save current adjustments as preset…"), this, &MainWindow::savePreset);
    preset_menu_->addAction(tr("&Import preset…"), this, [this] {
        QString path = QFileDialog::getOpenFileName(this, tr("Import preset"), QString(), "*.json");
        if (path.isEmpty()) return;
        QString target = presetsDir() + "/" + QFileInfo(path).fileName();
        QFile::remove(target);
        if (QFile::copy(path, target)) rebuildPresetMenu();
        applyPresetFile(target);
    });
    preset_menu_->addAction(tr("&Open presets folder"), this,
                            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(presetsDir())); });
    preset_menu_->addSeparator();
    QMenu* builtin = preset_menu_->addMenu(tr("Film and print pairings"));
    for (const engine::Preset& p : engine::builtin_presets()) {
        QAction* a = builtin->addAction(QString::fromStdString(p.name));
        a->setToolTip(QString::fromStdString(p.description));
        json diff = p.diff;
        connect(a, &QAction::triggered, this, [this, diff] {
            try {
                applyParams(engine::params_from_preset(*pack_, diff));
            } catch (const std::exception& e) {
                QMessageBox::warning(this, product::kName, e.what());
            }
        });
    }
    QDir dir(presetsDir());
    QStringList files = dir.entryList({"*.json"}, QDir::Files, QDir::Name);
    if (!files.isEmpty()) preset_menu_->addSeparator();
    for (const QString& f : files) {
        QString path = dir.filePath(f);
        QString name = QFileInfo(f).completeBaseName();
        preset_menu_->addAction(name, this, [this, path] { applyPresetFile(path); });
    }
}

void MainWindow::applyPresetFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    try {
        engine::Preset p = engine::preset_from_json(json::parse(f.readAll().toStdString()));
        applyParams(engine::params_from_preset(*pack_, p.diff));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, product::kName, tr("This preset could not be loaded: %1").arg(e.what()));
    }
}

void MainWindow::savePreset() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Save preset"), tr("Preset name"), QLineEdit::Normal, QString(), &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty()) return;
    QString safe = name;
    safe.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
    engine::Preset p;
    p.name = name.toStdString();
    p.diff = engine::params_diff(params_from_json(panel_->params()));
    QFile f(presetsDir() + "/" + safe + ".json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QByteArray::fromStdString(engine::preset_to_json(p).dump(1)));
        f.close();
        rebuildPresetMenu();
    } else {
        QMessageBox::warning(this, product::kName, tr("The preset could not be saved."));
    }
}

// ---- settings and about -------------------------------------------------------------------

void MainWindow::showSettings() {
    QDialog d(this);
    d.setWindowTitle(tr("Settings"));
    d.setMinimumWidth(560);
    auto* form = new QFormLayout(&d);
    auto* device = new QComboBox;
    device->addItem(tr("Automatic (best available)"), QString());
#ifdef SPECTRA_HAVE_GPU
    try {
        for (const auto& info : gpu::Context::enumerate())
            device->addItem(QString::fromStdString(info.name) + (info.is_software() ? tr("  (software)") : QString()),
                            QString::fromStdString(info.name));
    } catch (const std::exception&) {
    }
#endif
    int di = device->findData(settings_.gpu_device);
    device->setCurrentIndex(di < 0 ? 0 : di);
    auto* cpu = new QCheckBox(tr("Render on the CPU (slow; the reference implementation)"));
    cpu->setChecked(settings_.force_cpu);
    auto* match = new QCheckBox(tr("Match Python exactly"));
    match->setToolTip(tr("Off (default): the look is the same at every resolution; parameters given in pixels are "
                         "scaled to the film format. On: reproduces the Python reference, including its dependence "
                         "on image resolution."));
    match->setChecked(settings_.match_python);
    auto* budget = new QSpinBox;
    budget->setRange(0, 65536);
    budget->setSingleStep(256);
    budget->setSpecialValueText(tr("Automatic"));
    budget->setSuffix(tr(" MiB"));
    budget->setValue(settings_.budget_mb);
    budget->setToolTip(tr("GPU memory a render may use. Larger images are rendered in tiles."));
    auto* tile = new QSpinBox;
    tile->setRange(0, 16384);
    tile->setSingleStep(256);
    tile->setSpecialValueText(tr("Automatic"));
    tile->setSuffix(tr(" px"));
    tile->setValue(settings_.tile_size);
    tile->setToolTip(tr("Force full renders into tiles of this size."));
    auto* edge = new QSpinBox;
    edge->setRange(0, 4096);
    edge->setSingleStep(128);
    edge->setSpecialValueText(tr("Automatic (calibrated)"));
    edge->setSuffix(tr(" px"));
    edge->setValue(settings_.preview_edge);
    form->addRow(tr("Graphics device"), device);
    form->addRow(QString(), cpu);
    form->addRow(QString(), match);
    form->addRow(tr("GPU memory budget"), budget);
    form->addRow(tr("Tile size"), tile);
    form->addRow(tr("Preview size (long edge)"), edge);

    auto* log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setMinimumHeight(150);
    {
        json all = json::parse(QSettings().value("engine/calibration", "{}").toString().toStdString(), nullptr, false);
        if (all.is_object() && all.contains(device_id_.toStdString())) {
            engine::Calibration c = engine::Calibration::from_json(all.at(device_id_.toStdString()));
            for (auto& line : c.log) log->appendPlainText(QString::fromStdString(line));
        } else {
            log->setPlainText(gpu_ ? tr("No calibration stored for %1 yet.").arg(device_)
                                   : tr("Calibration applies to GPU rendering."));
        }
    }
    auto* recal = new QPushButton(tr("Re-run performance calibration"));
    recal->setEnabled(gpu_ && !calibrating_);
    connect(recal, &QPushButton::clicked, &d, [this, log, recal] {
        log->clear();
        recal->setEnabled(false);
        calibrating_ = true;
        if (last_request_) last_request_->store(true);
        post([c = controller_] { c->calibrate(); });
    });
    auto c1 = connect(controller_, &RenderController::calibrationProgress, log, &QPlainTextEdit::appendPlainText);
    auto c2 = connect(controller_, &RenderController::calibrationDone, recal, [recal] { recal->setEnabled(true); });
    form->addRow(tr("Performance"), recal);
    form->addRow(log);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    int rc = d.exec();
    disconnect(c1);
    disconnect(c2);
    if (rc != QDialog::Accepted) return;

    bool restart = device->currentData().toString() != settings_.gpu_device || cpu->isChecked() != settings_.force_cpu;
    settings_.gpu_device = device->currentData().toString();
    settings_.force_cpu = cpu->isChecked();
    settings_.match_python = match->isChecked();
    settings_.budget_mb = budget->value();
    settings_.tile_size = tile->value();
    settings_.preview_edge = edge->value();
    settings_.save();
    if (restart) {
        engine_ready_ = false;
        updateStatus();
        engine::EngineConfig cfg;
        cfg.gpu_device = settings_.gpu_device.toStdString();
        cfg.force_cpu = settings_.force_cpu;
        cfg.match_python = settings_.match_python;
        QString cached = QSettings().value("engine/calibration").toString();
        if (last_request_) last_request_->store(true);
        post([c = controller_, path = pack_path_, cfg, cached] { c->init(path, cfg, cached); });
    } else {
        post([c = controller_, s = settings_] {
            c->setOverrides(s.match_python, quint64(s.budget_mb) << 20, s.tile_size);
            c->setPreviewSizes(s.preview_edge, 0);
        });
        requestPreview();
    }
}

void MainWindow::showAbout() {
    QDialog d(this);
    d.setWindowTitle(tr("About %1").arg(product::kName));
    d.resize(760, 620);
    auto* col = new QVBoxLayout(&d);
    auto* text = new QTextBrowser;
    text->setOpenExternalLinks(true);
    QString commit, version;
    try {
        const json& prov = pack_->meta().at("provenance");
        commit = QString::fromStdString(prov.value("upstream_commit", ""));
        version = QString::fromStdString(prov.value("spektrafilm_version", ""));
    } catch (const std::exception&) {
    }
    QString html = QString(
        "<h2>%1 %2</h2>"
        "<p>A GPU-accelerated port of the <a href='https://github.com/andreavolpato/spektrafilm'>spektrafilm</a> "
        "spectral film simulation by Andrea Volpato.</p>"
        "<p><b>This program is free software</b>, licensed under the GNU General Public License, version 3 or "
        "(at your option) any later version. It comes with no warranty.</p>"
        "<h3>Attribution</h3><ul>"
        "<li>The film model, its equations and parameters are spektrafilm's (GPL-3.0). This port follows "
        "spektrafilm %3, commit <code>%4</code>.</li>"
        "<li>The film and paper profiles and the other data in the data pack are from spektrafilm and are licensed "
        "<a href='https://creativecommons.org/licenses/by-sa/4.0/'>CC BY-SA 4.0</a>, © Andrea Volpato and "
        "contributors.</li>"
        "<li>The Extended features (push / pull, bleach bypass, HDR output) are adapted from "
        "<a href='https://github.com/chaert-s/spektrafilm-ofx'>spektrafilm-ofx</a> (GPL-3.0). They are not part of "
        "spektrafilm.</li>"
        "<li>This program is not affiliated with or endorsed by the spektrafilm project. Film and paper names are "
        "trademarks of their owners and are used to identify the simulated materials.</li></ul>"
        "<h3>Third-party software</h3><pre style='white-space: pre-wrap'>%5</pre>"
        "<h3>spektrafilm licence notice</h3><pre style='white-space: pre-wrap'>%6</pre>")
        .arg(product::kName, product::kVersion, version, commit,
             readResource(":/THIRD_PARTY_NOTICES.md").toHtmlEscaped(),
             readResource(":/SPEKTRAFILM_LICENSE.txt").left(6000).toHtmlEscaped());
    text->setHtml(html);
    col->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    col->addWidget(buttons);
    d.exec();
}

}  // namespace spectra::app
