// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <memory>

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "spectra/engine/engine.hpp"

namespace spectra::app {

// One preview request. The UI thread never waits for it: it is queued to the
// render thread, and `superseded` is raised as soon as a newer one exists.
struct PreviewRequest {
    nlohmann::json params;
    engine::ViewRect view;
    quint64 generation = 0;
    int viewport = 1600;
    bool show_input = false;  // before / after: show the unprocessed input instead
    std::shared_ptr<std::atomic<bool>> superseded = std::make_shared<std::atomic<bool>>(false);
};

struct PreviewInfo {
    quint64 generation = 0;
    bool draft = false;
    bool gpu = false;
    bool input_view = false;
    double ms = 0.0;
    int first_stage = 0;
    engine::ViewRect rect;
    QStringList notes;
    QVector<int> histogram;  // 3 x 256, R then G then B
    qint64 gpu_bytes = 0;
};

struct ExportJob {
    QString in_path;   // empty: the image that is loaded
    QString out_path;
    nlohmann::json params;
    QString white_balance = "as_shot";
    double temperature = 6504.0, tint = 1.0;
    engine::ExportOptions out;
};

// Lives on the render thread and owns the Engine (and with it the Vulkan
// device). Every slot runs there; results come back as queued signals.
class RenderController : public QObject {
    Q_OBJECT
public:
    explicit RenderController(QObject* parent = nullptr);
    ~RenderController() override;

    // Set from the UI thread to abandon an export or calibration in progress.
    std::atomic<bool> abort{false};

public Q_SLOTS:
    void init(const QString& pack_path, const spectra::engine::EngineConfig& config, const QString& cached_calibration);
    void setImage(std::shared_ptr<const spectra::Image> image, const QString& color_space, bool proxy, quint64 image_token);
    void requestPreview(const spectra::app::PreviewRequest& request);
    void calibrate();
    void setOverrides(bool match_python, quint64 budget_bytes, int tile_size);
    void setPreviewSizes(int preview_long_edge, int draft_long_edge);  // 0 = calibrated
    void exportFiles(const QVector<spectra::app::ExportJob>& jobs);
    void exportLut(const nlohmann::json& params, const spectra::engine::LutOptions& options, const QString& path);
    void checkFullRenderNotice(const nlohmann::json& params);
    void shutdown();

Q_SIGNALS:
    void engineReady(const QString& device, const QString& device_id, const QString& note, bool gpu, bool calibrated);
    void engineFailed(const QString& message);
    void previewReady(const QImage& image, const spectra::app::PreviewInfo& info);
    void previewFailed(const QString& message, quint64 generation);
    void calibrationProgress(const QString& line);
    void calibrationDone(const QString& device_id, const QString& json);
    void exportProgress(int file, int files, int step, int steps, const QString& what);
    void exportFinished(int ok, int failed, const QString& message);
    void noticeChanged(const QString& text);

private:
    void restoreImage();
    std::shared_ptr<const Pack> pack_;
    std::shared_ptr<const ColourData> colour_;
    std::unique_ptr<engine::Engine> engine_;
    engine::Calibration calibrated_;
    std::shared_ptr<const Image> image_;
    QString space_;
    bool proxy_ = false;
    quint64 image_token_ = 0;
};

QImage to_qimage(const Image& img, QVector<int>* histogram = nullptr);

}  // namespace spectra::app

Q_DECLARE_METATYPE(spectra::app::PreviewRequest)
Q_DECLARE_METATYPE(spectra::app::PreviewInfo)
Q_DECLARE_METATYPE(spectra::app::ExportJob)
Q_DECLARE_METATYPE(spectra::engine::EngineConfig)
Q_DECLARE_METATYPE(spectra::engine::LutOptions)
Q_DECLARE_METATYPE(std::shared_ptr<const spectra::Image>)
Q_DECLARE_METATYPE(nlohmann::json)
