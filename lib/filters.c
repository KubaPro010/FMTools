#include "filters.h"

void init_emphasis(Emphasis *pe, float tau, float sample_rate) {
    pe->prev_sample = 0.0f;
    pe->alpha = exp(-1 / (tau * sample_rate));
}

float apply_pre_emphasis(Emphasis *pe, float sample) {
    float audio = sample-pe->alpha*pe->prev_sample;
    pe->prev_sample = audio;
    return audio*2;
}

void init_low_pass_filter(LowPassFilter *lp, float cutoff_frequency, float sample_rate) {
    for (int i = 0; i < FIR_TAPS; i++) {
        for (int j = 0; j < FIR_PHASES; j++) {
            int mi = i * FIR_PHASES + j + 1;
            float sincpos = mi - (((FIR_TAPS * FIR_PHASES) + 1.0f) / 2.0f);
            float firlowpass = (sincpos == 0.0f) ? 1.0f : sinf(M_2PI * cutoff_frequency * sincpos / sample_rate) / (PI * sincpos);
            float window = 0.54f - 0.46f * cosf(M_2PI * mi / (FIR_TAPS * FIR_PHASES)); // Hamming window
            lp->low_pass_fir[j][i] = firlowpass * window;
        }
    }
    memset(lp->sample_buffer, 0, sizeof(lp->sample_buffer));
    lp->buffer_index = 0;
}

float apply_low_pass_filter(LowPassFilter *lp, float sample) {
    // Update the sample buffer
    lp->sample_buffer[lp->buffer_index] = sample;
    lp->buffer_index = (lp->buffer_index + 1) % FIR_TAPS;

    // Apply the filter
    float result = 0.0f;
    int index = lp->buffer_index;
    for (int i = 0; i < FIR_TAPS; i++) {
        result += lp->low_pass_fir[0][i] * lp->sample_buffer[index];
        index = (index + 1) % FIR_TAPS;
    }
    return result*6;
}

void init_delay_line(DelayLine *delay_line, int max_delay) {
    delay_line->buffer = (float *)calloc(max_delay, sizeof(float));
    delay_line->size = max_delay;
    delay_line->write_idx = 0;
    delay_line->read_idx = 0;
    delay_line->delay = 0;
}

void set_delay_line(DelayLine *delay_line, int new_delay) {
    if (new_delay >= delay_line->size) {
        new_delay = delay_line->size - 1;
    }
    if (new_delay < 0) {
        new_delay = 0;
    }
    
    delay_line->delay = new_delay;
    delay_line->read_idx = (delay_line->write_idx - new_delay + delay_line->size) % delay_line->size;
}

float delay_line(DelayLine *delay_line, float in) {
    float out;
    
    // Read the delayed sample
    out = delay_line->buffer[delay_line->read_idx];
    
    // Write the new sample
    delay_line->buffer[delay_line->write_idx] = in;
    
    // Update indices
    delay_line->write_idx = (delay_line->write_idx + 1) % delay_line->size;
    delay_line->read_idx = (delay_line->read_idx + 1) % delay_line->size;
    
    return out;
}

void exit_delay_line(DelayLine *delay_line) {
    free(delay_line->buffer);
}