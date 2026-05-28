#include "WaterfallWidget.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

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
        m_image.fill(Qt::black);
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

    QPainter painter(this);
    painter.fillRect(rect(), QColor(6, 8, 14));

    if (m_image.isNull()) {
        painter.setPen(QColor(150, 160, 180));
        painter.drawText(rect(), Qt::AlignCenter, "Waiting for spectrum...");
        return;
    }

    painter.drawImage(rect(), m_image);
    painter.setPen(QColor(220, 230, 245));
    painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft, "Waterfall");
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
    resized.fill(Qt::black);

    if (!m_image.isNull()) {
        QPainter painter(&resized);
        painter.drawImage(0, 0, m_image.scaled(width, resized.height()));
    }

    m_image = resized;
}

QRgb WaterfallWidget::colorForValue(float value) const
{
    const float ratio = std::clamp((value - m_minDb) / (m_maxDb - m_minDb), 0.0f, 1.0f);
    const int r = static_cast<int>(255.0f * std::pow(ratio, 0.35f));
    const int g = static_cast<int>(255.0f * std::pow(ratio, 1.1f));
    const int b = static_cast<int>(255.0f * std::pow(1.0f - ratio, 1.8f));
    return qRgb(r, g, b);
}
