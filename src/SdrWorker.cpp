#include "SdrWorker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <exception>
#include <random>
#include <string>
#include <thread>

#include <QVector>

#include <uhd/stream.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/thread.hpp>

#include "FftProcessor.h"
#include "IqFftwProcessor.h"

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kInt16FullScale = 32767.0f;

std::vector<std::complex<float>> generateSimulatedIq(std::size_t sampleCount, double sampleRate, std::size_t frameIndex)
{
    std::vector<std::complex<float>> samples(sampleCount);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(0xB210u + frameIndex));
    std::normal_distribution<float> noise(0.0f, 0.015f);

    const float tone1 = 120000.0f;
    const float tone2Base = -320000.0f;
    const float tone2Sweep = 140000.0f * std::sin(static_cast<float>(frameIndex) * 0.035f);
    const float tone2 = tone2Base + tone2Sweep;
    const float tone3 = 460000.0f;

    for (std::size_t i = 0; i < sampleCount; ++i) {
        const float t = static_cast<float>((frameIndex * sampleCount) + i) / static_cast<float>(sampleRate);
        const std::complex<float> s1 = std::polar(0.85f, kTwoPi * tone1 * t);
        const std::complex<float> s2 = std::polar(0.45f, kTwoPi * tone2 * t);
        const std::complex<float> s3 = std::polar(0.20f, kTwoPi * tone3 * t);
        samples[i] = s1 + s2 + s3 + std::complex<float>(noise(rng), noise(rng));
    }

    return samples;
}

std::vector<std::complex<std::int16_t>> quantizeToSc16(const std::vector<std::complex<float>> &samples)
{
    std::vector<std::complex<std::int16_t>> quantized(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float iValue = std::clamp(samples[i].real(), -1.0f, 1.0f);
        const float qValue = std::clamp(samples[i].imag(), -1.0f, 1.0f);
        quantized[i] = std::complex<std::int16_t>(
            static_cast<std::int16_t>(std::lround(iValue * kInt16FullScale)),
            static_cast<std::int16_t>(std::lround(qValue * kInt16FullScale)));
    }
    return quantized;
}

QVector<float> processWithIqFftw(IqFftwProcessor &processor,
    const std::int16_t *iqInterleaved,
    std::size_t iqValueCount,
    std::size_t fftSize)
{
    std::vector<double> spectrumDbfs(fftSize);
    const int result =
        iq_fftw_processor_process_i16(&processor, iqInterleaved, iqValueCount, spectrumDbfs.data());
    if (result != IQ_FFTW_OK) {
        return {};
    }

    QVector<float> spectrum;
    spectrum.reserve(static_cast<qsizetype>(fftSize));
    for (double value : spectrumDbfs) {
        spectrum.append(static_cast<float>(value));
    }
    return spectrum;
}
}

SdrWorker::SdrWorker(QObject *parent)
    : QThread(parent),
      m_stopRequested(false)
{
}

SdrWorker::~SdrWorker()
{
    stopStreaming();
    wait();
}

void SdrWorker::startStreaming(const Settings &settings)
{
    if (isRunning()) {
        stopStreaming();
        wait();
    }

    m_settings = settings;
    m_stopRequested = false;
    start();
}

void SdrWorker::stopStreaming()
{
    m_stopRequested = true;
}

