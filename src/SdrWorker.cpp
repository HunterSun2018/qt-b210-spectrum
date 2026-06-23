#include "SdrWorker.h"
#include "SignalProcessor.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
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
#include "UdpIqClient.h"

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
    constexpr std::size_t kMaxProcessingQueueDepth = 3;
    constexpr std::size_t kMaxAudioQueueDepth = 8;
    constexpr std::size_t kMaxUdpQueueDepth = 8;
    constexpr std::size_t kMaxSpectrumChunksPerFrame = 8;
    constexpr double kTargetBufferDurationSeconds = 0.01;
    constexpr double kFmChannelSampleRate = 240000.0;
    constexpr double kAmChannelSampleRate = 96000.0;
    constexpr double kFmChannelCutoffHz = 100000.0;
    constexpr double kAmChannelCutoffHz = 9000.0;

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

    struct ChannelizerState
    {
        double inputSampleRate = 0.0;
        double inputCenterFreq = 0.0;
        double demodCenterFreq = 0.0;
        double outputSampleRate = 0.0;
        double ncoPhase = 0.0;
        double ncoStep = 0.0;
        float lpfAlpha = 1.0f;
        std::size_t decimation = 1;
        std::size_t decimationPhase = 0;
        std::complex<float> decimationAccumulator{0.0f, 0.0f};
        std::complex<float> filteredSample{0.0f, 0.0f};
        SdrWorker::DemodMode mode = SdrWorker::DemodMode::None;
    };

    std::vector<std::complex<float>> generateSimulatedIq(std::size_t sampleCount, double sampleRate, std::size_t frameIndex);

    std::size_t chooseSamplesPerBuffer(double sampleRate, std::size_t fftSize)
    {
        if (sampleRate <= 0.0)
        {
            return fftSize;
        }

        const auto targetSamples =
            static_cast<std::size_t>(std::llround(sampleRate * kTargetBufferDurationSeconds));
        const auto minimumSamples = fftSize;
        const auto requestedSamples = std::max(minimumSamples, targetSamples);
        const auto fftBlocks = std::max<std::size_t>(1, (requestedSamples + fftSize - 1) / fftSize);
        return fftBlocks * fftSize;
    }

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

    std::vector<std::complex<float>> convertSc16ToFloat(const std::complex<std::int16_t> *samples,
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

    std::vector<std::complex<float>> convertSc16ToFloat(const std::vector<std::complex<std::int16_t>> &samples,
                                                        std::size_t count)
    {
        return convertSc16ToFloat(samples.data(), count);
    }

    double targetDemodSampleRate(SdrWorker::DemodMode mode)
    {
        return mode == SdrWorker::DemodMode::FM ? kFmChannelSampleRate : kAmChannelSampleRate;
    }

    double targetDemodCutoffHz(SdrWorker::DemodMode mode)
    {
        return mode == SdrWorker::DemodMode::FM ? kFmChannelCutoffHz : kAmChannelCutoffHz;
    }

    void resetChannelizer(ChannelizerState *state,
                          double inputSampleRate,
                          double inputCenterFreq,
                          double demodCenterFreq,
                          SdrWorker::DemodMode mode)
    {
        if (state == nullptr)
        {
            return;
        }

        state->inputSampleRate = inputSampleRate;
        state->inputCenterFreq = inputCenterFreq;
        state->demodCenterFreq = demodCenterFreq;
        state->mode = mode;
        state->ncoPhase = 0.0;
        state->decimationPhase = 0;
        state->decimationAccumulator = {0.0f, 0.0f};
        state->filteredSample = {0.0f, 0.0f};

        const double targetRate = targetDemodSampleRate(mode);
        state->decimation =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(inputSampleRate / targetRate)));
        state->outputSampleRate = inputSampleRate / static_cast<double>(state->decimation);

        const double frequencyOffsetHz = std::clamp(
            demodCenterFreq - inputCenterFreq,
            -(inputSampleRate * 0.5),
            inputSampleRate * 0.5);
        state->ncoStep = -kTwoPi * (frequencyOffsetHz / inputSampleRate);

        const double cutoffHz = std::min(targetDemodCutoffHz(mode), state->outputSampleRate * 0.45);
        state->lpfAlpha = lowpassAlpha(cutoffHz, state->outputSampleRate);
    }

    bool channelizerNeedsReset(const ChannelizerState &state,
                               double inputSampleRate,
                               double inputCenterFreq,
                               double demodCenterFreq,
                               SdrWorker::DemodMode mode)
    {
        return state.mode != mode ||
               state.inputSampleRate != inputSampleRate ||
               state.inputCenterFreq != inputCenterFreq ||
               state.demodCenterFreq != demodCenterFreq;
    }

    std::vector<std::complex<float>> extractDemodChannel(const std::vector<std::complex<float>> &samples,
                                                         double inputSampleRate,
                                                         double inputCenterFreq,
                                                         double demodCenterFreq,
                                                         SdrWorker::DemodMode mode,
                                                         double *outputSampleRate,
                                                         ChannelizerState *state)
    {
        if (outputSampleRate != nullptr)
        {
            *outputSampleRate = inputSampleRate;
        }

        if (samples.empty() || mode == SdrWorker::DemodMode::None || state == nullptr)
        {
            return {};
        }

        if (channelizerNeedsReset(*state, inputSampleRate, inputCenterFreq, demodCenterFreq, mode))
        {
            resetChannelizer(state, inputSampleRate, inputCenterFreq, demodCenterFreq, mode);
        }

        std::vector<std::complex<float>> channelized;
        channelized.reserve(samples.size() / state->decimation + 1);
        const float scale = 1.0f / static_cast<float>(state->decimation);

        for (const auto &sample : samples)
        {
            const std::complex<float> oscillator(
                static_cast<float>(std::cos(state->ncoPhase)),
                static_cast<float>(std::sin(state->ncoPhase)));
            state->ncoPhase += state->ncoStep;
            if (state->ncoPhase > kTwoPi || state->ncoPhase < -kTwoPi)
            {
                state->ncoPhase = std::fmod(state->ncoPhase, static_cast<double>(kTwoPi));
            }

            state->decimationAccumulator += sample * oscillator;
            ++state->decimationPhase;
            if (state->decimationPhase < state->decimation)
            {
                continue;
            }

            const std::complex<float> averaged = state->decimationAccumulator * scale;
            state->decimationAccumulator = {0.0f, 0.0f};
            state->decimationPhase = 0;

            state->filteredSample += state->lpfAlpha * (averaged - state->filteredSample);
            channelized.push_back(state->filteredSample);
        }

        if (outputSampleRate != nullptr)
        {
            *outputSampleRate = state->outputSampleRate;
        }

        return channelized;
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

    QString describeUdpPacket(const UdpIqClient::ResponsePacket &packet)
    {
        if (packet.result.bodyType == 202)
        {
            return QString("UDP RX body=%1 iq_pairs=%2 sr=%3 bw=%4")
                .arg(packet.result.bodyType)
                .arg(packet.jamIq.iqSamples.size())
                .arg(packet.jamIq.sampleRate)
                .arg(packet.jamIq.bandwidth);
        }

        if (packet.result.bodyType == 201 || packet.result.bodyType == 203 || packet.result.bodyType == 204)
        {
            return QString("UDP RX body=%1 signal_type=%2 freq=%3")
                .arg(packet.result.bodyType)
                .arg(packet.result.signalType)
                .arg(packet.result.signalFreq);
        }

        return QString("UDP RX body=%1 len=%2")
            .arg(packet.result.bodyType)
            .arg(packet.result.bodyLength);
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

std::tuple<std::size_t, double> SdrWorker::calculateSignalBandwidthAndFreq(std::size_t freq,
                                                                           std::size_t sampleRate) const
{
    if (sampleRate == 0)
    {
        return {0, static_cast<double>(freq)};
    }

    std::size_t bandwidth = sampleRate / 2;
    double centerFrequency = static_cast<double>(freq);

    switch (m_settings.demodMode)
    {
    case DemodMode::FM:
    {
        constexpr std::size_t kFmBroadcastBandwidthHz = 320000;
        constexpr double kFmChannelStepHz = 200000.0;
        constexpr double kFmChannelOffsetHz = 100000.0;

        bandwidth = std::min<std::size_t>(kFmBroadcastBandwidthHz, sampleRate);
        centerFrequency =
            (std::round((static_cast<double>(freq) - kFmChannelOffsetHz) / kFmChannelStepHz) *
             kFmChannelStepHz) +
            kFmChannelOffsetHz;
        break;
    }
    case DemodMode::AM:
    {
        constexpr std::size_t kAmBandwidthHz = 10000;
        constexpr double kAmChannelStepHz = 10000.0;

        bandwidth = std::min<std::size_t>(kAmBandwidthHz, sampleRate);
        centerFrequency = std::round(static_cast<double>(freq) / kAmChannelStepHz) * kAmChannelStepHz;
        break;
    }
    case DemodMode::None:
    default:
        break;
    }

    return {bandwidth, centerFrequency};
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
    requestWorkerStop();
}

void SdrWorker::run()
{
    m_samplesPerBuffer = chooseSamplesPerBuffer(m_settings.sampleRate, m_settings.fftSize);
    initializeRunState();
    startWorkerThreads();

    try
    {
        if (m_settings.inputSource == InputSource::Simulator)
        {
            runSimulatorStream();
        }
        else
        {
            runUsrpStream();
        }

        stopWorkerThreads();
    }
    catch (const std::exception &ex)
    {
        stopWorkerThreads(false, false);
        emit errorOccurred(QString::fromStdString(ex.what()));
        emit statusChanged("Error");
    }
}

void SdrWorker::initializeRunState()
{
    {
        std::lock_guard<std::mutex> lock(m_processingQueueMutex);
        m_processingQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_audioQueueMutex);
        m_audioQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_udpQueueMutex);
        m_udpQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_processingErrorMutex);
        m_processingError = nullptr;
    }

    m_producerDone = false;
    m_audioProducerDone = false;
    m_udpProducerDone = false;
    m_udpClient = std::make_unique<UdpIqClient>();
}

