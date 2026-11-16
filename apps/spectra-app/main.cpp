// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdarg>
#include <cstdio>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>

#include "main_window.hpp"
#include "spectra/engine/product.hpp"

using namespace spectra;

namespace {

QString findPack() {
    QStringList candidates;
    if (qEnvironmentVariableIsSet("SPECTRA_PACK")) candidates << qEnvironmentVariable("SPECTRA_PACK");
    QString app = QCoreApplication::applicationDirPath();
    candidates << app + "/data/spektrafilm.sfpk" << app + "/../share/spectra-app/data/spektrafilm.sfpk"
               << app + "/../data/spektrafilm.sfpk" << app + "/../../data/spektrafilm.sfpk"
               << QStringLiteral(SPECTRA_SOURCE_DIR "/data/spektrafilm.sfpk");
    for (const QString& c : candidates)
        if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
    return {};
}

void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(43, 43, 46), base(30, 30, 32), text(225, 225, 225), mid(62, 62, 66), accent(214, 138, 56);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, mid);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::PlaceholderText, QColor(140, 140, 140));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, QColor(120, 120, 120));
    app.setPalette(p);
    app.setStyleSheet(
        "QToolButton#sectionHeader { border: none; border-top: 1px solid #1c1c1e; background: #37373b; padding: 6px 6px;"
        "  font-weight: 600; text-align: left; }"
        "QToolButton#sectionHeader:hover { background: #404045; }"
        "QLabel#sectionNote { color: #c9a26b; }"
        "QLabel#banner { background: #5a4a1c; color: #fff2cc; padding: 8px 12px; }"
        "QToolTip { border: 1px solid #555; padding: 4px; }"
        "QDockWidget::title { padding: 6px; background: #2b2b2e; }"
        "QListWidget { background: #1e1e20; }");
}

// Self-test messages go to stdout and to <dir>/selftest.log (a Windows GUI
// program has no console to print to).
FILE* g_selftest_log = nullptr;
void say(const char* fmt, ...) {
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    std::vprintf(fmt, a);
    std::fflush(stdout);
    if (g_selftest_log) {
        std::vfprintf(g_selftest_log, fmt, b);
        std::fflush(g_selftest_log);
    }
    va_end(a);
    va_end(b);
}

// Drives the window through open -> preview -> adjust -> export without a
// person, for CI: exit code 0 only if every step produced its result.
class SelfTest : public QObject {
public:
    SelfTest(app::MainWindow* w, const QString& out_dir, const QString& screenshot)
        : w_(w), out_(out_dir), shot_(screenshot) {
        QDir().mkpath(out_);
        g_selftest_log = std::fopen((out_ + "/selftest.log").toLocal8Bit().constData(), "w");
        say("selftest: started, platform %s\n", QGuiApplication::platformName().toUtf8().constData());
        connect(w_, &app::MainWindow::engineError, this, [this](const QString& m) {
            say("selftest: engine FAILED: %s\n", m.toUtf8().constData());
            finish(1);
        });
        connect(w_, &app::MainWindow::engineStarted, this, [this] {
            say("selftest: engine started (%s)\n", w_->engineHasGpu() ? "GPU" : "CPU");
            if (!w_->engineHasGpu()) begin();
        });
        connect(w_, &app::MainWindow::calibrationFinished, this, [this] {
            say("selftest: calibration finished\n");
            begin();
        });
        connect(w_, &app::MainWindow::previewShown, this, [this](bool draft) {
            if (draft) {
                ++drafts_;
                return;
            }
            ++refined_;
            if (step_ == 1) {
                say("selftest: first preview shown (%d draft, %d refined)\n", drafts_, refined_);
                step_ = 2;
                w_->setParameter("/enlarger/print_exposure", 1.25);
            } else if (step_ == 2) {
                say("selftest: preview after a parameter change shown\n");
                if (!shot_.isEmpty()) w_->grab().save(shot_);
                step_ = 3;
                w_->exportCurrentTo(out_ + "/selftest.png");
            }
        });
        connect(w_, &app::MainWindow::exportDone, this, [this](int ok, int failed, const QString& message) {
            if (step_ == 3) {
                bool good = ok == 1 && failed == 0 && QFileInfo(out_ + "/selftest.png").size() > 1000;
                say("selftest: export %s %s\n", good ? "ok" : "FAILED", message.toUtf8().constData());
                if (!good) return finish(1);
                step_ = 4;
                w_->exportLutTo(out_ + "/selftest.cube");
            } else if (step_ == 4) {
                bool good = ok == 1 && QFileInfo(out_ + "/selftest.cube").size() > 1000;
                say("selftest: LUT export %s\n", good ? "ok" : "FAILED");
                finish(good && drafts_ > 0 ? 0 : 1);
            }
        });
        QTimer::singleShot(600000, this, [this] {
            say("selftest: TIMEOUT at step %d\n", step_);
            finish(1);
        });
    }

private:
    void begin() {
        if (step_ != 0) return;
        step_ = 1;
        w_->loadSynthetic(1200, 800);
    }
    void finish(int code) {
        std::fflush(stdout);
        QTimer::singleShot(0, [code] { QCoreApplication::exit(code); });
    }
    app::MainWindow* w_;
    QString out_, shot_;
    int step_ = 0, drafts_ = 0, refined_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(product::kId);
    QCoreApplication::setApplicationName(product::kId);
    QCoreApplication::setApplicationVersion(product::kVersion);
    QGuiApplication::setApplicationDisplayName(product::kName);
    applyTheme(app);
    app.setWindowIcon(QIcon(":/app.png"));