void SdrWorker::run()
{
    try {
        const std::size_t fftSize = std::max<std::size_t>(256, m_settings.fftSize);
        const std::size_t samplesPerBuffer = fftSize * 4;
        std::vector<std::complex<float>> fftInput(fftSize);
        FftProcessor processor(static_cast<int>(fftSize));

        IqFftwProcessor iqProcessor;
        if (iq_fftw_processor_init(&iqProcessor, fftSize) != IQ_FFTW_OK) {
            throw std::runtime_error("Failed to initialize FFTW processor");
        }
        
        auto iqProcessorGuard = std::unique_ptr<IqFftwProcessor, decltype(&iq_fftw_processor_destroy)>(&iqProcessor, iq_fftw_processor_destroy);


        if (m_settings.inputSource == InputSource::Simulator) {
            const QString processorName = m_settings.processorMode == ProcessorMode::FloatFft
                ? "FftProcessor"
                : "IqFftwProcessor";
            emit statusChanged("Streaming (simulator, " + processorName + ")");

            std::size_t frameIndex = 0;
            while (!m_stopRequested) {
                const std::vector<std::complex<float>> samples =
                    generateSimulatedIq(samplesPerBuffer, m_settings.sampleRate, frameIndex++);

                if (m_settings.processorMode == ProcessorMode::FloatFft) {
                    std::copy_n(samples.begin(), fftSize, fftInput.begin());
                    const std::vector<float> spectrum = processor.process(fftInput);
                    if (!spectrum.empty()) {
                        emit spectrumReady(QVector<float>(spectrum.begin(), spectrum.end()));
                    }
                } else {
                    const std::vector<std::complex<std::int16_t>> quantized = quantizeToSc16(samples);
                    const QVector<float> spectrum = processWithIqFftw(
                        iqProcessor,
                        reinterpret_cast<const std::int16_t *>(quantized.data()),
                        quantized.size() * 2U,
                        fftSize);
                    if (!spectrum.isEmpty()) {
                        emit spectrumReady(spectrum);
                    }
                }

                const auto frameTime = std::chrono::duration<double>(static_cast<double>(fftSize) / m_settings.sampleRate);
                std::this_thread::sleep_for(frameTime);
            }

            emit statusChanged("Stopped");
            return;
        }

        uhd::set_thread_priority_safe();
        emit statusChanged("Connecting to USRP...");

        const std::string args = m_settings.deviceArgs.trimmed().toStdString();
        auto usrp = uhd::usrp::multi_usrp::make(args);

        emit statusChanged("Configuring USRP...");

        usrp->set_rx_rate(m_settings.sampleRate);
        usrp->set_rx_freq(m_settings.centerFreq);
        usrp->set_rx_gain(m_settings.gain);        
        usrp->set_rx_bandwidth(m_settings.sampleRate);
        usrp->set_rx_antenna(m_settings.antenna.toStdString());

        const bool useIqFftw = m_settings.processorMode == ProcessorMode::Int16Fftw;
        uhd::stream_args_t streamArgs(useIqFftw ? "sc16" : "fc32", "sc16");
        auto rxStreamer = usrp->get_rx_stream(streamArgs);

        std::vector<std::complex<float>> recvBufferFloat;
        std::vector<std::complex<std::int16_t>> recvBufferInt16;
        if (useIqFftw) {
            recvBufferInt16.resize(samplesPerBuffer);
        } else {
            recvBufferFloat.resize(samplesPerBuffer);
        }
        uhd::rx_metadata_t metadata;

        uhd::stream_cmd_t streamCmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        streamCmd.stream_now = true;
        rxStreamer->issue_stream_cmd(streamCmd);

        emit statusChanged(useIqFftw ? "Streaming (USRP, IqFftwProcessor)"
                                     : "Streaming (USRP, FftProcessor)");

        while (!m_stopRequested) {
            const std::size_t received = useIqFftw
                ? rxStreamer->recv(recvBufferInt16.data(), recvBufferInt16.size(), metadata, 0.25, false)
                : rxStreamer->recv(recvBufferFloat.data(), recvBufferFloat.size(), metadata, 0.25, false);

            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                continue;
            }

            if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
                throw std::runtime_error("RX error: " + metadata.strerror());
            }

            if (received < fftSize) {
                continue;
            }

            if (useIqFftw) {
                const QVector<float> spectrum = processWithIqFftw(
                    iqProcessor,
                    reinterpret_cast<const std::int16_t *>(recvBufferInt16.data()),
                    received * 2U,
                    fftSize);
                if (spectrum.isEmpty()) {
                    continue;
                }
                emit spectrumReady(spectrum);
            } else {
                std::copy_n(recvBufferFloat.begin(), fftSize, fftInput.begin());
                const std::vector<float> spectrum = processor.process(fftInput);
                if (spectrum.empty()) {
                    continue;
                }

                emit spectrumReady(QVector<float>(spectrum.begin(), spectrum.end()));
            }
        }

        uhd::stream_cmd_t stopCmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        rxStreamer->issue_stream_cmd(stopCmd);
        emit statusChanged("Stopped");
    } catch (const std::exception &ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
        emit statusChanged("Error");
    }
}
