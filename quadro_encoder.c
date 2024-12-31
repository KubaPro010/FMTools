// what am i doing with my life, writing some quadro encoders? (https://en.wikipedia.org/wiki/FM_broadcasting#Quadraphonic_FM)

#include <stdio.h>
#include <pulse/simple.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>

// Features
#define PREEMPHASIS
#define LPF

#define SAMPLE_RATE 192000 // Don't go lower than 182 KHz (91*2)

#define INPUT_DEVICE "real_real_tx_audio_input.monitor"
#define OUTPUT_DEVICE "alsa_output.platform-soc_sound.stereo-fallback"
#define BUFFER_SIZE 512
#define CLIPPER_THRESHOLD 0.425 // Adjust this as needed

#define MONO_VOLUME 0.45f // L+R Signal
#define PILOT_VOLUME 0.0225f // 19 KHz Pilot
#define SIN38_VOLUME 0.35f
#define COS38_VOLUME 0.35f
#define SIN76_VOLUME 0.35f

#ifdef PREEMPHASIS
#define PREEMPHASIS_TAU 0.00005  // 50 microseconds, use 0.000075 if in america
#endif

#ifdef LPF
#define LPF_CUTOFF 15000
#endif

volatile sig_atomic_t to_run = 1;

float clip(float sample) {
    if (sample > CLIPPER_THRESHOLD) {
        return CLIPPER_THRESHOLD;  // Clip to the upper threshold
    } else if (sample < -CLIPPER_THRESHOLD) {
        return -CLIPPER_THRESHOLD;  // Clip to the lower threshold
    } else {
        return sample;  // No clipping
    }
}

void uninterleave(const float *input, float *front_left, float *front_right, float *rear_left, float *rear_right, size_t num_samples) {
    for (size_t i = 0; i < num_samples / 4; i++) {
        front_left[i] = input[i * 4];
        front_right[i] = input[i * 4 + 1];
        rear_left[i] = input[i * 4 + 2];
        rear_right[i] = input[i * 4 + 3];
    }
}


#define FIR_PHASES 32
#define FIR_TAPS 32

#define PI 3.14159265358979323846
#define M_2PI (3.14159265358979323846 * 2.0)

// Track phase continuously to maintain frequency accuracy
typedef struct {
    float phase;
    float phase_increment;
} Oscillator;

void init_oscillator(Oscillator *osc, float frequency, float sample_rate) {
    osc->phase = 0.0f;
    osc->phase_increment = (M_2PI * frequency) / sample_rate;
}

float get_next_sample(Oscillator *osc) {
    float sample = sinf(osc->phase);
    osc->phase += osc->phase_increment;
    if (osc->phase >= M_2PI) {
        osc->phase -= M_2PI;
    }
    return sample;
}

#ifdef PREEMPHASIS
typedef struct {
    float alpha;
    float prev_sample;
} Emphasis;

void init_emphasis(Emphasis *pe, float sample_rate) {
    pe->prev_sample = 0.0f;
    pe->alpha = exp(-1 / (PREEMPHASIS_TAU * sample_rate));
}

float apply_pre_emphasis(Emphasis *pe, float sample) {
    float audio = sample-pe->alpha*pe->prev_sample;
    pe->prev_sample = audio;
    return audio*2;
}
#endif

#ifdef LPF
typedef struct {
    float low_pass_fir[FIR_PHASES][FIR_TAPS];
    float sample_buffer[FIR_TAPS];
    int buffer_index;
} LowPassFilter;