void SdrWorker::startWorkerThreads()
{
    m_audioThread = std::jthread(&SdrWorker::audioLoop, this);
    m_processingThread = std::jthread(&SdrWorker::processingLoop, this);
    if (m_settings.udp.enabled)
    {
        m_udpThread = std::jthread(&SdrWorker::udpLoop, this);
    }
}

void SdrWorker::stopWorkerThreads(bool emitStoppedStatus, bool rethrowError)
{
    requestWorkerStop();
    m_producerDone = true;
    m_audioProducerDone = true;
    m_udpProducerDone = true;
    m_processingQueueCv.notify_all();
    m_audioQueueCv.notify_all();
    m_udpQueueCv.notify_all();

    if (m_processingThread.joinable())
    {
        m_processingThread.join();
    }
    if (m_audioThread.joinable())
    {
        m_audioThread.join();
    }
    if (m_udpThread.joinable())
    {
        m_udpThread.join();
    }

    if (m_udpClient)
    {
        m_udpClient->close();
    }

    if (rethrowError)
    {
        rethrowProcessingError();
    }
    if (emitStoppedStatus)
    {
        emit statusChanged("Stopped");
    }
}

void SdrWorker::requestWorkerStop()
{
    if (m_processingThread.joinable())
    {
        m_processingThread.request_stop();
    }
    if (m_audioThread.joinable())
    {
        m_audioThread.request_stop();
    }
    if (m_udpThread.joinable())
    {
        m_udpThread.request_stop();
    }
    m_processingQueueCv.notify_all();
    m_audioQueueCv.notify_all();
    m_udpQueueCv.notify_all();
}

