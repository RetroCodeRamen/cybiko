#ifndef CYBIKO_TIMER16_H
#define CYBIKO_TIMER16_H
#include "types.h"

typedef struct h8s_cpu h8s_cpu_t;

/* Callback for timer output compare pin changes */
typedef void (*timer16_output_cb)(void *ctx, int level);

typedef struct {
    uint8_t  tcr, tmdr, tior, tier, tsr;
    uint16_t tcnt, tgra, tgrb;
    int      prescale_counter;
    int      cached_divisor;
    bool     enabled;
    int      channel, tgr_count;
    int      vec_tgia, vec_tgib, vec_ovf;
    const int *clock_divisors;
    h8s_cpu_t *cpu;

    /* Output compare pin state (TIOCA/TIOCB) */
    int      output_a_level;
    int      output_b_level;
    timer16_output_cb output_b_cb;  /* Called when TIOCB pin changes */
    void             *output_b_ctx;
} timer16_t;

void     timer16_init(timer16_t *t, int channel, int tgr_count, int base_vector, h8s_cpu_t *cpu);
void     timer16_tick(timer16_t *t);
uint8_t  timer16_read8(const timer16_t *t, int reg);
uint16_t timer16_read16(const timer16_t *t, int reg);
void     timer16_write8(timer16_t *t, int reg, uint8_t value);
void     timer16_write16(timer16_t *t, int reg, uint16_t value);
void     timer16_set_enabled(timer16_t *t, bool enabled);
bool     timer16_is_running(const timer16_t *t);

#endif
