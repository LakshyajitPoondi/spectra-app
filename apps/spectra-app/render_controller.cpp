// SPDX-License-Identifier: GPL-3.0-or-later
#include "render_controller.hpp"

#include <algorithm>
#include <cmath>

#include <QFileInfo>

#include "spectra/core/parallel.hpp"

namespace spectra::app {

using nlohmann::json;

QImage to_qimage(const Image& img, QVector<int>* histogram) {
    QImage out(img.w, img.h, QImage::Format_RGB888);
    std::vector<std::array<int, 768>> hist_parts;
    int nt = std::max(1, thread_count());
    hist_parts.assign(static_cast<size_t>(nt) + 1, {});
    std::atomic<int> next{0};
    parallel_for(img.h, [&](long a, long b) {
        auto& hist = hist_parts[static_cast<size_t>(next.fetch_add(1)) % hist_parts.size()];
        for (long y = a; y < b; ++y) {
            uchar* line = out.scanLine(static_cast<int>(y));
            const double* src = img.px(static_cast<int>(y), 0);
            for (int x = 0; x < img.w; ++x)
                for (int k = 0; k < 3; ++k) {
                    double v = src[static_cast<size_t>(x) * img.c + (img.c >= 3 ? k : 0)];
                    int q = std::isfinite(v) ? static_cast<int>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5) : 0;
                    line[x * 3 + k] = static_cast<uchar>(q);
                    ++hist[static_cast<size_t>(k * 256 + q)];
                }
        }
    }, 32);
    if (histogram) {
        histogram->fill(0, 768);
        for (auto& part : hist_parts)
            for (int i = 0; i < 768; ++i) (*histogram)[i] += part[static_cast<size_t>(i)];
    }
    return out;
}

RenderController::RenderController(QObject* parent) : QObject(parent) {}
RenderController::~RenderController() = default;

void RenderController::init(const QString& pack_path, const engine::EngineConfig& config, const QString& cached) {
    try {
        engine_.reset();
        if (!pack_) {
            pack_ = Pack::load(pack_path.toStdString());
            colour_ = std::make_shared<const ColourData>(pack_);
        }
        engine_ = std::make_unique<engine::Engine>(pack_, colour_, config);
        bool calibrated = false;
        if (!cached.isEmpty()) {
            try {
                json all = json::parse(cached.toStdString());
                const std::string id = engine_->device_id();
                if (all.is_object() && all.contains(id)) {
                    engine::Calibration c = engine::Calibration::from_json(all.at(id));
                    if (c.version == engine::Calibration::kVersion) {
                        engine_->set_calibration(c);
                        calibrated_ = c;
                        calibrated = true;
                    }
                }
            } catch (const std::exception&) {
                // an unreadable cache only means the calibration runs again
            }
        }
        if (!calibrated) calibrated_ = engine_->calibration();
        if (image_) engine_->set_image(image_, space_.toStdString(), proxy_);
        Q_EMIT engineReady(QString::fromStdString(engine_->device_name()), QString::fromStdString(engine_->device_id()),
                         QString::fromStdString(engine_->backend_note()), engine_->has_gpu(), calibrated);
    } catch (const std::exception& e) {
        Q_EMIT engineFailed(QString::fromUtf8(e.what()));
    }
}

void RenderController::shutdown() {
    engine_.reset();
    image_.reset();
}

void RenderController::setImage(std::shared_ptr<const Image> image, const QString& color_space, bool proxy,
                                quint64 image_token) {
    image_ = std::move(image);
    space_ = color_space;
    proxy_ = proxy;
    image_token_ = image_token;
    if (!engine_) return;
    try {
        engine_->set_image(image_, space_.toStdString(), proxy_);
    } catch (const std::exception& e) {
        Q_EMIT previewFailed(QString::fromUtf8(e.what()), 0);
    }
}

void RenderController::restoreImage() {
    if (engine_) {
        try {
            engine_->set_image(image_, space_.toStdString(), proxy_);
        } catch (const std::exception&) {
        }
    }
}

void RenderController::requestPreview(const PreviewRequest& rq) {
    if (!engine_ || !engine_->has_image()) return;
    // A newer request is already queued: skip this one entirely.
    if (rq.superseded->load()) return;
    auto send = [&](const Image& img, const engine::RenderResult* r, bool draft) {
        PreviewInfo info;
        info.generation = rq.generation;
        info.draft = draft;
        info.input_view = rq.show_input;
        QImage q = to_qimage(img, &info.histogram);
        if (r) {
            info.gpu = r->gpu;
            info.ms = r->ms;
            info.first_stage = r->first_stage;
            info.rect = r->rect;
            for (auto& n : r->notes) info.notes << QString::fromStdString(n);
        } else {
            info.rect = rq.view;
        }
        info.gpu_bytes = static_cast<qint64>(engine_->gpu_allocated_bytes());
        Q_EMIT previewReady(q, info);
    };
    try {
        engine_->set_viewport(rq.viewport);
        if (rq.show_input) {
            send(engine_->render_input(engine::Level::Preview, rq.view), nullptr, false);
            return;
        }
        Params p = params_from_json(rq.params);
        // Progressive: a low-resolution pass first. It is never interrupted, so a
        // slider that keeps moving still shows every step; only the refinement
        // is abandoned when a newer request arrives.
        engine::RenderResult draft = engine_->render_preview(p, engine::Level::Draft, rq.view, nullptr);
        send(draft.image, &draft, true);
        if (rq.superseded->load()) return;
        engine::RenderResult fine = engine_->render_preview(p, engine::Level::Preview, rq.view, rq.superseded.get());
        send(fine.image, &fine, false);
    } catch (const std::exception& e) {
        if (!engine::is_cancel(e)) Q_EMIT previewFailed(QString::fromUtf8(e.what()), rq.generation);
    }
}

