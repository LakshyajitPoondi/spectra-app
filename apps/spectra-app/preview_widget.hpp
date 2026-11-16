// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
#include <QPointF>
#include <QVector>
#include <QWidget>

#include "spectra/engine/engine.hpp"

namespace spectra::app {

// Shows the preview: fitted to the window, or at 100 % with panning. It only
// paints images it is given; rendering happens elsewhere.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImageSize(int w, int h);  // size of the loaded image; 0 clears
    void showResult(const QImage& image, const engine::ViewRect& rect);
    void clearResults();
    void setMessage(const QString& text);
    bool isActualSize() const { return actual_; }
    // Region the widget needs at 100 % (empty when fitted).
    engine::ViewRect currentView() const;
    int viewportLongEdge() const;
    const QImage& fitImage() const { return fit_; }

public Q_SLOTS:
    void setActualSize(bool on);

Q_SIGNALS:
    void viewChanged();
    void actualSizeChanged(bool on);
    void filesDropped(const QStringList& paths);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    QRectF fitRect() const;
    void clampCentre();
    int img_w_ = 0, img_h_ = 0;
    bool actual_ = false;
    QPointF centre_;        // image pixel at the widget centre (100 % mode)
    QImage fit_, detail_;
    engine::ViewRect detail_rect_;
    QString message_;
    bool dragging_ = false;
    QPointF drag_from_, centre_from_;
};

class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistogramWidget(QWidget* parent = nullptr);
    void setData(const QVector<int>& rgb768);
    QSize sizeHint() const override { return {260, 90}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<int> data_;
};

}  // namespace spectra::app
