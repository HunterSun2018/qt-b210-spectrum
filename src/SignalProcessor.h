#pragma once

#include <optional>
#include <vector>
#include <complex>

struct SignalEstimate {
    double f_left_Hz;
    double f_right_Hz;
    double bandwidth_Hz;
    double signal_center_Hz;
    double signal_offset_Hz;
    double threshold_dB;
    double noise_floor_dB;
};

std::optional<SignalEstimate> estimate_signal(
    const std::vector<std::complex<float>>& iq,
    double Fs = 320000.0,
    double fc = 172.68e6,
    int NFFT = 16384,
    double threshold_dB = 20.0
) ;