void init_low_pass_filter(LowPassFilter *lp, float sample_rate) {
    for (int i = 0; i < FIR_TAPS; i++) {
        for (int j = 0; j < FIR_PHASES; j++) {
            int mi = i * FIR_PHASES + j + 1;
            float sincpos = mi - (((FIR_TAPS * FIR_PHASES) + 1.0f) / 2.0f);
            float firlowpass = (sincpos == 0.0f) ? 1.0f : sinf(M_2PI * LPF_CUTOFF * sincpos / sample_rate) / (PI * sincpos);
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
#endif

static void stop(int signum) {
    (void)signum;
    printf("\nReceived stop signal. Cleaning up...\n");
    to_run = 0;
}

int main() {
    printf("QDCode : Quad encoder made by radio95 (with help of ChatGPT and Claude, thanks!)\n");
    // Define formats and buffer atributes
    pa_sample_spec stereo_format = {
        .format = PA_SAMPLE_FLOAT32NE, //Float32 NE, or Float32 Native Endian, the float in c uses the endianess of your pc, or native endian, and float is float32, and double is float64
        .channels = 4,
        .rate = SAMPLE_RATE // Same sample rate makes it easy, leave the resampling to pipewire, it should know better
    };
    pa_sample_spec mono_format = {
        .format = PA_SAMPLE_FLOAT32NE,
        .channels = 1,
        .rate = SAMPLE_RATE
    };

    pa_buffer_attr input_buffer_atr = {
        .maxlength = 4096, // You can lower this to 512, but this is fine, it's sub-second delay, you're probably not gonna notice unless you're looking for it
	    .fragsize = 2048
    };
    pa_buffer_attr output_buffer_atr = {
        .maxlength = 4096,
        .tlength = 2048,
	    .prebuf = 0
    };

    printf("Connecting to input device... (%s)\n", INPUT_DEVICE);

    pa_simple *input_device = pa_simple_new(
        NULL,
        "QuadCoder",
        PA_STREAM_RECORD,
        INPUT_DEVICE,
        "Audio Input",
        &stereo_format,
        NULL,
        &input_buffer_atr,
        NULL
    );
    if (!input_device) {
        fprintf(stderr, "Error: cannot open input device.\n");
        return 1;
    }

    printf("Connecting to output device... (%s)\n", OUTPUT_DEVICE);

    pa_simple *output_device = pa_simple_new(
        NULL,
        "QuadCoder",
        PA_STREAM_PLAYBACK,
        OUTPUT_DEVICE,
        "MPX",
        &mono_format,
        NULL,
        &output_buffer_atr,
        NULL
    );
    if (!output_device) {
        fprintf(stderr, "Error: cannot open output device.\n");
        pa_simple_free(input_device);
        return 1;
    }

    Oscillator pilot_osc;
    init_oscillator(&pilot_osc, 19000.0, SAMPLE_RATE); // Pilot, it's there to indicate stereo and as a refrence signal with the stereo carrier
#ifdef PREEMPHASIS
    Emphasis preemp_lf, preemp_lr, preemp_rf, preemp_rr;
    init_emphasis(&preemp_lf, SAMPLE_RATE);
    init_emphasis(&preemp_lr, SAMPLE_RATE);
    init_emphasis(&preemp_rf, SAMPLE_RATE);
    init_emphasis(&preemp_rr, SAMPLE_RATE);
#endif
#ifdef LPF
    LowPassFilter lpf_lf, lpf_lr, lpf_rf, lpf_rr;
    init_low_pass_filter(&lpf_lf, SAMPLE_RATE);
    init_low_pass_filter(&lpf_lr, SAMPLE_RATE);
    init_low_pass_filter(&lpf_rf, SAMPLE_RATE);
    init_low_pass_filter(&lpf_rr, SAMPLE_RATE);
#endif

    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    
    float input[BUFFER_SIZE*4]; // Input from device, interleaved
    float left_front[BUFFER_SIZE+64], left_rear[BUFFER_SIZE+64]; // Audio, same thing as in input but ininterleaved, ai told be there could be a buffer overflow here
    float right_front[BUFFER_SIZE+64], right_rear[BUFFER_SIZE+64]; // Audio, same thing as in input but ininterleaved, ai told be there could be a buffer overflow here
    float mpx[BUFFER_SIZE]; // MPX, this goes to the output
    while (to_run) {
        if (pa_simple_read(input_device, input, sizeof(input), NULL) < 0) {
            fprintf(stderr, "Error reading from input device.\n");
            break;
        }
        uninterleave(input, left_front, right_front, left_rear, right_rear, BUFFER_SIZE*4);

        for (int i = 0; i < BUFFER_SIZE; i++) {
            float sin38 = sinf((pilot_osc.phase+(0.5*PI))*2);
            float cos38 = cosf((pilot_osc.phase+(0.5*PI))*2);
            float sin76 = sinf((pilot_osc.phase+(0.5*PI))*4);
            float pilot = get_next_sample(&pilot_osc);
            float lf_in = left_front[i];
            float lr_in = left_rear[i];
            float rf_in = right_front[i];
            float rr_in = right_rear[i];

#ifdef PREEMPHASIS
#ifdef LPF
            float lowpassed_frontleft = apply_low_pass_filter(&lpf_lf, lf_in);
            float lowpassed_frontright = apply_low_pass_filter(&lpf_rf, rf_in);
            float lowpassed_rearleft = apply_low_pass_filter(&lpf_lr, lr_in);
            float lowpassed_rearright = apply_low_pass_filter(&lpf_rr, rr_in);
            float preemphasized_frontleft = apply_pre_emphasis(&preemp_lf, lowpassed_frontleft);
            float preemphasized_frontright = apply_pre_emphasis(&preemp_rf, lowpassed_frontright);
            float preemphasized_rearleft = apply_pre_emphasis(&preemp_lr, lowpassed_rearleft);
            float preemphasized_rearright = apply_pre_emphasis(&preemp_rr, lowpassed_rearright);
            float current_lf_input = clip(preemphasized_frontleft);
            float current_rf_input = clip(preemphasized_frontright);
            float current_lr_input = clip(preemphasized_rearleft);
            float current_rr_input = clip(preemphasized_rearright);
#else
            float preemphasized_frontleft = apply_pre_emphasis(&preemp_lf, lf_in);
            float preemphasized_frontright = apply_pre_emphasis(&preemp_rf, rf_in);
            float preemphasized_rearleft = apply_pre_emphasis(&preemp_lr, lr_in);
            float preemphasized_rearright = apply_pre_emphasis(&preemp_rr, rr_in);
            float current_lf_input = clip(preemphasized_frontleft);
            float current_rf_input = clip(preemphasized_frontright);
            float current_lr_input = clip(preemphasized_rearleft);
            float current_rr_input = clip(preemphasized_rearright);
#endif
#else
#ifdef LPF
            float lowpassed_frontleft = apply_low_pass_filter(&lpf_lf, lf_in);
            float lowpassed_frontright = apply_low_pass_filter(&lpf_rf, rf_in);
            float lowpassed_rearleft = apply_low_pass_filter(&lpf_lr, lr_in);
            float lowpassed_rearright = apply_low_pass_filter(&lpf_rr, rr_in);
            float current_lf_input = clip(lowpassed_frontleft);
            float current_rf_input = clip(lowpassed_frontright);
            float current_lr_input = clip(lowpassed_rearleft);
            float current_rr_input = clip(lowpassed_rearright);
#else
            float current_lf_input = clip(lf_in);
            float current_rf_input = clip(rf_in);
            float current_lr_input = clip(lr_in);
            float current_rr_input = clip(rr_in);
#endif
#endif

            float mono = (current_lf_input+current_rf_input+current_lr_input+current_rr_input)/4;
            float signal_sin38 = ((current_lf_input+current_lr_input)-(current_rf_input+current_rr_input))/4;
            float signal_cos38 = ((current_lf_input+current_rr_input)-(current_lr_input+current_rf_input))/4;
            float signal_sin76 = ((current_lf_input+current_rf_input)-(current_lr_input+current_rr_input))/4;

            mpx[i] = mono * MONO_VOLUME +
                pilot * PILOT_VOLUME +
                (sin38*signal_sin38)*SIN38_VOLUME +
                (cos38*signal_cos38)*COS38_VOLUME +
                (sin76*signal_sin76)*SIN76_VOLUME;

        }

        if (pa_simple_write(output_device, mpx, sizeof(mpx), NULL) < 0) {
            fprintf(stderr, "Error writing to output device.\n");
            break;
        }
    }
    printf("Cleaning up...\n");
    pa_simple_free(input_device);
    pa_simple_free(output_device);
    return 0;
}
