#ifndef CYBIKO_MODEM_PROTO_H
#define CYBIKO_MODEM_PROTO_H

/*
 * Serial protocol between the PC emulator (radio_serial_phy.c) and a
 * Heltec WiFi LoRa 32 V3 running cybiko-heltec-modem. The modem is a dumb
 * packet radio: it does not look inside packets.
 *
 * SLIP framing (RFC 1055), 921600 baud 8N1. Each frame is one type byte
 * then its body. Anything outside a frame (boot messages from the ESP32
 * ROM) is discarded by the decoder until the first END.
 *
 * PC -> modem
 *   'T' pkt...        transmit one packet (1..255 bytes)
 *   'S'               ask for a status frame (also a 2 s keepalive
 *                     while the emulator holds the port)
 * modem -> PC
 *   'H' text          hello at boot, ASCII
 *   'R' rssi_lo rssi_hi snr pkt...   received packet, rssi int16 dBm,
 *                     snr int8 dB
 *   'D' code          the last 'T' finished: 0 sent, 1 timed out,
 *                     2 refused (radio busy or packet too long)
 *   'S' text          status line, ASCII
 */

#include <stddef.h>
#include <stdint.h>

#define MODEM_BAUD      921600
#define MODEM_MAX_FRAME (1 + 3 + 255)

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

#define MODEM_TX_DONE_OK      0
#define MODEM_TX_DONE_TIMEOUT 1
#define MODEM_TX_DONE_REFUSED 2

/* Encode one frame into out (needs 2 * len + 2). Returns the length. */
static inline size_t slip_encode(const uint8_t *in, size_t len, uint8_t *out) {
    size_t n = 0;
    out[n++] = SLIP_END;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == SLIP_END) {
            out[n++] = SLIP_ESC;
            out[n++] = SLIP_ESC_END;
        } else if (in[i] == SLIP_ESC) {
            out[n++] = SLIP_ESC;
            out[n++] = SLIP_ESC_ESC;
        } else {
            out[n++] = in[i];
        }
    }
    out[n++] = SLIP_END;
    return n;
}

typedef struct {
    uint8_t buf[MODEM_MAX_FRAME];
    size_t len;
    uint8_t esc;
    uint8_t synced;   /* 0 until the first END: drops boot noise */
    uint8_t overrun;  /* current frame too long, dropped at END */
} slip_decoder_t;

/* Feed one byte. Returns the frame length when a frame completes (frame
 * is in d->buf), 0 otherwise. Empty frames are skipped. */
static inline size_t slip_feed(slip_decoder_t *d, uint8_t b) {
    if (b == SLIP_END) {
        size_t n = (d->synced && !d->overrun) ? d->len : 0;
        d->synced = 1;
        d->len = 0;
        d->esc = 0;
        d->overrun = 0;
        return n;
    }
    if (!d->synced)
        return 0;
    if (b == SLIP_ESC) {
        d->esc = 1;
        return 0;
    }
    if (d->esc) {
        b = b == SLIP_ESC_END ? SLIP_END : b == SLIP_ESC_ESC ? SLIP_ESC : b;
        d->esc = 0;
    }
    if (d->len < sizeof d->buf)
        d->buf[d->len++] = b;
    else
        d->overrun = 1;
    return 0;
}

#endif
