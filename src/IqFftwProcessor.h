#pragma once

#include <fftw3.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IqFftwProcessor {
    size_t fft_size;
    fftw_complex* in;
    fftw_complex* out;
    fftw_plan plan;
    double* window;
    double window_sum;
} IqFftwProcessor;

enum {
    IQ_FFTW_OK = 0,
    IQ_FFTW_ERROR_INVALID_ARGUMENT = -1,
    IQ_FFTW_ERROR_NO_MEMORY = -2,
    IQ_FFTW_ERROR_PLAN_FAILED = -3,
    IQ_FFTW_ERROR_NOT_READY = -4
};

int iq_fftw_processor_init(IqFftwProcessor* processor, size_t fft_size);
void iq_fftw_processor_destroy(IqFftwProcessor* processor);
int iq_fftw_processor_set_fft_size(IqFftwProcessor* processor, size_t fft_size);
size_t iq_fftw_processor_fft_size(const IqFftwProcessor* processor);

int iq_fftw_processor_process_i16(IqFftwProcessor* processor,
    const int16_t* iq_interleaved,
    size_t iq_value_count,
    double* spectrum_dbfs_out);

int iq_fftw_processor_frequency_axis(const IqFftwProcessor* processor,
    double sample_rate_hz,
    double* axis_hz_out);

#ifdef __cplusplus
}
#endif
