#include "WaterfallWidget.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

namespace {

QColor lerpColor(const QColor &a, const QColor &b, float t)
{
    return QColor(
        static_cast<int>(a.red() + (b.red() - a.red()) * t),
        static_cast<int>(a.green() + (b.green() - a.green()) * t),
        static_cast<int>(a.blue() + (b.blue() - a.blue()) * t));
}

}

WaterfallWidget::WaterfallWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(280);
}

void WaterfallWidget::addSpectrumLine(const QVector<float> &spectrum)
{
    if (spectrum.isEmpty()) {
        return;
    }

    ensureImage();
    if (m_image.width() != spectrum.size()) {
        m_image = QImage(spectrum.size(), std::max(1, height()), QImage::Format_RGB32);
        m_image.fill(QColor(4, 6, 14));
    }

    if (m_image.height() > 1) {
        memmove(
            m_image.scanLine(1),
            m_image.scanLine(0),
            static_cast<std::size_t>(m_image.bytesPerLine()) * static_cast<std::size_t>(m_image.height() - 1));
    }

    QRgb *line = reinterpret_cast<QRgb *>(m_image.scanLine(0));
    for (int i = 0; i < spectrum.size(); ++i) {
        line[i] = colorForValue(spectrum[i]);
    }

    update();
}

void WaterfallWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    const QRect drawRect = rect().adjusted(70, 0, -18, 0);
    if (drawRect.width() <= 0 || drawRect.height() <= 0) {
        return;
    }

    QPainter painter(this);
    painter.fillRect(drawRect, QColor(6, 8, 14));

    if (m_image.isNull()) {
        painter.setPen(QColor(150, 160, 180));
        painter.drawText(drawRect, Qt::AlignCenter, "Waiting for spectrum...");
        return;
    }

    painter.drawImage(drawRect, m_image);
    painter.setPen(QColor(220, 230, 245));
    painter.drawText(drawRect.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft, "Waterfall");
}

void WaterfallWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    ensureImage();
}

void WaterfallWidget::ensureImage()
{
    if (!m_image.isNull() && m_image.height() == height()) {
        return;
    }

    const int width = std::max(1, m_image.width());
    QImage resized(width, std::max(1, height()), QImage::Format_RGB32);
    resized.fill(QColor(4, 6, 14));

    if (!m_image.isNull()) {
        QPainter painter(&resized);
        painter.drawImage(0, 0, m_image.scaled(width, resized.height()));
    }

    m_image = resized;
}

QRgb WaterfallWidget::colorForValue(float value) const
{
    const float ratio = std::clamp((value - m_minDb) / (m_maxDb - m_minDb), 0.0f, 1.0f);
    const float shaped = std::pow(ratio, 0.78f);

    struct ColorStop {
        float position;
        QColor color;
    };

    static const ColorStop stops[] = {
        {0.00f, QColor(2, 4, 10)},
        {0.12f, QColor(8, 18, 46)},
        {0.28f, QColor(22, 58, 124)},
        {0.45f, QColor(28, 142, 196)},
        {0.62f, QColor(76, 208, 160)},
        {0.78f, QColor(244, 220, 72)},
        {0.90f, QColor(245, 128, 32)},
        {1.00f, QColor(250, 250, 250)},
    };

    for (int i = 1; i < static_cast<int>(std::size(stops)); ++i) {
        if (shaped <= stops[i].position) {
            const float range = stops[i].position - stops[i - 1].position;
            const float t = range > 0.0f ? (shaped - stops[i - 1].position) / range : 0.0f;
            return lerpColor(stops[i - 1].color, stops[i].color, t).rgb();
        }
    }

    return stops[std::size(stops) - 1].color.rgb();
}
