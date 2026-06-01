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

#include <alsa/asoundlib.h>
#include <uhd/stream.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/thread.hpp>

#include "FftProcessor.h"
#include "IqFftwProcessor.h"

namespace
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kInt16FullScale = 32767.0f;
    constexpr unsigned int kAudioSampleRate = 48000;
    constexpr unsigned int kAudioChannels = 2;
    constexpr float kFmDeviationScale = 0.35f;
    constexpr float kFmDeemphasisTauSeconds = 75.0e-6f;
    constexpr float kPilotLockRate = 0.0015f;
    constexpr float kStereoBlendScale = 45.0f;
    constexpr float kPilotDetectThreshold = 0.0008f;
    constexpr float kAmCutoffHz = 5000.0f;
    constexpr float kFmAudioCutoffHz = 15000.0f;
    constexpr float kPilotLowpassHz = 400.0f;
    constexpr float kSignalPowerFloor = 1.0e-12f;

    struct AudioPlaybackContext
    {
        snd_pcm_t *pcmHandle = nullptr;
    };

    struct SinglePoleFilter
    {
        float y = 0.0f;
    };

    struct StereoSample
    {
        float left = 0.0f;
        float right = 0.0f;
    };

    struct DemodulatorState
    {
        float fmLastPhase = 0.0f;
        float amDcEstimate = 0.0f;
        float pilotReferencePhase = 0.0f;
        float squelchGain = 0.0f;
        double resampleCursor = 0.0;
        double resampleStep = 1.0;
        double sampleIndex = 0.0;
        bool hasLastResampleSample = false;
        StereoSample lastResampleSample;
        SinglePoleFilter fmMonoFilter;
        SinglePoleFilter fmDiffFilter;
        SinglePoleFilter fmPilotIFilter;
        SinglePoleFilter fmPilotQFilter;
        SinglePoleFilter fmLeftDeemphasis;
        SinglePoleFilter fmRightDeemphasis;
        SinglePoleFilter amAudioFilter;
    };

    std::vector<std::complex<float>> generateSimulatedIq(std::size_t sampleCount, double sampleRate, std::size_t frameIndex);

    void closeAudioDevice(AudioPlaybackContext *context)
    {
        if (context == nullptr || context->pcmHandle == nullptr)
        {
            return;
        }
        snd_pcm_drain(context->pcmHandle);
        snd_pcm_close(context->pcmHandle);
        context->pcmHandle = nullptr;
    }

    void openAudioDevice(AudioPlaybackContext *context, unsigned int sampleRate)
    {
        if (context == nullptr)
        {
            throw std::runtime_error("Invalid audio context");
        }

        int error = snd_pcm_open(&context->pcmHandle, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (error < 0)
        {
            throw std::runtime_error("Failed to open ALSA device");
        }

        error = snd_pcm_set_params(context->pcmHandle,
                                   SND_PCM_FORMAT_S16_LE,
                                   SND_PCM_ACCESS_RW_INTERLEAVED,
                                   kAudioChannels,
                                   sampleRate,
                                   1,
                                   100000);
        if (error < 0)
        {
            closeAudioDevice(context);
            throw std::runtime_error("Failed to configure ALSA playback");
        }
    }

    void writeAudioFrames(AudioPlaybackContext *context, const std::vector<std::int16_t> &samples)
    {
        if (context == nullptr || context->pcmHandle == nullptr || samples.empty())
        {
            return;
        }

        const std::int16_t *data = samples.data();
        snd_pcm_sframes_t framesRemaining =
            static_cast<snd_pcm_sframes_t>(samples.size() / kAudioChannels);
        while (framesRemaining > 0)
        {
            const snd_pcm_sframes_t written = snd_pcm_writei(context->pcmHandle, data, framesRemaining);
            if (written == -EPIPE)
            {
                snd_pcm_prepare(context->pcmHandle);
                continue;
            }
            if (written < 0)
            {
                snd_pcm_recover(context->pcmHandle, static_cast<int>(written), 1);
                continue;
            }
            data += written * kAudioChannels;
            framesRemaining -= written;
        }
    }

    float lowpassAlpha(float cutoffHz, double sampleRate)
    {
        const float normalized = static_cast<float>((2.0 * M_PI * cutoffHz) / sampleRate);
        return 1.0f - std::exp(-normalized);
    }

    float applyLowpass(float sample, SinglePoleFilter *filter, float alpha)
    {
        filter->y += alpha * (sample - filter->y);
        return filter->y;
    }

    float applyDeemphasis(float sample, SinglePoleFilter *filter, double sampleRate)
    {
        const float alpha = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * kFmDeemphasisTauSeconds));
        filter->y += alpha * (sample - filter->y);
        return filter->y;
    }

    std::vector<std::complex<float>> convertSc16ToFloat(const std::vector<std::complex<std::int16_t>> &samples,
                                                        std::size_t count)
    {
        std::vector<std::complex<float>> converted(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            converted[i] = std::complex<float>(
                static_cast<float>(samples[i].real()) / kInt16FullScale,
                static_cast<float>(samples[i].imag()) / kInt16FullScale);
        }
        return converted;
    }

    float estimateSignalPowerDbfs(const std::vector<std::complex<float>> &samples)
    {
        if (samples.empty())
        {
            return -120.0f;
        }

        double sum = 0.0;
        for (const auto &sample : samples)
        {
            sum += std::norm(sample);
        }
        const double avg = sum / static_cast<double>(samples.size());
        return 10.0f * std::log10(std::max(avg, static_cast<double>(kSignalPowerFloor)));
    }

    std::vector<std::complex<float>> makeSimulatorBaseband(std::size_t sampleCount,
                                                           double sampleRate,
                                                           std::size_t frameIndex,
                                                           SdrWorker::DemodMode demodMode)
    {
        if (demodMode == SdrWorker::DemodMode::FM)
        {
            std::vector<std::complex<float>> samples(sampleCount);
            float phase = 0.0f;
            const float audio1 = 1000.0f;
            const float audio2 = 2300.0f;
            for (std::size_t i = 0; i < sampleCount; ++i)
            {
                const float t = static_cast<float>((frameIndex * sampleCount) + i) / static_cast<float>(sampleRate);
                const float audio = 0.65f * std::sin(kTwoPi * audio1 * t) + 0.25f * std::sin(kTwoPi * audio2 * t);
                phase += kFmDeviationScale * audio;
                samples[i] = std::polar(0.9f, phase);
            }
            return samples;
        }

        if (demodMode == SdrWorker::DemodMode::AM)
        {
            std::vector<std::complex<float>> samples(sampleCount);
            const float carrierOffset = 12000.0f;
            for (std::size_t i = 0; i < sampleCount; ++i)
            {
                const float t = static_cast<float>((frameIndex * sampleCount) + i) / static_cast<float>(sampleRate);
                const float envelope = 0.55f + 0.35f * std::sin(kTwoPi * 1200.0f * t) + 0.1f * std::sin(kTwoPi * 2600.0f * t);
                samples[i] = envelope * std::polar(1.0f, kTwoPi * carrierOffset * t);
            }
            return samples;
        }

        return generateSimulatedIq(sampleCount, sampleRate, frameIndex);
    }

    std::vector<std::int16_t> demodulateAudio(const std::vector<std::complex<float>> &samples,
                                              double sampleRate,
                                              float squelchDb,
                                              SdrWorker::DemodMode demodMode,
                                              DemodulatorState *state)
    {
        if (demodMode == SdrWorker::DemodMode::None || state == nullptr || samples.empty())
        {
            return {};
        }

        state->resampleStep = sampleRate / static_cast<double>(kAudioSampleRate);
        if (state->resampleCursor <= 0.0)
        {
            state->resampleCursor = state->resampleStep;
        }

        const float signalDb = estimateSignalPowerDbfs(samples);
        const float squelchTarget = signalDb >= squelchDb ? 1.0f : 0.0f;
        state->squelchGain += 0.02f * (squelchTarget - state->squelchGain);

        const float fmAudioAlpha = lowpassAlpha(kFmAudioCutoffHz, sampleRate);
        const float amAudioAlpha = lowpassAlpha(kAmCutoffHz, sampleRate);
        const float pilotAlpha = lowpassAlpha(kPilotLowpassHz, sampleRate);
        std::vector<std::int16_t> pcm;
        pcm.reserve((samples.size() / std::max(1.0, state->resampleStep)) * kAudioChannels + 8);

        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            StereoSample current{};
            if (demodMode == SdrWorker::DemodMode::FM)
            {
                const float phase = std::atan2(samples[i].imag(), samples[i].real());
                float delta = phase - state->fmLastPhase;
                if (delta > static_cast<float>(M_PI))
                {
                    delta -= kTwoPi;
                }
                else if (delta < -static_cast<float>(M_PI))
                {
                    delta += kTwoPi;
                }
                state->fmLastPhase = phase;
                const float mono = applyLowpass(delta, &state->fmMonoFilter, fmAudioAlpha);

                const float cos19 = std::cos(state->pilotReferencePhase);
                const float sin19 = std::sin(state->pilotReferencePhase);
                const float pilotI = applyLowpass(delta * cos19, &state->fmPilotIFilter, pilotAlpha);
                const float pilotQ = applyLowpass(delta * sin19, &state->fmPilotQFilter, pilotAlpha);
                const float pilotMagnitude = std::sqrt(pilotI * pilotI + pilotQ * pilotQ);
                const float pilotAngle = std::atan2(pilotQ, pilotI);
                const float stereoBlend = std::clamp(
                    (pilotMagnitude - kPilotDetectThreshold) * kStereoBlendScale, 0.0f, 1.0f);

                const float subcarrier = std::cos(2.0f * (state->pilotReferencePhase + pilotAngle));
                const float diff = applyLowpass(2.0f * delta * subcarrier, &state->fmDiffFilter, fmAudioAlpha);

                float left = mono + stereoBlend * diff;
                float right = mono - stereoBlend * diff;
                left = applyDeemphasis(left, &state->fmLeftDeemphasis, sampleRate);
                right = applyDeemphasis(right, &state->fmRightDeemphasis, sampleRate);

                current.left = left * state->squelchGain;
                current.right = right * state->squelchGain;

                state->pilotReferencePhase += static_cast<float>((kTwoPi * 19000.0) / sampleRate);
                if (state->pilotReferencePhase >= kTwoPi)
                {
                    state->pilotReferencePhase -= kTwoPi;
                }
            }
            else if (demodMode == SdrWorker::DemodMode::AM)
            {
                const float magnitude = std::abs(samples[i]);
                state->amDcEstimate = 0.9995f * state->amDcEstimate + 0.0005f * magnitude;
                const float demodulated = applyLowpass(magnitude - state->amDcEstimate, &state->amAudioFilter, amAudioAlpha);
                current.left = demodulated * state->squelchGain;
                current.right = current.left;
            }

            if (!state->hasLastResampleSample)
            {
                state->lastResampleSample = current;
                state->hasLastResampleSample = true;
                state->sampleIndex = 0.0;
                continue;
            }

            state->sampleIndex += 1.0;
            while (state->resampleCursor <= state->sampleIndex)
            {
                const float frac = static_cast<float>(state->resampleCursor - (state->sampleIndex - 1.0));
                const float left = state->lastResampleSample.left + frac * (current.left - state->lastResampleSample.left);
                const float right = state->lastResampleSample.right + frac * (current.right - state->lastResampleSample.right);
                pcm.push_back(static_cast<std::int16_t>(std::lround(std::clamp(left * 22000.0f, -32767.0f, 32767.0f))));
                pcm.push_back(static_cast<std::int16_t>(std::lround(std::clamp(right * 22000.0f, -32767.0f, 32767.0f))));
                state->resampleCursor += state->resampleStep;
            }
            state->lastResampleSample = current;
        }
        return pcm;
    }

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

        for (std::size_t i = 0; i < sampleCount; ++i)
        {
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
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
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
        if (result != IQ_FFTW_OK)
        {
            return {};
        }

        QVector<float> spectrum;
        spectrum.reserve(static_cast<qsizetype>(fftSize));
        for (double value : spectrumDbfs)
        {
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
    if (isRunning())
    {
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
    try
    {
        const std::size_t fftSize = std::max<std::size_t>(256, m_settings.fftSize);
        const std::size_t samplesPerBuffer = fftSize * 4;
        std::vector<std::complex<float>> fftInput(fftSize);
        FftProcessor processor(static_cast<int>(fftSize));
        DemodulatorState demodState;
        AudioPlaybackContext audioContext;
        auto audioGuard = std::unique_ptr<AudioPlaybackContext, decltype(&closeAudioDevice)>(&audioContext, closeAudioDevice);
        if (m_settings.demodMode != DemodMode::None)
        {
            openAudioDevice(&audioContext, kAudioSampleRate);
        }

        IqFftwProcessor iqProcessor;
        if (iq_fftw_processor_init(&iqProcessor, fftSize) != IQ_FFTW_OK)
        {
            throw std::runtime_error("Failed to initialize FFTW processor");
        }

        using IqProcessorGuard = std::unique_ptr<IqFftwProcessor, decltype(&iq_fftw_processor_destroy)>;
        auto iqProcessorGuard = IqProcessorGuard(&iqProcessor, iq_fftw_processor_destroy);

        if (m_settings.inputSource == InputSource::Simulator)
        {
            const QString processorName = m_settings.processorMode == ProcessorMode::FloatFft
                                              ? "FftProcessor"
                                              : "IqFftwProcessor";
            const QString demodName = m_settings.demodMode == DemodMode::FM
                                          ? ", FM"
                                          : (m_settings.demodMode == DemodMode::AM ? ", AM" : "");
            emit statusChanged("Streaming (simulator, " + processorName + demodName + ")");

            std::size_t frameIndex = 0;
            while (!m_stopRequested)
            {
                const std::vector<std::complex<float>> samples =
                    makeSimulatorBaseband(samplesPerBuffer, m_settings.sampleRate, frameIndex++, m_settings.demodMode);

                if (m_settings.processorMode == ProcessorMode::FloatFft)
                {
                    std::copy_n(samples.begin(), fftSize, fftInput.begin());
                    const std::vector<float> spectrum = processor.process(fftInput);
                    if (!spectrum.empty())
                    {
                        emit spectrumReady(QVector<float>(spectrum.begin(), spectrum.end()));
                    }
                }
                else
                {
                    const std::vector<std::complex<std::int16_t>> quantized = quantizeToSc16(samples);
                    const QVector<float> spectrum = processWithIqFftw(
                        iqProcessor,
                        reinterpret_cast<const std::int16_t *>(quantized.data()),
                        quantized.size() * 2U,
                        fftSize);
                    if (!spectrum.isEmpty())
                    {
                        emit spectrumReady(spectrum);
                    }
                }

                if (m_settings.demodMode != DemodMode::None)
                {
                    writeAudioFrames(&audioContext,
                                     demodulateAudio(
                                         samples,
                                         m_settings.sampleRate,
                                         static_cast<float>(m_settings.squelchDb),
                                         m_settings.demodMode,
                                         &demodState));
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
        m_usrp = uhd::usrp::multi_usrp::make(args);
        

        emit statusChanged("Configuring USRP...");

        m_usrp->set_rx_rate(m_settings.sampleRate);
        m_usrp->set_rx_freq(m_settings.centerFreq);
        m_usrp->set_rx_gain(m_settings.gain);
        m_usrp->set_rx_bandwidth(m_settings.sampleRate);
        m_usrp->set_rx_antenna(m_settings.antenna.toStdString());

        const bool useIqFftw = m_settings.processorMode == ProcessorMode::Int16Fftw;
        uhd::stream_args_t streamArgs(useIqFftw ? "sc16" : "fc32", "sc16");
        auto rxStreamer = m_usrp->get_rx_stream(streamArgs);

        std::vector<std::complex<float>> recvBufferFloat;
        std::vector<std::complex<std::int16_t>> recvBufferInt16;
        if (useIqFftw)
        {
            recvBufferInt16.resize(samplesPerBuffer);
        }
        else
        {
            recvBufferFloat.resize(samplesPerBuffer);
        }
        uhd::rx_metadata_t metadata;

        uhd::stream_cmd_t streamCmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        streamCmd.stream_now = true;
        rxStreamer->issue_stream_cmd(streamCmd);

        const QString demodName = m_settings.demodMode == DemodMode::FM
                                      ? ", FM"
                                      : (m_settings.demodMode == DemodMode::AM ? ", AM" : "");
        emit statusChanged(useIqFftw ? "Streaming (USRP, IqFftwProcessor" + demodName + ")"
                                     : "Streaming (USRP, FftProcessor" + demodName + ")");

        while (!m_stopRequested)
        {
            const std::size_t received = useIqFftw
                                             ? rxStreamer->recv(recvBufferInt16.data(), recvBufferInt16.size(), metadata, 0.25, false)
                                             : rxStreamer->recv(recvBufferFloat.data(), recvBufferFloat.size(), metadata, 0.25, false);

            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT)
            {
                continue;
            }

            if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
            {
                throw std::runtime_error("RX error: " + metadata.strerror());
            }

            if (received < fftSize)
            {
                continue;
            }

            std::vector<std::complex<float>> demodInput;
            if (useIqFftw)
            {
                const QVector<float> spectrum = processWithIqFftw(
                    iqProcessor,
                    reinterpret_cast<const std::int16_t *>(recvBufferInt16.data()),
                    received * 2U,
                    fftSize);
                if (spectrum.isEmpty())
                {
                    continue;
                }
                emit spectrumReady(spectrum);
                if (m_settings.demodMode != DemodMode::None)
                {
                    demodInput = convertSc16ToFloat(recvBufferInt16, received);
                }
            }
            else
            {
                std::copy_n(recvBufferFloat.begin(), fftSize, fftInput.begin());
                const std::vector<float> spectrum = processor.process(fftInput);
                if (spectrum.empty())
                {
                    continue;
                }

                emit spectrumReady(QVector<float>(spectrum.begin(), spectrum.end()));
                if (m_settings.demodMode != DemodMode::None)
                {
                    demodInput.assign(recvBufferFloat.begin(), recvBufferFloat.begin() + static_cast<std::ptrdiff_t>(received));
                }
            }

            if (m_settings.demodMode != DemodMode::None && !demodInput.empty())
            {
                writeAudioFrames(&audioContext,
                                 demodulateAudio(
                                     demodInput,
                                     m_settings.sampleRate,
                                     static_cast<float>(m_settings.squelchDb),
                                     m_settings.demodMode,
                                     &demodState));
            }
        }

        uhd::stream_cmd_t stopCmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        rxStreamer->issue_stream_cmd(stopCmd);
        emit statusChanged("Stopped");
    }
    catch (const std::exception &ex)
    {
        emit errorOccurred(QString::fromStdString(ex.what()));
        emit statusChanged("Error");
    }
}

void SdrWorker::setSampleRate(double sampleRate)
{
    m_usrp->set_rx_rate(sampleRate);

    m_settings.sampleRate = sampleRate;
}

void SdrWorker::setCenterFreq(double centerFreq)
{
    m_settings.centerFreq = centerFreq;
    
    m_usrp->set_rx_freq(centerFreq);
}

void SdrWorker::setGain(double gain)
{
    m_settings.gain = gain;
    
    m_usrp->set_rx_gain(gain);
}

void SdrWorker::setSquelchDb(double squelchDb)
{
    m_settings.squelchDb = squelchDb;
}

void SdrWorker::setDemodMode(DemodMode mode)
{
    m_settings.demodMode = mode;
}