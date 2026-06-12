#pragma once

#include <QVector>
#include <QWidget>

class SpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrumWidget(QWidget *parent = nullptr);
    void setFrequencySpan(double centerFreqHz, double sampleRateHz);
    void setDemodMarker(double demodFreqHz, bool enabled);

public slots:
    void setSpectrum(const QVector<float> &spectrum);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString formatFrequency(double hz) const;

    QVector<float> m_spectrum;
    float m_minDb = -120.0f;
    float m_maxDb = 0.0f;
    double m_centerFreqHz = 2.45e9;
    double m_sampleRateHz = 2.0e6;
    double m_demodFreqHz = 2.45e9;
    bool m_demodMarkerEnabled = false;
};
