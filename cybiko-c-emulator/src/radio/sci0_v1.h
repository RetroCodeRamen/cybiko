#ifndef CYBIKO_SCI0_V1_H
#define CYBIKO_SCI0_V1_H

/*
 * Cybiko Classic (V1) SCI0, interrupt driven, to the radio co-processor,
 * plus the CyOS 1.2.46 radio driver workarounds.
 *
 * Behavior follows AddressBus.java (MIT, Copyright 2026 Dan Berkowitz):
 * tickSci0, tickV1Radio, the SCI0 SCR write, and CybikoEmulator's
 * set_task_state hook. CyOS sends a poll/scan command and the frame byte by
 * byte from TXI0; clearing TIE ends the frame, and the co-processor answers
 * 0x03, an indicator and 50/200 bytes through RXI0.
 *
 * The workarounds are tied to CyOS 1.2.46 RAM addresses (radio object
 * pointer 0x21F216, CyID 0x21F9DC, RF object 0x223186), each read checked
 * against the RAM range before use:
 *  - bootstrap: connObj is only allocated by a completion handler that
 *    needs connObj, so state 9 plus an injected 0x03 starts it;
 *  - poll/scan: in emulation CyOS never calls its radio tick, so one is
 *    started every 15 frames;
 *  - RF service: set_task_state never resolves the RF object, so the first
 *    call after the desktop is up does it.
 */

#include "radio_coproc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYBIKO_SCI0_SMR 0xFFFF78u
#define CYBIKO_SCI0_SCR 0xFFFF7Au
#define CYBIKO_SCI0_TDR 0xFFFF7Bu
#define CYBIKO_SCI0_SSR 0xFFFF7Cu
#define CYBIKO_SCI0_RDR 0xFFFF7Du

#define CYBIKO_SCI0_RXI_VECTOR 81
#define CYBIKO_SCI0_TXI_VECTOR 82

/* CyOS 1.2.46 set_task_state, hooked to resolve the RF service */
#define CYBIKO_V1_SET_TASK_STATE 0x205088u

typedef uint8_t (*cybiko_sci0_read_fn)(void *ctx, uint32_t address);
typedef void (*cybiko_sci0_poke_fn)(void *ctx, uint32_t address, uint8_t value);
typedef void (*cybiko_sci0_irq_fn)(void *ctx, int vector);

typedef struct cybiko_sci0_v1 {
    struct cybiko_radio_coproc *coproc;
    bool bound;
    uint8_t scr;
    bool tdre;
    bool rdrf;
    uint8_t rdr;
    int txi_delay;          /* cycles until TDRE */
    int async_countdown;    /* cycles until the next async check */
    /* bootstrap */
    bool bootstrapped;
    bool bootstrap_pending;
    int64_t bootstrap_wait; /* cycles */
    uint32_t saved_conn;
    /* poll/scan driver */
    int poll_frames;
    int poll_alt;
    bool driver_tx;
    /* identity */
    uint32_t radio_id;      /* 0 = keep the ROM CyID */
    bool cyid_patched;
    /* RF service */
    bool rf_armed;          /* set by the host once the desktop is up */
    bool rf_resolved;
    cybiko_sci0_read_fn read8;
    cybiko_sci0_poke_fn poke8;
    cybiko_sci0_irq_fn irq;
    void *ctx;
    /* counters */
    uint32_t tx_frames;
    uint32_t rx_injected;
} cybiko_sci0_v1_t;

void cybiko_sci0_init(cybiko_sci0_v1_t *s);
void cybiko_sci0_bind(cybiko_sci0_v1_t *s, struct cybiko_radio_coproc *coproc,
                      cybiko_sci0_read_fn read8, cybiko_sci0_poke_fn poke8,
                      cybiko_sci0_irq_fn irq, void *ctx, uint32_t radio_id);

/* SCR, SSR and RDR reads once bound. False: not an SCI0 register. */
bool cybiko_sci0_read(cybiko_sci0_v1_t *s, uint32_t address, uint8_t *out);
/* Guest write to SCI0 register reg (0 = SMR .. 6 = SCMR). */
void cybiko_sci0_on_reg(cybiko_sci0_v1_t *s, int reg, uint8_t value);
/* Advance by guest cycles. */
void cybiko_sci0_advance(cybiko_sci0_v1_t *s, int cycles);
/* Once per frame: the poll/scan driver. */
void cybiko_sci0_frame(cybiko_sci0_v1_t *s);
/* The CPU reached CYBIKO_V1_SET_TASK_STATE. */
void cybiko_sci0_on_set_task_state(cybiko_sci0_v1_t *s);

#ifdef __cplusplus
}
#endif

#endif
