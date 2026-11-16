// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <memory>

#include <QIcon>
#include <QMainWindow>
#include <QThread>
#include <QVector>

#include "param_panel.hpp"
#include "preview_widget.hpp"
#include "render_controller.hpp"

class QAction;
class QLabel;
class QListWidget;
class QMenu;
class QProgressDialog;
class QTimer;

namespace spectra::app {

struct FileEntry {
    QString path;
    nlohmann::json params, baseline;
    RawSettings raw;
    bool is_raw = false;
};

struct AppSettings {
    QString gpu_device;
    bool force_cpu = false;
    bool match_python = false;
    int budget_mb = 0;       // 0 = automatic
    int tile_size = 0;       // 0 = automatic
    int preview_edge = 0;    // 0 = calibrated
    bool advanced = false;
    void load();
    void save() const;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(std::shared_ptr<const Pack> pack, const QString& pack_path, bool force_cpu, QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFiles(const QStringList& paths);
    // Self-test hooks: work on a generated picture instead of a file.
    void loadSynthetic(int w, int h);
    void setParameter(const std::string& json_pointer, const nlohmann::json& value);
    void exportCurrentTo(const QString& path);
    void exportLutTo(const QString& path);
    bool engineHasGpu() const { return gpu_; }

Q_SIGNALS:
    void engineStarted();
    void previewShown(bool draft);
    void exportDone(int ok, int failed, const QString& message);
    void calibrationFinished();

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    void buildMenus();
    void buildDocks();
    void startEngine();
    void selectFile(int index);
    void startDecode();
    void onDecoded(quint64 token, std::shared_ptr<const Image> image, const QString& space, bool proxy,
                   const QString& info);
    void requestPreview();
    void onPreviewReady(const QImage& image, const PreviewInfo& info);
    void onStockChanged();
    void onParamsChanged();
    void applyParams(const Params& p);
    void exportCurrent();
    void exportBatch();
    void exportLut();
    void runExport(const QVector<ExportJob>& jobs);
    void showSettings();
    void showAbout();
    void rebuildPresetMenu();
    void savePreset();
    void applyPresetFile(const QString& path);
    void saveEdits(const FileEntry& e) const;
    bool loadEdits(FileEntry& e) const;
    QString editsPath(const QString& file) const;
    QString presetsDir() const;
    void updateStatus();
    void setBanner(const QString& text);
    FileEntry* current() { return current_ >= 0 && current_ < files_.size() ? &files_[current_] : nullptr; }
    template <class Fn>
    void post(Fn&& fn) {
        QMetaObject::invokeMethod(controller_, std::forward<Fn>(fn), Qt::QueuedConnection);
    }

    std::shared_ptr<const Pack> pack_;
    std::shared_ptr<const ColourData> colour_;
    QString pack_path_;
    AppSettings settings_;
    QThread thread_;
    RenderController* controller_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    ParamPanel* panel_ = nullptr;
    HistogramWidget* histogram_ = nullptr;
    QListWidget* strip_ = nullptr;
    QLabel *banner_ = nullptr, *status_device_ = nullptr, *status_render_ = nullptr, *status_memory_ = nullptr;
    QMenu* preset_menu_ = nullptr;
    QAction *act_export_ = nullptr, *act_batch_ = nullptr, *act_lut_ = nullptr, *act_actual_ = nullptr,
            *act_before_ = nullptr;
    QProgressDialog* progress_ = nullptr;
    QTimer* notice_timer_ = nullptr;

    QVector<FileEntry> files_;
    int current_ = -1;
    quint64 decode_token_ = 0, generation_ = 0, shown_generation_ = 0;
    std::shared_ptr<std::atomic<bool>> last_request_;
    bool engine_ready_ = false, gpu_ = false, have_image_ = false, image_is_proxy_ = false, calibrating_ = false;
    QString device_, device_id_, engine_note_, notice_, decode_info_;
    nlohmann::json clipboard_;
    Params synthetic_base_;
};

}  // namespace spectra::app
