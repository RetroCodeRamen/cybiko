#ifndef CYBIKO_SCI2_XT_H
#define CYBIKO_SCI2_XT_H

/*
 * XT SCI2 plus the DTC shortcut used by CyOS for the radio co-processor.
 *
 * Behavior follows AddressBus.java (MIT, Copyright 2026 Dan Berkowitz):
 * executeSci2Dtc, tickSci2, and the DTCERF write at 0xFFFF35. The C core
 * keeps today's "always TDRE" SCI2 until a co-processor is attached.
 *
 * DTCERF bit 6 is cleared by a direct memory store. That store must not
 * re-enter the DTCERF handler: the 0x03 and the frame indicator are queued
 * only when a later guest write clears bit 6. Local 0x03 is the AVR command
 * acknowledgement, not a peer RF acknowledgement.
 */

#include "radio_coproc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYBIKO_SCI2_SCR    0xFFFF8Au
#define CYBIKO_SCI2_TDR    0xFFFF8Bu
#define CYBIKO_SCI2_SSR    0xFFFF8Cu
#define CYBIKO_SCI2_RDR    0xFFFF8Du
#define CYBIKO_SCI2_DTCERF 0xFFFF35u
#define CYBIKO_SCI2_TX_DTC 0xFFFBF4u
#define CYBIKO_SCI2_RX_DTC 0xFFFBE8u

#define CYBIKO_SCI2_TXI_VECTOR 90
#define CYBIKO_SCI2_RXI_VECTOR 89
#define CYBIKO_SCI2_TXI_DELAY  32
#define CYBIKO_SCI2_ASYNC_INTERVAL 512

#define CYBIKO_SCI2_SSR_TDRE 0x80u
#define CYBIKO_SCI2_SSR_RDRF 0x40u
#define CYBIKO_SCI2_SSR_TEND 0x04u

typedef uint8_t (*cybiko_sci2_read_fn)(void *ctx, uint32_t address);
typedef void (*cybiko_sci2_poke_fn)(void *ctx, uint32_t address, uint8_t value);
typedef void (*cybiko_sci2_irq_fn)(void *ctx, int vector);

typedef struct cybiko_sci2_xt {
    struct cybiko_radio_coproc *coproc;
    bool bound;
    uint8_t scr;
    bool tdre;
    bool rdrf;
    uint8_t rdr;
    int txi_delay;
    int pending_indicator;
    int async_countdown;
    cybiko_sci2_read_fn read8;
    cybiko_sci2_poke_fn poke8;
    cybiko_sci2_irq_fn irq;
    void *ctx;
} cybiko_sci2_xt_t;

void cybiko_sci2_init(cybiko_sci2_xt_t *s);
void cybiko_sci2_bind(cybiko_sci2_xt_t *s, struct cybiko_radio_coproc *coproc,
                      cybiko_sci2_read_fn read8, cybiko_sci2_poke_fn poke8,
                      cybiko_sci2_irq_fn irq, void *ctx);

bool cybiko_sci2_read(cybiko_sci2_xt_t *s, uint32_t address, uint8_t *out);
void cybiko_sci2_on_reg(cybiko_sci2_xt_t *s, int reg, uint8_t value);
void cybiko_sci2_on_dtcerf(cybiko_sci2_xt_t *s, uint8_t value);
void cybiko_sci2_tick(cybiko_sci2_xt_t *s);
/* n ticks; skips the ones that only count down. */
void cybiko_sci2_advance(cybiko_sci2_xt_t *s, int n);

/* Guest traffic counters for the desktop boot runner. Both instances share them. */
extern uint32_t cybiko_sci2_reg_writes;
extern uint32_t cybiko_sci2_dtc_runs;

#ifdef __cplusplus
}
#endif

#endif