void SdrWorker::runSimulatorStream()
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
        ProcessingFrame frame;
        frame.received = m_samplesPerBuffer;
        frame.floatSamples = makeSimulatorBaseband(
            m_samplesPerBuffer, m_settings.sampleRate, frameIndex++, m_settings.demodMode);
        enqueueUdpFrame(frame);
        enqueueProcessingFrame(std::move(frame));

        const auto frameTime =
            std::chrono::duration<double>(static_cast<double>(m_samplesPerBuffer) / m_settings.sampleRate);
        std::this_thread::sleep_for(frameTime);
    }
}

void SdrWorker::runUsrpStream()
{
    uhd::set_thread_priority_safe();
    emit statusChanged("Connecting to USRP...");

    const std::string args = m_settings.deviceArgs.trimmed().toStdString();
    m_usrp = uhd::usrp::multi_usrp::make(args);

    emit statusChanged("Configuring USRP...");

    auto subdevSpec =
        uhd::usrp::subdev_spec_t(QString("%1:%2").arg("A").arg(m_settings.rxFrontend).toStdString());
    m_usrp->set_rx_subdev_spec(subdevSpec);
    m_usrp->set_rx_rate(m_settings.sampleRate);
    m_usrp->set_rx_freq(m_settings.centerFreq);
    m_usrp->set_rx_gain(m_settings.gain);
    m_usrp->set_rx_bandwidth(m_settings.sampleRate);
    m_usrp->set_rx_antenna(m_settings.antenna.toStdString());

    auto rxInfo = m_usrp->get_usrp_rx_info(0);
    for (const auto &info : std::map<std::string, std::string>(rxInfo))
    {
        std::cout << "  " << info.first << ": " << info.second << std::endl;
    }

    const bool useInt16 = m_settings.processorMode == ProcessorMode::Int16Fftw;
    uhd::stream_args_t streamArgs(useInt16 ? "sc16" : "fc32", "sc16");
    auto rxStreamer = m_usrp->get_rx_stream(streamArgs);

    std::vector<std::complex<float>> recvBufferFloat;
    std::vector<std::complex<std::int16_t>> recvBufferInt16;
    if (useInt16)
    {
        recvBufferInt16.resize(m_samplesPerBuffer);
    }
    else
    {
        recvBufferFloat.resize(m_samplesPerBuffer);
    }

    uhd::rx_metadata_t metadata;
    uhd::stream_cmd_t streamCmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    streamCmd.stream_now = true;
    rxStreamer->issue_stream_cmd(streamCmd);

    const QString demodName = m_settings.demodMode == DemodMode::FM
                                  ? ", FM"
                                  : (m_settings.demodMode == DemodMode::AM ? ", AM" : "");
    emit statusChanged(useInt16 ? "Streaming (USRP, IqFftwProcessor" + demodName + ")"
                                : "Streaming (USRP, FftProcessor" + demodName + ")");

    std::size_t bufferedSamples = 0;
    while (!m_stopRequested)
    {
        const auto samplesRemaining = m_samplesPerBuffer - bufferedSamples;
        const std::size_t received = useInt16
                                         ? rxStreamer->recv(recvBufferInt16.data() + bufferedSamples, samplesRemaining, metadata, 0.25, false)
                                         : rxStreamer->recv(recvBufferFloat.data() + bufferedSamples, samplesRemaining, metadata, 0.25, false);

        if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT)
        {
            std::cerr << "RX timeout: No samples received within the timeout period." << std::endl;
            continue;
        }

        if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
        {
            std::cerr << "RX overflow: host processing fell behind, dropping buffered samples." << std::endl;
            bufferedSamples = 0;
            continue;
        }

        if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            throw std::runtime_error(std::string("RX error: ") + metadata.strerror());
        }

        if (received == 0)
        {
            continue;
        }

        bufferedSamples += received;
        if (bufferedSamples < m_samplesPerBuffer)
        {
            continue;
        }

        ProcessingFrame frame;
        frame.received = bufferedSamples;
        frame.usesInt16 = useInt16;
        if (useInt16)
        {
            frame.int16Samples.resize(bufferedSamples);
            std::copy_n(recvBufferInt16.begin(), static_cast<std::ptrdiff_t>(bufferedSamples), frame.int16Samples.begin());
        }
        else
        {
            frame.floatSamples.resize(bufferedSamples);
            std::copy_n(recvBufferFloat.begin(), static_cast<std::ptrdiff_t>(bufferedSamples), frame.floatSamples.begin());
        }
        enqueueUdpFrame(frame);
        enqueueProcessingFrame(std::move(frame));
        bufferedSamples = 0;
    }

    uhd::stream_cmd_t stopCmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
    rxStreamer->issue_stream_cmd(stopCmd);
}

