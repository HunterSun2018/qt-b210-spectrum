#include "SpectrumWidget.h"

#include <algorithm>
#include <cmath>

#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>

SpectrumWidget::SpectrumWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(240);
}

void SpectrumWidget::setFrequencySpan(double centerFreqHz, double sampleRateHz)
{
    m_centerFreqHz = centerFreqHz;
    m_sampleRateHz = sampleRateHz;
    update();
}

void SpectrumWidget::setSpectrum(const QVector<float> &spectrum)
{
    m_spectrum = spectrum;
    update();
}

void SpectrumWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor(10, 14, 24));
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect drawRect = rect().adjusted(70, 18, -18, -42);
    if (drawRect.width() <= 0 || drawRect.height() <= 0) {
        return;
    }

    const QFontMetrics metrics(font());
    painter.setPen(QColor(45, 58, 88));
    painter.drawRect(drawRect);
    painter.setPen(QColor(45, 58, 88));

    for (int i = 0; i <= 4; ++i) {
        const int y = drawRect.top() + (drawRect.height() * i) / 4;
        painter.drawLine(drawRect.left(), y, drawRect.right(), y);

        const float db = m_maxDb - ((m_maxDb - m_minDb) * static_cast<float>(i) / 4.0f);
        const QString label = QString::number(std::lround(db)) + " dB";
        painter.setPen(QColor(180, 190, 205));
        painter.drawText(8, y - (metrics.height() / 2), 54, metrics.height(), Qt::AlignRight | Qt::AlignVCenter, label);
        painter.setPen(QColor(45, 58, 88));
    }

    for (int i = 0; i <= 8; ++i) {
        const int x = drawRect.left() + (drawRect.width() * i) / 8;
        painter.drawLine(x, drawRect.top(), x, drawRect.bottom());

        const double freq = m_centerFreqHz - (m_sampleRateHz * 0.5) + (m_sampleRateHz * static_cast<double>(i) / 8.0);
        const QString label = formatFrequency(freq);
        painter.setPen(QColor(180, 190, 205));
        painter.drawText(x - 42, drawRect.bottom() + 8, 84, metrics.height(), Qt::AlignHCenter | Qt::AlignTop, label);
        painter.setPen(QColor(45, 58, 88));
    }

    if (m_spectrum.isEmpty()) {
        painter.setPen(QColor(150, 160, 180));
        painter.drawText(drawRect, Qt::AlignCenter, "Waiting for IQ data...");
    } else {
        QPainterPath path;
        for (int i = 0; i < m_spectrum.size(); ++i) {
            const float ratio = (m_spectrum[i] - m_minDb) / (m_maxDb - m_minDb);
            const float clamped = std::clamp(ratio, 0.0f, 1.0f);
            const qreal x = drawRect.left() + (drawRect.width() * static_cast<qreal>(i)) / (m_spectrum.size() - 1);
            const qreal y = drawRect.bottom() - (drawRect.height() * clamped);
            if (i == 0) {
                path.moveTo(x, y);
            } else {
                path.lineTo(x, y);
            }
        }

        painter.setPen(QPen(QColor(66, 220, 163), 0.8));
        
        painter.drawPath(path);
    }

    painter.setPen(QColor(220, 230, 245));
    painter.drawText(drawRect.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, "Spectrum (dBFS)");
    painter.drawText(10, drawRect.top() - 2, 50, metrics.height(), Qt::AlignRight | Qt::AlignVCenter, "Power");
    painter.drawText(drawRect.left(), drawRect.bottom() + 24, drawRect.width(), metrics.height(), Qt::AlignHCenter | Qt::AlignTop, "Frequency");
}

QString SpectrumWidget::formatFrequency(double hz) const
{
    const double absHz = std::abs(hz);
    if (absHz >= 1.0e9) {
        return QString::number(hz / 1.0e9, 'f', 2) + " GHz";
    }
    if (absHz >= 1.0e6) {
        return QString::number(hz / 1.0e6, 'f', 2) + " MHz";
    }
    if (absHz >= 1.0e3) {
        return QString::number(hz / 1.0e3, 'f', 1) + " kHz";
    }
    return QString::number(hz, 'f', 0) + " Hz";
}
