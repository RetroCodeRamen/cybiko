#ifndef CYBIKO_TIMER8_H
#define CYBIKO_TIMER8_H
#include "types.h"

typedef struct h8s_cpu h8s_cpu_t;

typedef struct {
    uint8_t  tcr, tcsr, tcora, tcorb, tcnt;
    int      prescale_counter;
    int      cached_divisor;
    int      vec_cmia, vec_cmib, vec_ovi;
    h8s_cpu_t *cpu;
} timer8_t;

void    timer8_init(timer8_t *t, int channel, h8s_cpu_t *cpu);
void    timer8_tick(timer8_t *t);
uint8_t timer8_read(const timer8_t *t, int reg);
void    timer8_write(timer8_t *t, int reg, uint8_t value);
bool    timer8_is_running(const timer8_t *t);

#endif
