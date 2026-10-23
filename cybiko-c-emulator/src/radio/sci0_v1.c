#include "sci0_v1.h"

#include <string.h>

/* CyOS 1.2.46 radio object layout (AddressBus.java) */
#define V1_RADIO_OBJ_PTR 0x21F216u
#define V1_STATE_OFF     0x3370u
#define V1_CYID_RAM      0x21F9DCu

#define RAM_LO 0x200000u
#define RAM_HI 0x280000u

/* Java counts these in CPU steps at one instruction per cycle */
#define TXI_DELAY            32
#define ASYNC_INTERVAL       512

void cybiko_sci0_init(cybiko_sci0_v1_t *s) {
    memset(s, 0, sizeof *s);
    s->tdre = true;
}

void cybiko_sci0_bind(cybiko_sci0_v1_t *s, struct cybiko_radio_coproc *coproc,
                      cybiko_sci0_read_fn read8, cybiko_sci0_poke_fn poke8,
                      cybiko_sci0_irq_fn irq, void *ctx, uint32_t radio_id) {
    cybiko_sci0_init(s);
    s->coproc = coproc;
    s->bound = coproc != NULL;
    s->read8 = read8;
    s->poke8 = poke8;
    s->irq = irq;
    s->ctx = ctx;
    s->radio_id = radio_id;
    if (coproc)
        cybiko_radio_coproc_set_v1(coproc, true);
}

static void sci0_raise_irq(cybiko_sci0_v1_t *s, int vector) {
    if (s->irq)
        s->irq(s->ctx, vector);
}

static uint32_t sci0_read32(cybiko_sci0_v1_t *s, uint32_t a) {
    return ((uint32_t)s->read8(s->ctx, a) << 24) |
           ((uint32_t)s->read8(s->ctx, a + 1) << 16) |
           ((uint32_t)s->read8(s->ctx, a + 2) << 8) |
           (uint32_t)s->read8(s->ctx, a + 3);
}

static void sci0_poke32(cybiko_sci0_v1_t *s, uint32_t a, uint32_t v) {
    s->poke8(s->ctx, a, (uint8_t)(v >> 24));
    s->poke8(s->ctx, a + 1, (uint8_t)(v >> 16));
    s->poke8(s->ctx, a + 2, (uint8_t)(v >> 8));
    s->poke8(s->ctx, a + 3, (uint8_t)v);
}

static bool sci0_in_ram(uint32_t a) { return a >= RAM_LO && a < RAM_HI; }

static uint32_t sci0_radio_obj(cybiko_sci0_v1_t *s) {
    uint32_t obj = sci0_read32(s, V1_RADIO_OBJ_PTR);
    return sci0_in_ram(obj) && sci0_in_ram(obj + V1_STATE_OFF) ? obj : 0;
}

bool cybiko_sci0_read(cybiko_sci0_v1_t *s, uint32_t address, uint8_t *out) {
    if (!s->bound)
        return false;
    switch (address) {
    case CYBIKO_SCI0_SCR:
        *out = s->scr;
        return true;
    case CYBIKO_SCI0_SSR:
        *out = (uint8_t)((s->tdre ? 0x84 : 0) | (s->rdrf ? 0x40 : 0));
        return true;
    case CYBIKO_SCI0_RDR:
        s->rdrf = false;
        *out = s->rdr;
        return true;
    }
    return false;
}

static void sci0_write_scr(cybiko_sci0_v1_t *s, uint8_t value) {
    uint8_t old = s->scr;
    s->scr = value;
    if (value == 0)
        s->rdrf = false;
    /* RIE just enabled with a byte already waiting */
    if ((value & 0x40) && !(old & 0x40) && s->rdrf)
        sci0_raise_irq(s, CYBIKO_SCI0_RXI_VECTOR);
    /* TIE just enabled with TDRE set: first TXI0 after a byte time */
    if ((value & 0x80) && s->tdre && (!(old & 0x80) || s->txi_delay == 0))
        s->txi_delay = TXI_DELAY;
    /* TIE 1 -> 0: the TXI0 state machine finished the frame */
    if ((old & 0x80) && !(value & 0x80)) {
        if (s->coproc->v1_accum) {
            s->tx_frames++;
            s->radio_up = true;
        }
        cybiko_radio_coproc_v1_tx_complete(s->coproc);
    }
}

void cybiko_sci0_on_reg(cybiko_sci0_v1_t *s, int reg, uint8_t value) {
    if (!s->bound)
        return;
    if (reg == 3) {   /* TDR */
        cybiko_radio_coproc_receive(s->coproc, value);
        s->tdre = false;
        s->txi_delay = TXI_DELAY;
    } else if (reg == 2) {
        sci0_write_scr(s, value);
    }
}

static void sci0_async_delivery(cybiko_sci0_v1_t *s) {
    if (!s->radio_up || s->rdrf || cybiko_radio_coproc_rx_count(s->coproc) > 0 ||
        cybiko_radio_coproc_pending(s->coproc) <= 0)
        return;
    uint32_t obj = sci0_radio_obj(s);
    if (!obj || s->read8(s->ctx, obj + V1_STATE_OFF) != 1)
        return;
    int ind = cybiko_radio_coproc_try_async(s->coproc);
    if (ind >= 0) {
        cybiko_radio_coproc_prepare_rx(s->coproc, ind == CYBIKO_RADIO_IND_LARGE ? 200 : 50);
        s->rx_injected++;
    }
}

/*
 * Java ticks this once per CPU step. Here one call covers a batch of
 * cycles: the TXI countdown, one RDR load and the async check each happen
 * at most once per batch. A batch is eight instructions, short against
 * the guest's byte handling.
 */
void cybiko_sci0_advance(cybiko_sci0_v1_t *s, int cycles) {
    if (!s->bound || cycles <= 0)
        return;
    if (s->txi_delay > 0) {
        s->txi_delay -= cycles;
        if (s->txi_delay <= 0) {
            s->txi_delay = 0;
            s->tdre = true;
            if (s->scr & 0x80)
                sci0_raise_irq(s, CYBIKO_SCI0_TXI_VECTOR);
        }
    }
    if (!s->rdrf && cybiko_radio_coproc_rx_count(s->coproc) > 0) {
        s->rdr = (uint8_t)cybiko_radio_coproc_read(s->coproc);
        s->rdrf = true;
        if (s->scr & 0x40)
            sci0_raise_irq(s, CYBIKO_SCI0_RXI_VECTOR);
    }
    s->async_countdown -= cycles;
    if (s->async_countdown <= 0) {
        s->async_countdown = ASYNC_INTERVAL;
        sci0_async_delivery(s);
    }
}

void cybiko_sci0_frame(cybiko_sci0_v1_t *s) {
    if (!s->bound || !s->radio_id)
        return;
    /* Both ends usually boot the same flash.bin, so give this one its own
     * CyID. The boot loader uses that RAM before CyOS runs, so wait for
     * CyOS's radio object; CyOS may rewrite the cache, so keep it set. */
    if (!sci0_radio_obj(s))
        return;
    if (sci0_read32(s, V1_CYID_RAM) != s->radio_id)
        sci0_poke32(s, V1_CYID_RAM, s->radio_id);
}
