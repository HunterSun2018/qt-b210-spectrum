#pragma once

#include <atomic>
#include <cstdint>
#include <complex>
#include <memory>
#include <vector>

#include <QThread>

class FftProcessor;

class SdrWorker : public QThread
{
    Q_OBJECT

public:
    enum class InputSource {
        Usrp,
        Simulator
    };

    enum class ProcessorMode {
        FloatFft,
        Int16Fftw
    };

    struct Settings {
        InputSource inputSource = InputSource::Usrp;
        ProcessorMode processorMode = ProcessorMode::FloatFft;
        QString deviceArgs;
        double sampleRate = 1.0e6;
        double centerFreq = 103.9e6;
        double gain = 40.0;
        QString antenna = "TX/RX";
        std::size_t fftSize = 2048;
    };

    explicit SdrWorker(QObject *parent = nullptr);
    ~SdrWorker() override;

    void startStreaming(const Settings &settings);
    void stopStreaming();

signals:
    void spectrumReady(QVector<float> spectrum);
    void statusChanged(const QString &status);
    void errorOccurred(const QString &errorText);

protected:
    void run() override;

private:
    Settings m_settings;
    std::atomic_bool m_stopRequested;
};
