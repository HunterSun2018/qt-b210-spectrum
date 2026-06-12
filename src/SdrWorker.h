#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <complex>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
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

    enum class FftProcessorMode {
        FftProcessor,
        IqFftwProcessor
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
        FftProcessorMode fftProcessorMode = FftProcessorMode::FftProcessor;
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
    
    void setFftProcessor(FftProcessorMode processor);
    void setFftSize(std::size_t fftSize);
    
signals:
    void spectrumReady(QVector<float> spectrum);
    void statusChanged(const QString &status);
    void errorOccurred(const QString &errorText);

protected:
    void run() override;

private:
    struct ProcessingFrame {
        std::size_t received = 0;
        bool usesInt16 = false;
        std::vector<std::complex<float>> floatSamples;
        std::vector<std::complex<std::int16_t>> int16Samples;
    };

    struct AudioFrame {
        double sampleRate = 0.0;
        float squelchDb = 0.0f;
        DemodMode demodMode = DemodMode::None;
        std::vector<std::complex<float>> samples;
    };

    void initializeRunState();
    void startWorkerThreads();
    void stopWorkerThreads(bool emitStoppedStatus = true, bool rethrowError = true);
    void runSimulatorStream();
    void runUsrpStream();
    void processingLoop(std::stop_token stopToken);
    void audioLoop(std::stop_token stopToken);
    void enqueueProcessingFrame(ProcessingFrame &&frame);
    void enqueueAudioFrame(AudioFrame &&frame);
    void storeProcessingError(std::exception_ptr error);
    void rethrowProcessingError();
    void requestWorkerStop();

    Settings m_settings;
    std::atomic_bool m_stopRequested;

    uhd::usrp::multi_usrp::sptr m_usrp;
    std::size_t m_samplesPerBuffer;

    std::mutex m_processingQueueMutex;
    std::condition_variable_any m_processingQueueCv;
    std::deque<ProcessingFrame> m_processingQueue;

    std::mutex m_audioQueueMutex;
    std::condition_variable_any m_audioQueueCv;
    std::deque<AudioFrame> m_audioQueue;

    std::mutex m_processingErrorMutex;
    std::exception_ptr m_processingError;
    std::atomic_bool m_producerDone{false};
    std::atomic_bool m_audioProducerDone{false};

    std::jthread m_processingThread;
    std::jthread m_audioThread;
};