    qRegisterMetaType<spectra::app::PreviewRequest>();
    qRegisterMetaType<spectra::app::PreviewInfo>();
    qRegisterMetaType<spectra::app::ExportJob>();
    qRegisterMetaType<QVector<spectra::app::ExportJob>>();
    qRegisterMetaType<spectra::engine::EngineConfig>();
    qRegisterMetaType<spectra::engine::LutOptions>();
    qRegisterMetaType<std::shared_ptr<const spectra::Image>>();
    qRegisterMetaType<nlohmann::json>();

    QCommandLineParser cli;
    cli.setApplicationDescription(product::kName);
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument("files", "Images to open.", "[files...]");
    QCommandLineOption selftest("selftest", "Run the unattended self-test, writing its files to <dir>.", "dir");
    QCommandLineOption screenshot("screenshot", "With --selftest: save a picture of the window.", "file");
    QCommandLineOption cpu("cpu", "Render on the CPU for this run.");
    QCommandLineOption fresh("fresh-settings", "Ignore stored settings and calibration for this run.");
    cli.addOptions({selftest, screenshot, cpu, fresh});
    cli.process(app);
    if (cli.isSet(selftest) || cli.isSet(fresh)) {
        // Keep the run independent of whatever this machine has stored.
        QCoreApplication::setApplicationName(QString("%1-selftest").arg(product::kId));
        QSettings().clear();
    }

    QString pack_path = findPack();
    std::shared_ptr<const Pack> pack;
    try {
        if (pack_path.isEmpty()) throw std::runtime_error("the data pack (data/spektrafilm.sfpk) was not found");
        pack = Pack::load(pack_path.toStdString());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (!cli.isSet(selftest)) QMessageBox::critical(nullptr, product::kName, QString::fromUtf8(e.what()));
        return 1;
    }

    app::MainWindow window(pack, pack_path, cli.isSet(cpu));
    window.show();
    std::unique_ptr<SelfTest> test;
    window.setUnattended(cli.isSet(selftest));
    if (cli.isSet(selftest)) test = std::make_unique<SelfTest>(&window, cli.value(selftest), cli.value(screenshot));
    else if (!cli.positionalArguments().isEmpty()) window.openFiles(cli.positionalArguments());
    if (!cli.isSet(selftest) && cli.isSet(screenshot)) {
        // Documentation helper: picture of the window once the opened image is rendered.
        auto shots = std::make_shared<int>(0);
        QString file = cli.value(screenshot);
        QObject::connect(&window, &app::MainWindow::previewShown, &window, [&window, shots, file](bool draft) {
            if (draft || ++*shots < 2) return;
            QTimer::singleShot(300, &window, [&window, file] {
                window.grab().save(file);
                QCoreApplication::quit();
            });
        });
    }
    return app.exec();
}
