#include "FftProcessor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <fftw3.h>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinPower = 1.0e-20f;
}

FftProcessor::FftProcessor(int fftSize)
    : m_fftSize(fftSize),
      m_window(fftSize),
      m_input(fftSize),
      m_output(fftSize),
      m_plan(nullptr)
{
    if (m_fftSize <= 0) {
        throw std::invalid_argument("FFT size must be positive");
    }

    for (int i = 0; i < m_fftSize; ++i) {
        m_window[i] = 0.5f - 0.5f * std::cos((2.0f * kPi * i) / (m_fftSize - 1));
    }

    m_plan = fftwf_plan_dft_1d(
        m_fftSize,
        reinterpret_cast<fftwf_complex *>(m_input.data()),
        reinterpret_cast<fftwf_complex *>(m_output.data()),
        FFTW_FORWARD,
        FFTW_MEASURE);

    if (!m_plan) {
        throw std::runtime_error("Failed to create FFTW plan");
    }
}

FftProcessor::~FftProcessor()
{
    if (m_plan) {
        fftwf_destroy_plan(static_cast<fftwf_plan>(m_plan));
    }
}

std::vector<float> FftProcessor::process(const std::vector<std::complex<float>> &samples)
{
    if (static_cast<int>(samples.size()) < m_fftSize) {
        return {};
    }

    for (int i = 0; i < m_fftSize; ++i) {
        m_input[i] = samples[i] * m_window[i];
    }

    fftwf_execute(static_cast<fftwf_plan>(m_plan));

    std::vector<float> spectrum(m_fftSize);
    const float norm = 1.0f / static_cast<float>(m_fftSize);
    const int half = m_fftSize / 2;

    for (int i = 0; i < m_fftSize; ++i) {
        const int shifted = (i + half) % m_fftSize;
        const float power = std::norm(m_output[shifted]) * norm * norm;
        spectrum[i] = 10.0f * std::log10(std::max(power, kMinPower));
    }

    return spectrum;
}