void SdrWorker::udpLoop(std::stop_token stopToken)
{
    try
    {
        if (!m_udpClient)
        {
            throw std::runtime_error("UDP client is not initialized");
        }

        m_udpClient->open(
            UdpIqClient::Endpoint{m_settings.udp.remoteAddress.toStdString(), m_settings.udp.remotePort},
            UdpIqClient::BindEndpoint{m_settings.udp.bindAddress.toStdString(), m_settings.udp.bindPort});

        while (!stopToken.stop_requested())
        {
            ProcessingFrame frame;
            bool hasFrame = false;
            {
                std::unique_lock<std::mutex> lock(m_udpQueueMutex);
                m_udpQueueCv.wait_for(lock,
                                      stopToken,
                                      std::chrono::milliseconds(20),
                                      [&]
                                      { return m_udpProducerDone || !m_udpQueue.empty() || m_stopRequested; });

                if (!m_udpQueue.empty())
                {
                    frame = std::move(m_udpQueue.front());
                    m_udpQueue.pop_front();
                    hasFrame = true;
                }
                else if (m_udpProducerDone || m_stopRequested || stopToken.stop_requested())
                {
                    break;
                }
            }

            if (hasFrame && frame.usesInt16 && !frame.int16Samples.empty())
            {
                const auto sampleRate = static_cast<std::uint32_t>(
                    std::clamp(std::llround(m_settings.sampleRate),
                               0LL,
                               static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
                const auto bandwidth = sampleRate;
                const auto frequencyHz = static_cast<std::uint64_t>(
                    std::clamp(std::llround(m_settings.centerFreq),
                               0LL,
                               static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));

                m_udpClient->sendInterleaved(
                    UdpIqClient::SendMeta{
                        m_settings.udp.majorVersion,
                        m_settings.udp.minorVersion,
                        m_settings.udp.bodyType,
                        sampleRate,
                        frequencyHz,
                        bandwidth},
                    frame.int16Samples.data(),
                    static_cast<std::uint32_t>(std::min(frame.received, frame.int16Samples.size())));
            }

            while (!stopToken.stop_requested())
            {
                const auto packet = m_udpClient->receivePacket(std::chrono::milliseconds(hasFrame ? 5 : 1));
                if (!packet.has_value())
                {
                    break;
                }

                if (packet->result.bodyType == 202 && !packet->jamIq.iqSamples.empty())
                {
                    ProcessingFrame responseFrame;
                    responseFrame.received = packet->jamIq.iqSamples.size();
                    responseFrame.usesInt16 = true;
                    responseFrame.int16Samples = packet->jamIq.iqSamples;
                    enqueueProcessingFrame(std::move(responseFrame));
                }

                std::cout << describeUdpPacket(*packet).toStdString() << std::endl;
            }
        }
    }
    catch (...)
    {
        storeProcessingError(std::current_exception());
    }
}

void SdrWorker::processingLoop(std::stop_token stopToken)
{
    try
    {
        std::vector<std::complex<float>> fftInput(m_settings.fftSize);
        auto processor = std::make_unique<FftProcessor>(static_cast<int>(m_settings.fftSize));
        ChannelizerState channelizerState;

        IqFftwProcessor iqProcessor;
        if (iq_fftw_processor_init(&iqProcessor, m_settings.fftSize) != IQ_FFTW_OK)
        {
            throw std::runtime_error("Failed to initialize FFTW processor");
        }

        using IqProcessorGuard = std::unique_ptr<IqFftwProcessor, decltype(&iq_fftw_processor_destroy)>;
        auto iqProcessorGuard = IqProcessorGuard(&iqProcessor, iq_fftw_processor_destroy);

        while (!stopToken.stop_requested())
        {
            const auto fftSize = m_settings.fftSize;

            if (fftInput.size() != fftSize)
            {
                fftInput.resize(fftSize);
                processor = std::make_unique<FftProcessor>(static_cast<int>(fftSize));
            }

            ProcessingFrame frame;
            {
                std::unique_lock<std::mutex> lock(m_processingQueueMutex);
                m_processingQueueCv.wait(lock, stopToken, [&]
                                         { return m_producerDone || !m_processingQueue.empty() || m_stopRequested; });

                if (m_processingQueue.empty())
                {
                    if (m_producerDone || m_stopRequested || stopToken.stop_requested())
                    {
                        break;
                    }
                    continue;
                }

                frame = std::move(m_processingQueue.front());
                m_processingQueue.pop_front();
            }

            if (frame.received < fftSize)
            {
                continue;
            }

            const std::size_t fftChunkCount = frame.received / fftSize;
            const std::size_t spectrumChunkCount = std::min(fftChunkCount, kMaxSpectrumChunksPerFrame);
            std::vector<float> spectrumAccumulator(fftSize, 0.0f);
            std::size_t processedSpectrumChunks = 0;

            if (m_settings.fftProcessorMode == FftProcessorMode::IqFftwProcessor)
            {
                if (frame.usesInt16)
                {
                    for (std::size_t spectrumChunk = 0; spectrumChunk < spectrumChunkCount; ++spectrumChunk)
                    {
                        const std::size_t chunkIndex = (spectrumChunk * fftChunkCount) / spectrumChunkCount;
                        const auto *chunk = frame.int16Samples.data() + (chunkIndex * fftSize);
                        const QVector<float> spectrum = processWithIqFftw(
                            iqProcessor,
                            reinterpret_cast<const std::int16_t *>(chunk),
                            fftSize * 2U,
                            fftSize);
                        if (spectrum.isEmpty())
                        {
                            continue;
                        }

                        for (std::size_t i = 0; i < fftSize; ++i)
                        {
                            spectrumAccumulator[i] += spectrum[static_cast<int>(i)];
                        }
                        ++processedSpectrumChunks;
                    }
                }
                else
                {
                    const std::vector<std::complex<std::int16_t>> quantized = quantizeToSc16(frame.floatSamples);
                    for (std::size_t spectrumChunk = 0; spectrumChunk < spectrumChunkCount; ++spectrumChunk)
                    {
                        const std::size_t chunkIndex = (spectrumChunk * fftChunkCount) / spectrumChunkCount;
                        const auto *chunk = quantized.data() + (chunkIndex * fftSize);
                        const QVector<float> spectrum = processWithIqFftw(
                            iqProcessor,
                            reinterpret_cast<const std::int16_t *>(chunk),
                            fftSize * 2U,
                            fftSize);
                        if (spectrum.isEmpty())
                        {
                            continue;
                        }

                        for (std::size_t i = 0; i < fftSize; ++i)
                        {
                            spectrumAccumulator[i] += spectrum[static_cast<int>(i)];
                        }
                        ++processedSpectrumChunks;
                    }
                }
            }
            else
            {
                if (frame.usesInt16)
                {
                    for (std::size_t spectrumChunk = 0; spectrumChunk < spectrumChunkCount; ++spectrumChunk)
                    {
                        const std::size_t chunkIndex = (spectrumChunk * fftChunkCount) / spectrumChunkCount;
                        const auto *chunk = frame.int16Samples.data() + (chunkIndex * fftSize);
                        std::vector<std::complex<float>> convertedInput = convertSc16ToFloat(chunk, fftSize);
                        const std::vector<float> spectrum = processor->process(convertedInput);
                        if (spectrum.empty())
                        {
                            continue;
                        }

                        for (std::size_t i = 0; i < fftSize; ++i)
                        {
                            spectrumAccumulator[i] += spectrum[i];
                        }
                        ++processedSpectrumChunks;
                    }
                }
                else
                {
                    for (std::size_t spectrumChunk = 0; spectrumChunk < spectrumChunkCount; ++spectrumChunk)
                    {
                        const std::size_t chunkIndex = (spectrumChunk * fftChunkCount) / spectrumChunkCount;
                        const auto chunkOffset = static_cast<std::ptrdiff_t>(chunkIndex * fftSize);
                        std::copy_n(frame.floatSamples.begin() + chunkOffset, fftSize, fftInput.begin());
                        const std::vector<float> spectrum = processor->process(fftInput);
                        if (spectrum.empty())
                        {
                            continue;
                        }

                        for (std::size_t i = 0; i < fftSize; ++i)
                        {
                            spectrumAccumulator[i] += spectrum[i];
                        }
                        ++processedSpectrumChunks;

                        // estimate signal center frequency and print it every 100 chunks
                        auto signal_estimate = estimate_signal(fftInput, m_settings.sampleRate, m_settings.centerFreq, fftSize);
                        if (signal_estimate.has_value())
                        {
                            static std::vector<double> recent_estimates;

                            if (recent_estimates.size() > 100)
                            {
                                recent_estimates.clear();
                            }

                            recent_estimates.push_back(signal_estimate->signal_center_Hz);

                            if (recent_estimates.size() == 100)
                            {
                                auto m = recent_estimates.begin() + recent_estimates.size() / 2;
                                std::nth_element(recent_estimates.begin(), m ,recent_estimates.end());
                                const double average = recent_estimates[recent_estimates.size() / 2];
                                emit signalCenterFrequencyUpdated(average);

                                std::cout << "Signal center frequency: " << average << " Hz, "
                                          //   << signal_estimate->power_dbfs << " dBFS"
                                          << std::endl;
                            }
                        }
                    }
                }
            }

            if (processedSpectrumChunks == 0)
            {
                continue;
            }

            QVector<float> averagedSpectrum;
            averagedSpectrum.reserve(static_cast<qsizetype>(fftSize));
            const float spectrumScale = 1.0f / static_cast<float>(processedSpectrumChunks);
            for (float value : spectrumAccumulator)
            {
                averagedSpectrum.append(value * spectrumScale);
            }
            emit spectrumReady(averagedSpectrum);

            if (m_settings.demodMode != DemodMode::None)
            {
                const double inputCenterFreq = m_settings.centerFreq;
                const double demodCenterFreq = m_settings.demodCenterFreq;
                const double inputSampleRate = m_settings.sampleRate;
                if (frame.usesInt16)
                {
                    double demodSampleRate = inputSampleRate;
                    auto demodSamples = extractDemodChannel(convertSc16ToFloat(frame.int16Samples, frame.received),
                                                            inputSampleRate,
                                                            inputCenterFreq,
                                                            demodCenterFreq,
                                                            m_settings.demodMode,
                                                            &demodSampleRate,
                                                            &channelizerState);
                    enqueueAudioFrame(
                        AudioFrame{demodSampleRate,
                                   static_cast<float>(m_settings.squelchDb),
                                   m_settings.demodMode,
                                   std::move(demodSamples)});
                }
                else
                {
                    double demodSampleRate = inputSampleRate;
                    AudioFrame audioFrame;
                    audioFrame.sampleRate = demodSampleRate;
                    audioFrame.squelchDb = static_cast<float>(m_settings.squelchDb);
                    audioFrame.demodMode = m_settings.demodMode;
                    audioFrame.samples = extractDemodChannel(frame.floatSamples,
                                                             inputSampleRate,
                                                             inputCenterFreq,
                                                             demodCenterFreq,
                                                             m_settings.demodMode,
                                                             &demodSampleRate,
                                                             &channelizerState);
                    audioFrame.sampleRate = demodSampleRate;
                    enqueueAudioFrame(std::move(audioFrame));
                }
            }
        }
    }
    catch (...)
    {
        storeProcessingError(std::current_exception());
    }

    m_audioProducerDone = true;
    m_audioQueueCv.notify_all();
}

void SdrWorker::audioLoop(std::stop_token stopToken)
{
    try
    {
        DemodulatorState demodState;
        AudioPlaybackContext audioContext;
        auto audioGuard =
            std::unique_ptr<AudioPlaybackContext, decltype(&closeAudioDevice)>(&audioContext, closeAudioDevice);
        if (m_settings.demodMode != DemodMode::None)
        {
            openAudioDevice(&audioContext, kAudioSampleRate);
        }

        while (!stopToken.stop_requested())
        {
            AudioFrame frame;
            {
                std::unique_lock<std::mutex> lock(m_audioQueueMutex);
                m_audioQueueCv.wait(lock, stopToken, [&]
                                    { return m_audioProducerDone || !m_audioQueue.empty() || m_stopRequested; });

                if (m_audioQueue.empty())
                {
                    if (m_audioProducerDone || m_stopRequested || stopToken.stop_requested())
                    {
                        break;
                    }
                    continue;
                }

                frame = std::move(m_audioQueue.front());
                m_audioQueue.pop_front();
            }

            writeAudioFrames(&audioContext,
                             demodulateAudio(frame.samples,
                                             frame.sampleRate,
                                             frame.squelchDb,
                                             frame.demodMode,
                                             &demodState));
        }
    }
    catch (...)
    {
        storeProcessingError(std::current_exception());
    }
}

void SdrWorker::enqueueProcessingFrame(ProcessingFrame &&frame)
{
    if (m_stopRequested)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_processingQueueMutex);
    if (m_processingQueue.size() >= kMaxProcessingQueueDepth)
    {
        m_processingQueue.pop_front();
    }
    m_processingQueue.push_back(std::move(frame));
    m_processingQueueCv.notify_one();
}

void SdrWorker::enqueueAudioFrame(AudioFrame &&frame)
{
    if (m_stopRequested || frame.samples.empty() || frame.demodMode == DemodMode::None)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_audioQueueMutex);
    if (m_audioQueue.size() >= kMaxAudioQueueDepth)
    {
        m_audioQueue.pop_front();
    }
    m_audioQueue.push_back(std::move(frame));
    m_audioQueueCv.notify_one();
}

