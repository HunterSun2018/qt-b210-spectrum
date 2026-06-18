#pragma once

#include <QPoint>
#include <QVector>
#include <QWidget>

class SpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrumWidget(QWidget *parent = nullptr);
    void setFrequencySpan(double centerFreqHz, double sampleRateHz);
    void setDemodMarker(double demodFreqHz, bool enabled);

signals:
    void demodFrequencySelected(double frequencyHz);

public slots:
    void setSpectrum(const QVector<float> &spectrum);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QString formatFrequency(double hz) const;
    QRect spectrumDrawRect() const;
    bool tryMapFromPosition(const QPoint &position, double *frequencyHz, float *levelDb, int *binIndex) const;

    QVector<float> m_spectrum;
    float m_minDb = -120.0f;
    float m_maxDb = 0.0f;
    double m_centerFreqHz = 2.45e9;
    double m_sampleRateHz = 2.0e6;
    double m_demodFreqHz = 2.45e9;
    bool m_demodMarkerEnabled = false;
    bool m_hoverActive = false;
    QPoint m_hoverPosition;
    double m_hoverFrequencyHz = 0.0;
    float m_hoverLevelDb = 0.0f;
    int m_hoverBinIndex = -1;
};
