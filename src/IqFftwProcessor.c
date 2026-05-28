#include "IqFftwProcessor.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const double k_int16_scale = 32768.0;
static const double k_min_magnitude = 1e-15;
static const double k_pi = 3.14159265358979323846;

static void iq_fftw_processor_reset(IqFftwProcessor* processor)
{
    if (processor == NULL) {
        return;
    }

    if (processor->plan != NULL) {
        fftw_destroy_plan(processor->plan);
        processor->plan = NULL;
    }

    if (processor->in != NULL) {
        fftw_free(processor->in);
        processor->in = NULL;
    }

    if (processor->out != NULL) {
        fftw_free(processor->out);
        processor->out = NULL;
    }

    free(processor->window);
    processor->window = NULL;
    processor->window_sum = 0.0;
}

static void iq_fftw_fill_hann_window(double* window, size_t fft_size)
{
    size_t i;

    if (window == NULL || fft_size == 0U) {
        return;
    }

    if (fft_size == 1U) {
        window[0] = 1.0;
        return;
    }

    for (i = 0U; i < fft_size; ++i) {
        window[i] = 0.5 * (1.0 - cos((2.0 * k_pi * (double)i) / (double)(fft_size - 1U)));
    }
}

static int iq_fftw_allocate_plan(IqFftwProcessor* processor, size_t fft_size)
{
    size_t i;

    if (processor == NULL || fft_size == 0U) {
        return IQ_FFTW_ERROR_INVALID_ARGUMENT;
    }

    iq_fftw_processor_reset(processor);

    processor->in = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * fft_size);
    processor->out = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * fft_size);
    processor->window = (double*)malloc(sizeof(double) * fft_size);
    if (processor->in == NULL || processor->out == NULL || processor->window == NULL) {
        iq_fftw_processor_reset(processor);
        return IQ_FFTW_ERROR_NO_MEMORY;
    }

    iq_fftw_fill_hann_window(processor->window, fft_size);
    processor->window_sum = 0.0;
    for (i = 0U; i < fft_size; ++i) {
        processor->window_sum += processor->window[i];
    }

    memset(processor->in, 0, sizeof(fftw_complex) * fft_size);
    memset(processor->out, 0, sizeof(fftw_complex) * fft_size);

    processor->plan = fftw_plan_dft_1d((int)fft_size,
        processor->in,
        processor->out,
        FFTW_FORWARD,
        FFTW_MEASURE);
    if (processor->plan == NULL) {
        iq_fftw_processor_reset(processor);
        return IQ_FFTW_ERROR_PLAN_FAILED;
    }

    processor->fft_size = fft_size;
    return IQ_FFTW_OK;
}

int iq_fftw_processor_init(IqFftwProcessor* processor, size_t fft_size)
{
    if (processor == NULL) {
        return IQ_FFTW_ERROR_INVALID_ARGUMENT;
    }

    memset(processor, 0, sizeof(*processor));
    return iq_fftw_allocate_plan(processor, fft_size);
}

void iq_fftw_processor_destroy(IqFftwProcessor* processor)
{
    if (processor == NULL) {
        return;
    }

    iq_fftw_processor_reset(processor);
    processor->fft_size = 0U;
}

int iq_fftw_processor_set_fft_size(IqFftwProcessor* processor, size_t fft_size)
{
    if (processor == NULL || fft_size == 0U) {
        return IQ_FFTW_ERROR_INVALID_ARGUMENT;
    }

    if (processor->fft_size == fft_size && processor->plan != NULL) {
        return IQ_FFTW_OK;
    }

    return iq_fftw_allocate_plan(processor, fft_size);
}

size_t iq_fftw_processor_fft_size(const IqFftwProcessor* processor)
{
    if (processor == NULL) {
        return 0U;
    }

    return processor->fft_size;
}

int iq_fftw_processor_process_i16(IqFftwProcessor* processor,
    const int16_t* iq_interleaved,
    size_t iq_value_count,
    double* spectrum_dbfs_out)
{
    size_t n;
    const double* window = NULL;
    double norm;
    size_t half;

    if (processor == NULL || iq_interleaved == NULL || spectrum_dbfs_out == NULL) {
        return IQ_FFTW_ERROR_INVALID_ARGUMENT;
    }

    if (processor->plan == NULL || processor->fft_size == 0U) {
        return IQ_FFTW_ERROR_NOT_READY;
    }

    if (iq_value_count < processor->fft_size * 2U) {
        return IQ_FFTW_ERROR_INVALID_ARGUMENT;
    }

    window = processor->window;
    for (n = 0U; n < processor->fft_size; ++n) {
        const double i_value = (double)iq_interleaved[2U * n] / k_int16_scale;
        const double q_value = (double)iq_interleaved[2U * n + 1U] / k_int16_scale;
        const double weight = window[n];
        processor->in[n][0] = i_value * weight;
        processor->in[n][1] = q_value * weight;
    }

    fftw_execute(processor->plan);

    norm = (processor->window_sum > 0.0) ? processor->window_sum : (double)processor->fft_size;
    half = processor->fft_size / 2U;
    for (n = 0U; n < processor->fft_size; ++n) {
        const size_t src = (n + half) % processor->fft_size;
        const double real = processor->out[src][0];
        const double imag = processor->out[src][1];
        const double magnitude = sqrt(real * real + imag * imag) / norm;
        const double limited = (magnitude > k_min_magnitude) ? magnitude : k_min_magnitude;
        spectrum_dbfs_out[n] = 20.0 * log10(limited);
    }

    return IQ_FFTW_OK;
}

int iq_fftw_processor_frequency_axis(const IqFftwProcessor* processor,
    double sample_rate_hz,
    double* axis_hz_out)
{
    size_t k;
    size_t half;

    if (processor == NULL || axis_hz_out == NULL) {
        return IQ_FFTW_ERROR_INVALID_ARGUMENT;
    }

    if (processor->fft_size == 0U) {
        return IQ_FFTW_ERROR_NOT_READY;
    }

    half = processor->fft_size / 2U;
    for (k = 0U; k < processor->fft_size; ++k) {
        axis_hz_out[k] = ((double)k - (double)half) * sample_rate_hz / (double)processor->fft_size;
    }

    return IQ_FFTW_OK;
}
