#include <fftw3.h>
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <optional>
#include "SignalProcessor.h"


static double median(std::vector<double> v) {
    if (v.empty()) return 0.0;

    std::sort(v.begin(), v.end());
    size_t n = v.size();

    if (n % 2 == 0)
        return 0.5 * (v[n / 2 - 1] + v[n / 2]);
    else
        return v[n / 2];
}

std::optional<SignalEstimate> estimate_signal(
    const std::vector<std::complex<float>>& iq,
    double Fs,
    double fc,
    int NFFT,
    double threshold_dB
) {
    if (iq.size() < static_cast<size_t>(NFFT)) {
        std::cerr << "IQ data length is smaller than NFFT\n";
        return std::nullopt;
    }

    std::vector<std::complex<double>> input(NFFT);
    std::vector<std::complex<double>> output(NFFT);

    // 1. 加 Hann 窗
    for (int n = 0; n < NFFT; ++n) {
        double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * n / (NFFT - 1));
        input[n] = std::complex<double>(
            iq[n].real() * w,
            iq[n].imag() * w
        );
    }

    // 2. FFT
    fftw_plan plan = fftw_plan_dft_1d(
        NFFT,
        reinterpret_cast<fftw_complex*>(input.data()),
        reinterpret_cast<fftw_complex*>(output.data()),
        FFTW_FORWARD,
        FFTW_ESTIMATE
    );

    fftw_execute(plan);
    fftw_destroy_plan(plan);

    // 3. fftshift + 计算功率谱
    std::vector<double> power_db(NFFT);
    std::vector<double> freq_offset(NFFT);
    std::vector<double> freq_rf(NFFT);

    double bin_bw = Fs / NFFT;

    for (int i = 0; i < NFFT; ++i) {
        int shifted_index = (i + NFFT / 2) % NFFT;

        double mag = std::abs(output[shifted_index]);
        power_db[i] = 20.0 * std::log10(mag + 1e-12);

        freq_offset[i] = (i - NFFT / 2) * bin_bw;
        freq_rf[i] = fc + freq_offset[i];
    }

    // 4. 搜索范围：-10 kHz ~ +60 kHz
    std::vector<double> search_power;

    for (int i = 0; i < NFFT; ++i) {
        // if (freq_offset[i] > -10e3 && freq_offset[i] < 60e3) 
        {
            search_power.push_back(power_db[i]);
        }
    }

    if (search_power.empty()) {
        return std::nullopt;
    }

    double noise_floor = median(search_power);
    double threshold = noise_floor + threshold_dB;

    // 5. 找超过门限的 bin
    std::vector<int> idx;

    for (int i = 0; i < NFFT; ++i) {
        // bool in_search = freq_offset[i] > -10e3 && freq_offset[i] < 60e3;
        bool above_threshold = power_db[i] > threshold;

        if (above_threshold) {
            idx.push_back(i);
        }
    }

    if (idx.empty()) {
        return std::nullopt;
    }

    int i_left = idx.front();
    int i_right = idx.back();

    double f_left = freq_rf[i_left];
    double f_right = freq_rf[i_right];

    SignalEstimate result;
    result.f_left_Hz = f_left;
    result.f_right_Hz = f_right;
    result.bandwidth_Hz = f_right - f_left;
    result.signal_center_Hz = 0.5 * (f_left + f_right);
    result.signal_offset_Hz = result.signal_center_Hz - fc;
    result.threshold_dB = threshold;
    result.noise_floor_dB = noise_floor;

    return result;
}

void frequency_shift_to_center(
    std::vector<std::complex<float>>& iq,
    double Fs,
    double original_center_freq,
    double signal_center_freq
) {
    double f_offset = signal_center_freq - original_center_freq;

    double phase_inc = -2.0 * M_PI * f_offset / Fs;
    double phase = 0.0;

    for (auto& sample : iq) {
        std::complex<float> mixer(
            static_cast<float>(std::cos(phase)),
            static_cast<float>(std::sin(phase))
        );

        sample *= mixer;

        phase += phase_inc;

        // 防止 phase 无限增大
        if (phase > M_PI)
            phase -= 2.0 * M_PI;
        else if (phase < -M_PI)
            phase += 2.0 * M_PI;
    }
}