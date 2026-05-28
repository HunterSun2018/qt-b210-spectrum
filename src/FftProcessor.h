#pragma once

#include <complex>
#include <vector>

class FftProcessor
{
public:
    explicit FftProcessor(int fftSize);
    ~FftProcessor();

    FftProcessor(const FftProcessor &) = delete;
    FftProcessor &operator=(const FftProcessor &) = delete;

    int fftSize() const { return m_fftSize; }
    std::vector<float> process(const std::vector<std::complex<float>> &samples);

private:
    int m_fftSize;
    std::vector<float> m_window;
    std::vector<std::complex<float>> m_input;
    std::vector<std::complex<float>> m_output;
    void *m_plan;
};