void SdrWorker::enqueueUdpFrame(const ProcessingFrame &frame)
{
    if (m_stopRequested || !m_settings.udp.enabled || frame.received == 0)
    {
        return;
    }

    ProcessingFrame udpFrame;
    udpFrame.received = frame.received;
    udpFrame.usesInt16 = true;
    if (frame.usesInt16)
    {
        udpFrame.int16Samples = frame.int16Samples;
    }
    else
    {
        udpFrame.int16Samples = quantizeToSc16(frame.floatSamples);
    }
    udpFrame.received = std::min(udpFrame.received, udpFrame.int16Samples.size());

    std::lock_guard<std::mutex> lock(m_udpQueueMutex);
    if (m_udpQueue.size() >= kMaxUdpQueueDepth)
    {
        m_udpQueue.pop_front();
    }
    m_udpQueue.push_back(std::move(udpFrame));
    m_udpQueueCv.notify_one();
}

void SdrWorker::storeProcessingError(std::exception_ptr error)
{
    {
        std::lock_guard<std::mutex> lock(m_processingErrorMutex);
        if (m_processingError == nullptr)
        {
            m_processingError = error;
        }
    }

    m_stopRequested = true;
    requestWorkerStop();
    m_processingQueueCv.notify_all();
    m_audioQueueCv.notify_all();
    m_udpQueueCv.notify_all();
}