void RenderController::calibrate() {
    if (!engine_) return;
    abort = false;
    try {
        engine::Calibration c = engine_->calibrate(
            [this](const std::string& line) { Q_EMIT calibrationProgress(QString::fromStdString(line)); }, &abort);
        calibrated_ = c;
        Q_EMIT calibrationDone(QString::fromStdString(engine_->device_id()), QString::fromStdString(c.to_json().dump()));
    } catch (const std::exception& e) {
        if (engine::is_cancel(e)) Q_EMIT calibrationProgress("Calibration cancelled.");
        else Q_EMIT calibrationProgress(QString("Calibration failed: ") + e.what());
        Q_EMIT calibrationDone(QString(), QString());
    }
}

void RenderController::setOverrides(bool match_python, quint64 budget_bytes, int tile_size) {
    if (engine_) engine_->set_overrides(match_python, budget_bytes, tile_size);
}

void RenderController::setPreviewSizes(int preview_long_edge, int draft_long_edge) {
    if (!engine_) return;
    engine::Calibration c = calibrated_;
    if (preview_long_edge > 0) {
        c.preview_long_edge = preview_long_edge;
        c.draft_long_edge = draft_long_edge > 0 ? draft_long_edge : std::max(128, preview_long_edge / 3);
    }
    engine_->set_calibration(c);
}

void RenderController::checkFullRenderNotice(const json& params) {
    if (!engine_ || !engine_->has_image()) return;
    try {
        Q_EMIT noticeChanged(QString::fromStdString(engine_->full_render_notice(params_from_json(params))));
    } catch (const std::exception&) {
        Q_EMIT noticeChanged(QString());
    }
}

void RenderController::exportFiles(const QVector<ExportJob>& jobs) {
    if (!engine_) return;
    abort = false;
    int ok = 0, failed = 0;
    QString message;
    bool touched_image = false;
    for (int i = 0; i < jobs.size(); ++i) {
        const ExportJob& job = jobs[i];
        QString name = QFileInfo(job.out_path).fileName();
        auto progress = [&](int done, int total) { Q_EMIT exportProgress(i, jobs.size(), done, total, name); };
        try {
            if (abort) throw engine::Cancelled{};
            Q_EMIT exportProgress(i, jobs.size(), 0, 1, name);
            Params p = params_from_json(job.params);
            if (job.in_path.isEmpty()) {
                if (!engine_->has_image()) throw std::runtime_error("no image loaded");
                engine::RenderResult r = engine_->render_full(p, progress, &abort);
#ifdef SPECTRA_HAVE_IO
                engine::write_result(job.out_path.toStdString(), r.image, job.out);
#else
                throw std::runtime_error("this build has no image file support");
#endif
            } else {
#ifdef SPECTRA_HAVE_IO
                engine::FileJob fj;
                fj.in_path = job.in_path.toStdString();
                fj.out_path = job.out_path.toStdString();
                fj.white_balance = job.white_balance.toStdString();
                fj.temperature = job.temperature;
                fj.tint = job.tint;
                fj.out = job.out;
                touched_image = true;
                engine::process_file(*engine_, p, fj, progress, &abort);
#else
                throw std::runtime_error("this build has no image file support");
#endif
            }
            ++ok;
        } catch (const std::exception& e) {
            if (engine::is_cancel(e)) {
                message = "Export cancelled.";
                break;
            }
            ++failed;
            message += QString("%1: %2\n").arg(name, QString::fromUtf8(e.what()));
        }
    }
    if (touched_image) restoreImage();
    Q_EMIT exportFinished(ok, failed, message.trimmed());
}

void RenderController::exportLut(const json& params, const engine::LutOptions& options, const QString& path) {
    try {
        engine::export_cube_lut(*pack_, colour_, params_from_json(params), options, path.toStdString());
        Q_EMIT exportFinished(1, 0, QString("LUT written to %1").arg(path));
    } catch (const std::exception& e) {
        Q_EMIT exportFinished(0, 1, QString::fromUtf8(e.what()));
    }
}

}  // namespace spectra::app
