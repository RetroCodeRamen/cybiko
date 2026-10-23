/*
 * H8S 8-bit timer (TMR) emulation.
 *
 * The H8S/2323 has two 8-bit timer channels (TMR0, TMR1).
 * Each has TCR, TCSR, TCORA, TCORB, TCNT registers.
 *
 * Interrupt vectors:
 *   Channel 0: CMIA=64, CMIB=65, OVI=66
 *   Channel 1: CMIA=68, CMIB=69, OVI=70
 */
#include "core/timer8.h"
#include "core/h8s_cpu.h"

/* TCR bits */
#define TCR_CMIEB 0x80  /* Compare Match Interrupt Enable B */
#define TCR_CMIEA 0x40  /* Compare Match Interrupt Enable A */
#define TCR_OVIE  0x20  /* Overflow Interrupt Enable */

/* TCSR bits */
#define TCSR_CMFB 0x80  /* Compare Match Flag B */
#define TCSR_CMFA 0x40  /* Compare Match Flag A */
#define TCSR_OVF  0x20  /* Overflow Flag */

/* Clock divisors indexed by CKS (bits 2-0 of TCR) */
static const int clock_divisors[8] = {
    0, 8, 64, 8192, 256, 256, 256, 256
};

void timer8_init(timer8_t *t, int channel, h8s_cpu_t *cpu) {
    t->tcr   = 0;
    t->tcsr  = 0;
    t->tcora = 0xFF;
    t->tcorb = 0xFF;
    t->tcnt  = 0;
    t->prescale_counter = 0;
    t->cached_divisor   = 0;
    t->cpu = cpu;

    if (channel == 0) {
        t->vec_cmia = 64;
        t->vec_cmib = 65;
        t->vec_ovi  = 66;
    } else {
        t->vec_cmia = 68;
        t->vec_cmib = 69;
        t->vec_ovi  = 70;
    }
}

uint8_t timer8_read(const timer8_t *t, int reg) {
    switch (reg) {
    case 0: return t->tcr;
    case 2: return t->tcsr;
    case 4: return t->tcora;
    case 6: return t->tcorb;
    case 8: return t->tcnt;
    default: return 0;
    }
}

void timer8_write(timer8_t *t, int reg, uint8_t value) {
    switch (reg) {
    case 0:
        t->tcr = value;
        t->cached_divisor = clock_divisors[value & 0x07];
        break;
    case 2:
        /* Flags (bits 7-5): write-0-to-clear; lower 5 bits writable directly */
        t->tcsr = (t->tcsr & ~0x1F) | (value & 0x1F);
        t->tcsr &= value | 0x1F;
        break;
    case 4:
        t->tcora = value;
        break;
    case 6:
        t->tcorb = value;
        break;
    case 8:
        t->tcnt = value;
        break;
    default:
        break;
    }
}

void timer8_tick(timer8_t *t) {
    int divisor = t->cached_divisor;
    if (divisor == 0) return; /* Timer stopped */

    if (++t->prescale_counter < divisor) return;
    t->prescale_counter = 0;

    /* Increment counter */
    t->tcnt = (t->tcnt + 1) & 0xFF;

    /* Check compare match A */
    if (t->tcnt == t->tcora) {
        if (!(t->tcsr & TCSR_CMFA)) {
            t->tcsr |= TCSR_CMFA;
            if (t->tcr & TCR_CMIEA) {
                h8s_cpu_request_interrupt(t->cpu, t->vec_cmia);
            }
        }
        if (((t->tcr >> 3) & 0x03) == 1) {
            t->tcnt = 0;
        }
    }

    /* Check compare match B */
    if (t->tcnt == t->tcorb) {
        if (!(t->tcsr & TCSR_CMFB)) {
            t->tcsr |= TCSR_CMFB;
            if (t->tcr & TCR_CMIEB) {
                h8s_cpu_request_interrupt(t->cpu, t->vec_cmib);
            }
        }
        if (((t->tcr >> 3) & 0x03) == 2) {
            t->tcnt = 0;
        }
    }

    /* Check overflow (only on natural wrap 0xFF -> 0x00) */
    if (t->tcnt == 0 && !(t->tcsr & TCSR_OVF)) {
        t->tcsr |= TCSR_OVF;
        if (t->tcr & TCR_OVIE) {
            h8s_cpu_request_interrupt(t->cpu, t->vec_ovi);
        }
    }
}

bool timer8_is_running(const timer8_t *t) {
    return t->cached_divisor != 0;
}
