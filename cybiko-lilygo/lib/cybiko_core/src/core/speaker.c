/*
 * 1-bit speaker audio output.
 *
 * The Cybiko speaker is driven by a single digital pin (Port 1 bit 3).
 * This module converts pin level transitions into PCM audio samples.
 *
 * Level transitions are recorded with their cycle position during CPU
 * execution, then converted to proper waveform samples at frame end.
 */
#include "core/speaker.h"
#include <string.h>

void speaker_init(speaker_t *spk, int clock_hz) {
    memset(spk, 0, sizeof(*spk));
    spk->cycles_per_sample_q8 = ((int64_t)clock_hz << 8) / SPEAKER_SAMPLE_RATE;
    spk->cycle_fraction_q8 = 0;
    spk->current_level = 0;
    spk->frame_start_level = 0;
    spk->buffer_pos = 0;
    spk->frame_cycle = 0;
    spk->transition_count = 0;
}

void speaker_begin_frame(speaker_t *spk) {
    spk->frame_start_level = spk->current_level;
    spk->frame_cycle = 0;
    spk->transition_count = 0;
}

void speaker_set_level(speaker_t *spk, int level) {
    if (level != spk->current_level) {
        spk->current_level = level;
        if (spk->transition_count < SPEAKER_MAX_TRANSITIONS) {
            spk->transitions[spk->transition_count].cycle = spk->frame_cycle;
            spk->transitions[spk->transition_count].level = level;
            spk->transition_count++;
        }
    }
}

int speaker_generate_samples(speaker_t *spk, int cpu_cycles, uint8_t *out, int out_size) {
    spk->cycle_fraction_q8 += cpu_cycles << 8;
    int samples_to_write = spk->cycle_fraction_q8 / spk->cycles_per_sample_q8;
    spk->cycle_fraction_q8 -= samples_to_write * spk->cycles_per_sample_q8;

    if (samples_to_write > out_size) {
        samples_to_write = out_size;
    }

    if (spk->transition_count == 0) {
        /* No transitions this frame - output silence (constant level = no sound) */
        memset(out, 128, samples_to_write);
    } else {
        /* Walk through transitions to produce proper waveform */
        int trans_idx = 0;
        int level = spk->frame_start_level;

        for (int i = 0; i < samples_to_write; i++) {
            /* Midpoint cycle for this sample in Q8, then shift back */
            int sample_cycle = ((2 * i + 1) * spk->cycles_per_sample_q8) >> 9;

            /* Advance through transitions up to this sample's cycle */
            while (trans_idx < spk->transition_count &&
                   spk->transitions[trans_idx].cycle <= sample_cycle) {
                level = spk->transitions[trans_idx].level;
                trans_idx++;
            }

            out[i] = (level == 0) ? 96 : 160;
        }
    }

    return samples_to_write;
}
