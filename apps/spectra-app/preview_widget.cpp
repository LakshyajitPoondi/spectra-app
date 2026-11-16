// SPDX-License-Identifier: GPL-3.0-or-later
#include "preview_widget.hpp"

#include <algorithm>
#include <cmath>

#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace spectra::app {

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void PreviewWidget::setImageSize(int w, int h) {
    bool same = w == img_w_ && h == img_h_;
    img_w_ = w;
    img_h_ = h;
    if (!same) {
        centre_ = QPointF(w / 2.0, h / 2.0);
        detail_ = QImage();
    }
    update();
}

void PreviewWidget::clearResults() {
    fit_ = QImage();
    detail_ = QImage();
    update();
}

void PreviewWidget::setMessage(const QString& text) {
    message_ = text;
    update();
}

void PreviewWidget::showResult(const QImage& image, const engine::ViewRect& rect) {
    if (rect.empty()) {
        fit_ = image;
    } else {
        detail_ = image;
        detail_rect_ = rect;
    }
    update();
}

int PreviewWidget::viewportLongEdge() const {
    return static_cast<int>(std::ceil(std::max(width(), height()) * devicePixelRatioF()));
}

engine::ViewRect PreviewWidget::currentView() const {
    if (!actual_ || img_w_ <= 0) return {};
    double dpr = devicePixelRatioF();
    int vw = std::min(img_w_, static_cast<int>(std::ceil(width() * dpr)));
    int vh = std::min(img_h_, static_cast<int>(std::ceil(height() * dpr)));
    int x = static_cast<int>(std::lround(centre_.x() - vw / 2.0));
    int y = static_cast<int>(std::lround(centre_.y() - vh / 2.0));
    x = std::clamp(x, 0, img_w_ - vw);
    y = std::clamp(y, 0, img_h_ - vh);
    return {x, y, vw, vh};
}

void PreviewWidget::clampCentre() {
    double dpr = devicePixelRatioF();
    double hw = std::min<double>(img_w_, width() * dpr) / 2.0, hh = std::min<double>(img_h_, height() * dpr) / 2.0;
    centre_.setX(std::clamp(centre_.x(), hw, std::max(hw, img_w_ - hw)));
    centre_.setY(std::clamp(centre_.y(), hh, std::max(hh, img_h_ - hh)));
}

void PreviewWidget::setActualSize(bool on) {
    if (on == actual_) return;
    actual_ = on;
    if (on) clampCentre();
    setCursor(on ? Qt::OpenHandCursor : Qt::ArrowCursor);
    update();
    Q_EMIT actualSizeChanged(on);
    Q_EMIT viewChanged();
}

QRectF PreviewWidget::fitRect() const {
    if (img_w_ <= 0 || img_h_ <= 0) return {};
    double margin = 12.0;
    double s = std::min((width() - 2 * margin) / img_w_, (height() - 2 * margin) / img_h_);
    double w = img_w_ * s, h = img_h_ * s;
    return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
}

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // 18 % grey surround, as on a light table: neutral reference for judging colour.
    p.fillRect(rect(), QColor(118, 118, 118));
    if (img_w_ <= 0) {
        p.setPen(QColor(230, 230, 230));
        p.drawText(rect(), Qt::AlignCenter,
                   message_.isEmpty() ? tr("Open an image, or drop files here") : message_);
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!actual_) {
        if (!fit_.isNull()) p.drawImage(fitRect(), fit_);
    } else {
        double dpr = devicePixelRatioF();
        // image pixel -> widget coordinate
        auto map = [&](double ix, double iy) {
            return QPointF(width() / 2.0 + (ix - centre_.x()) / dpr, height() / 2.0 + (iy - centre_.y()) / dpr);
        };
        QRectF whole(map(0, 0), map(img_w_, img_h_));
        if (!fit_.isNull()) p.drawImage(whole, fit_);  // backdrop until the detail arrives
        if (!detail_.isNull()) {
            QRectF r(map(detail_rect_.x, detail_rect_.y),
                     map(detail_rect_.x + detail_rect_.w, detail_rect_.y + detail_rect_.h));
            p.drawImage(r, detail_);
        }
    }
    if (!message_.isEmpty()) {
        QRect box(12, height() - 40, width() - 24, 28);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(box, 6, 6);
        p.setPen(QColor(240, 240, 240));
        p.drawText(box.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, message_);
    }
}

void PreviewWidget::resizeEvent(QResizeEvent*) {
    if (actual_) clampCentre();
    Q_EMIT viewChanged();
}

void PreviewWidget::mousePressEvent(QMouseEvent* e) {
    if (actual_ && e->button() == Qt::LeftButton) {
        dragging_ = true;
        drag_from_ = e->position();
        centre_from_ = centre_;
        setCursor(Qt::ClosedHandCursor);
    }
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    QPointF d = (e->position() - drag_from_) * devicePixelRatioF();
    centre_ = centre_from_ - d;
    clampCentre();
    update();
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (dragging_ && e->button() == Qt::LeftButton) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        Q_EMIT viewChanged();
    }
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (img_w_ <= 0) return;
    if (!actual_) {
        QRectF f = fitRect();
        if (f.width() > 0)
            centre_ = QPointF((e->position().x() - f.x()) / f.width() * img_w_,
                              (e->position().y() - f.y()) / f.height() * img_h_);
    }
    dragging_ = false;
    setActualSize(!actual_);
}

void PreviewWidget::wheelEvent(QWheelEvent* e) {
    if (!actual_ || img_w_ <= 0) return;
    centre_ -= QPointF(e->angleDelta().x(), e->angleDelta().y()) * devicePixelRatioF() * 0.5;
    clampCentre();
    update();
    Q_EMIT viewChanged();
}

void PreviewWidget::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void PreviewWidget::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    if (!paths.isEmpty()) Q_EMIT filesDropped(paths);
}

// ---- histogram --------------------------------------------------------------------

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(72);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void HistogramWidget::setData(const QVector<int>& rgb768) {
    data_ = rgb768;
    update();
}

void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 24));
    if (data_.size() != 768) return;
    // Scale to the 99.5th percentile bin so one clipped spike does not flatten the rest.
    QVector<int> sorted = data_;
    std::sort(sorted.begin(), sorted.end());
    double top = std::max(1, sorted[static_cast<int>(sorted.size() * 0.995)]);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    const QColor colours[3] = {QColor(200, 60, 60), QColor(60, 180, 60), QColor(70, 90, 220)};
    for (int k = 0; k < 3; ++k) {
        QPainterPath path;
        path.moveTo(0, height());
        for (int i = 0; i < 256; ++i) {
            double v = std::min(1.0, data_[k * 256 + i] / top);
            path.lineTo(i / 255.0 * width(), height() - v * (height() - 4));
        }
        path.lineTo(width(), height());
        path.closeSubpath();
        p.fillPath(path, colours[k]);
    }
}

}  // namespace spectra::app
