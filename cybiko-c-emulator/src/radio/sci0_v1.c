#include "sci0_v1.h"

#include <string.h>

/* CyOS 1.2.46 radio object layout (AddressBus.java) */
#define V1_RADIO_OBJ_PTR 0x21F216u
#define V1_STATE_OFF     0x3370u
#define V1_CONN_OFF      0x3344u
#define V1_POOL_OFF      0x32C8u
#define V1_TXPTR_OFF     0x3350u
#define V1_TXEND_OFF     0x3354u
#define V1_CMD_OFF       0x336Eu
#define V1_PARAM_OFF     0x336Fu
#define V1_CHANNEL_OFF   0x3371u
#define V1_CYID_RAM      0x21F9DCu
#define V1_RF_OBJECT     0x223186u
#define V1_TASK_STATE    0x14u

#define RAM_LO 0x200000u
#define RAM_HI 0x280000u

/* Java counts these in CPU steps at one instruction per cycle */
#define TXI_DELAY            32
#define ASYNC_INTERVAL       512
#define BOOTSTRAP_INITIAL    2000000
#define BOOTSTRAP_RETRY      500000
/* Java polls every 15 frames. Each poll also sends a beacon, and every
 * reply is run through the boot ROM's error correction (~650 guest
 * instructions a byte): at 15 the Classic never idled at stride 12, and a
 * 4 Hz beacon would fill a LoRa channel by itself. */
#ifndef CYBIKO_V1_POLL_FRAMES
#define CYBIKO_V1_POLL_FRAMES 60
#endif
#define POLL_INTERVAL_FRAMES CYBIKO_V1_POLL_FRAMES

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
    return sci0_in_ram(obj) && sci0_in_ram(obj + V1_CHANNEL_OFF) ? obj : 0;
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
        s->driver_tx = false;
        if (s->coproc->v1_accum)
            s->tx_frames++;
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

static void sci0_bootstrap(cybiko_sci0_v1_t *s, int cycles) {
    if (s->bootstrapped || !s->coproc->initialized)
        return;
    s->bootstrap_wait += cycles;
    if (s->bootstrap_wait < (s->bootstrap_pending ? BOOTSTRAP_RETRY : BOOTSTRAP_INITIAL))
        return;
    s->bootstrap_wait = 0;
    uint32_t obj = sci0_radio_obj(s);
    if (!obj)
        return;
    uint32_t conn = sci0_read32(s, obj + V1_CONN_OFF);
    if (conn) {
        s->bootstrapped = true;
        s->saved_conn = conn;
        return;
    }
    if (sci0_read32(s, obj + V1_POOL_OFF) == 0)
        return;   /* allocator not ready yet */
    if (!s->bootstrap_pending) {
        /* State 9 plus an ACK runs the completion handler, which
         * allocates connObj */
        s->poke8(s->ctx, CYBIKO_SCI0_SCR, 0x70);
        sci0_write_scr(s, 0x70);
        s->poke8(s->ctx, obj + V1_STATE_OFF, 9);
        cybiko_radio_coproc_queue_response(s->coproc, CYBIKO_RADIO_ACK);
        s->bootstrap_pending = true;
    } else {
        s->bootstrap_pending = false;   /* try again */
    }
}

static void sci0_async_delivery(cybiko_sci0_v1_t *s) {
    if (!s->bootstrapped || s->rdrf || cybiko_radio_coproc_rx_count(s->coproc) > 0 ||
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
 * cycles: the TXI countdown, one RDR load, the async check and the
 * bootstrap wait each happen at most once per batch. A batch is eight
 * instructions, short against the guest's byte handling.
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
    sci0_bootstrap(s, cycles);
    s->async_countdown -= cycles;
    if (s->async_countdown <= 0) {
        s->async_countdown = ASYNC_INTERVAL;
        sci0_async_delivery(s);
    }
}

void cybiko_sci0_frame(cybiko_sci0_v1_t *s) {
    if (!s->bound || !s->bootstrapped)
        return;
    if (++s->poll_frames < POLL_INTERVAL_FRAMES)
        return;
    s->poll_frames = 0;

    uint32_t obj = sci0_radio_obj(s);
    if (!obj || s->read8(s->ctx, obj + V1_STATE_OFF) != 1)
        return;   /* only start a cycle when CyOS's radio is idle */

    uint32_t conn = sci0_read32(s, obj + V1_CONN_OFF);
    if (conn == 0 && s->saved_conn) {
        sci0_poke32(s, obj + V1_CONN_OFF, s->saved_conn);   /* completion NULLed it */
        conn = s->saved_conn;
    } else if (conn && !s->saved_conn) {
        s->saved_conn = conn;
    }
    if (!sci0_in_ram(conn) || !sci0_in_ram(conn + 0xD4))
        return;

    if (s->radio_id) {
        if (!s->cyid_patched) {
            sci0_poke32(s, V1_CYID_RAM, s->radio_id);
            s->cyid_patched = true;
        }
        /* Beacon header CyOS never fills in emulation */
        sci0_poke32(s, conn + 4, s->radio_id);
        s->poke8(s->ctx, conn + 9, 0x01);    /* frame type: beacon */
        s->poke8(s->ctx, conn + 16, 0x2A);   /* payload length marker */
        if (s->read8(s->ctx, conn + 20) == 0) {
            static const char name[8] = "Cybiko";
            for (int i = 0; i < 8; i++)
                s->poke8(s->ctx, conn + 20 + (uint32_t)i, (uint8_t)name[i]);
        }
    }

    sci0_poke32(s, obj + V1_TXPTR_OFF, conn);   /* completion clears it */

    /* conn+0xD2 set: poll (0x30, 50 bytes); otherwise four polls to a scan */
    int cmd, size;
    if (s->read8(s->ctx, conn + 0xD2) != 0 || s->poll_alt < 4) {
        cmd = 0x30; size = 50;
        if (s->read8(s->ctx, conn + 0xD2) == 0)
            s->poll_alt++;
    } else {
        cmd = 0xCF; size = 200;
        s->poll_alt = 0;
    }
    uint8_t d4 = s->read8(s->ctx, conn + 0xD4);
    uint8_t ch = d4 ? d4 : s->read8(s->ctx, obj + V1_CHANNEL_OFF);
    s->poke8(s->ctx, conn + 8, (uint8_t)(0xC0 | (ch & 0x3F)));
    s->poke8(s->ctx, obj + V1_CMD_OFF, (uint8_t)cmd);
    s->poke8(s->ctx, obj + V1_PARAM_OFF, (uint8_t)((s->read8(s->ctx, conn + 9) >> 3) & 1));
    sci0_poke32(s, obj + V1_TXEND_OFF, conn + (uint32_t)size);
    s->poke8(s->ctx, obj + V1_STATE_OFF, 7);   /* TXI0 sends the command */
    s->driver_tx = true;

    s->scr |= 0x80;   /* TIE: start the TXI0 chain */
    s->poke8(s->ctx, CYBIKO_SCI0_SCR, s->scr);
    if (s->tdre && s->txi_delay == 0)
        s->txi_delay = TXI_DELAY;
}

void cybiko_sci0_on_set_task_state(cybiko_sci0_v1_t *s) {
    if (!s->bound || !s->rf_armed || s->rf_resolved)
        return;
    if (s->read8(s->ctx, V1_RF_OBJECT + V1_TASK_STATE) != 0x01) {
        s->poke8(s->ctx, V1_RF_OBJECT + V1_TASK_STATE, 0x01);
        s->rf_resolved = true;
    }
}
