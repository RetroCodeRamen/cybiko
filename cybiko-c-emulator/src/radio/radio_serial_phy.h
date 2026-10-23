#ifndef CYBIKO_RADIO_SERIAL_PHY_H
#define CYBIKO_RADIO_SERIAL_PHY_H

/*
 * PHY over a USB serial LoRa modem (cybiko-heltec-modem, modem_proto.h).
 * POSIX only: the Linux emulator and host tools.
 *
 * tx() writes one 'T' frame and reports busy until the modem answers 'D',
 * so radio_lora paces itself on real airtime. poll() reads whatever the
 * serial port has without blocking.
 */

#include "radio_phy.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYBIKO_SERIAL_PHY_INBOX 8

typedef struct {
    uint32_t tx_sent, tx_timeout, tx_refused, tx_lost;  /* 'D' codes; lost = no 'D' */
    uint32_t rx_pkts, rx_overflow;
    int16_t last_rssi_dbm;
    int8_t last_snr_db;
} cybiko_serial_phy_stats_t;

typedef struct {
    cybiko_radio_phy_t phy;
    int fd;
    int tx_busy;
    uint64_t tx_started_ms;
    void *dec;   /* slip_decoder_t, kept out of this header */
    struct {
        uint8_t len;
        uint8_t pkt[255];
    } inbox[CYBIKO_SERIAL_PHY_INBOX];
    int in_head, in_count;
    cybiko_serial_phy_stats_t stats;
    char hello[96];   /* last 'H' text */
    char status[160]; /* last 'S' text */
    /* Called for 'H' and 'S' frames, e.g. to print them. May be NULL. */
    void (*on_text)(void *user, char type, const char *text);
    void *user;
} cybiko_serial_phy_t;

/* Opens and configures the port. 0 = ok, -1 = see errno. */
int cybiko_serial_phy_open(cybiko_serial_phy_t *s, const char *path);
void cybiko_serial_phy_close(cybiko_serial_phy_t *s);
/* Ask the modem for a status line; it arrives through on_text. */
void cybiko_serial_phy_request_status(cybiko_serial_phy_t *s);

#ifdef __cplusplus
}
#endif

#endif
