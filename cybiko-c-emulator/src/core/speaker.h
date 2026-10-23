#ifndef CYBIKO_SPEAKER_H
#define CYBIKO_SPEAKER_H
#include "types.h"

#define SPEAKER_SAMPLE_RATE 48000
#define SPEAKER_BUFFER_SIZE 2048
#define SPEAKER_MAX_TRANSITIONS 4096

typedef struct {
    int      cycle;     /* CPU cycle within frame when transition occurred */
    int      level;     /* New speaker level (0 or 1) */
} speaker_transition_t;

typedef struct {
    int      current_level;
    int      frame_start_level;    /* Level at the beginning of the current frame */
    double   cycles_per_sample;
    double   cycle_fraction;
    uint8_t  buffer[SPEAKER_BUFFER_SIZE];
    int      buffer_pos;

    /* Per-frame transition tracking */
    int      frame_cycle;          /* Current cycle within the frame (set by emulator) */
    speaker_transition_t transitions[SPEAKER_MAX_TRANSITIONS];
    int      transition_count;
} speaker_t;

void speaker_init(speaker_t *spk, int clock_hz);
void speaker_set_level(speaker_t *spk, int level);
void speaker_begin_frame(speaker_t *spk);
int  speaker_generate_samples(speaker_t *spk, int cpu_cycles, uint8_t *out, int out_size);

#endif
