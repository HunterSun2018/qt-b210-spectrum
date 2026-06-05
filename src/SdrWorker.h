#pragma once

#include <atomic>
#include <cstdint>
#include <complex>
#include <memory>
#include <vector>

#include <QThread>

#include <uhd/usrp/multi_usrp.hpp>

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

    enum class DemodMode {
        None,
        FM,
        AM
    };

    struct Settings {
        InputSource inputSource = InputSource::Usrp;
        ProcessorMode processorMode = ProcessorMode::FloatFft;
        DemodMode demodMode = DemodMode::None;
        QString deviceArgs = "A";
        QString rxFrontend = "A";
        double sampleRate = 1.0e6;
        double centerFreq = 103.9e6;
        double gain = 40.0;
        QString antenna = "TX/RX";
        double squelchDb = -55.0;
        std::size_t fftSize = 2048;
    };

    explicit SdrWorker(QObject *parent = nullptr);
    ~SdrWorker() override;

    void startStreaming(const Settings &settings);
    void stopStreaming();

    void setRxFrontend(const QString &rxFrontend);
    void setRxAntenna(const QString &antenna);
    void setRxSampleRate(double sampleRate);
    void setFxCenterFreq(double centerFreq);
    void setRxGain(double gain);
    void setSquelchDb(double squelchDb);
    void setDemodMode(DemodMode mode);
    void setFftSize(std::size_t fftSize);
    
signals:
    void spectrumReady(QVector<float> spectrum);
    void statusChanged(const QString &status);
    void errorOccurred(const QString &errorText);

protected:
    void run() override;

private:
    Settings m_settings;
    std::atomic_bool m_stopRequested;

    uhd::usrp::multi_usrp::sptr m_usrp;
    std::size_t m_samplesPerBuffer;
};