void SdrWorker::rethrowProcessingError()
{
    std::lock_guard<std::mutex> lock(m_processingErrorMutex);
    if (m_processingError != nullptr)
    {
        std::rethrow_exception(m_processingError);
    }
}
void SdrWorker::setRxFrontend(const QString &rxFrontend)
{
    m_settings.rxFrontend = rxFrontend;

    if (m_usrp)
    {
        auto subdev_spec = uhd::usrp::subdev_spec_t(QString("%1:%2").arg("A").arg(rxFrontend).toStdString());
        m_usrp->set_rx_subdev_spec(subdev_spec);

        m_usrp->set_rx_rate(m_settings.sampleRate);
        m_usrp->set_rx_freq(m_settings.centerFreq);
        m_usrp->set_rx_gain(m_settings.gain);
        m_usrp->set_rx_bandwidth(m_settings.sampleRate);
        m_usrp->set_rx_antenna(m_settings.antenna.toStdString());
    }
}

void SdrWorker::setRxAntenna(const QString &antenna)
{
    m_settings.antenna = antenna;

    if (m_usrp)
    {
        m_usrp->set_rx_antenna(antenna.toStdString());
    }
}

void SdrWorker::setRxSampleRate(double sampleRate)
{
    m_settings.sampleRate = sampleRate;

    if (m_usrp)
    {
        m_usrp->set_rx_rate(sampleRate);
    }
}

void SdrWorker::setFxCenterFreq(double centerFreq)
{
    m_settings.centerFreq = centerFreq;

    if (m_usrp)
    {
        m_usrp->set_rx_freq(centerFreq);
    }
}

void SdrWorker::setDemodCenterFreq(double centerFreq)
{
    m_settings.demodCenterFreq = centerFreq;
}

void SdrWorker::setRxGain(double gain)
{
    m_settings.gain = gain;

    if (m_usrp)
    {
        m_usrp->set_rx_gain(gain);
    }
}

void SdrWorker::setSquelchDb(double squelchDb)
{
    m_settings.squelchDb = squelchDb;
}

void SdrWorker::setDemodMode(DemodMode mode)
{
    m_settings.demodMode = mode;
}

void SdrWorker::setFftProcessor(FftProcessorMode processor)
{
    m_settings.fftProcessorMode = processor;
}

void SdrWorker::setFftSize(std::size_t fftSize)
{
    m_settings.fftSize = fftSize;

    m_samplesPerBuffer = chooseSamplesPerBuffer(m_settings.sampleRate, fftSize);
}
