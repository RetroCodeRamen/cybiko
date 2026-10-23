#ifndef CYBIKO_SX1262_PHY_H
#define CYBIKO_SX1262_PHY_H

/*
 * SX1262 packet radio for the ESP32 firmwares (original T-Deck, Heltec
 * WiFi LoRa 32 V3). Arduino only; the host build does not compile it.
 *
 * Direct SPI, no RadioLib. LoRa explicit header, CRC on, continuous RX.
 * DIO3 drives the 1.8 V TCXO and DIO2 the RF switch on both boards.
 * tx() writes the buffer and keys the chip, then returns; TxDone and
 * RxDone are picked up by poll() when DIO1 goes high. Nothing here waits
 * for airtime.
 */

#include "radio_phy.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int8_t cs, rst, busy, dio1;
    /* Other chip selects on the same SPI bus, driven high before every
     * transfer. -1 = unused. */
    int8_t hold_high[2];
    void *spi;               /* SPIClass * */
    /* NULL = the bus is always ours. The T-Deck returns false while the
     * display DMA is running; the call is then retried on a later poll. */
    bool (*bus_free)(void);
} cybiko_sx1262_pins_t;

typedef struct {
    uint32_t freq_hz;
    uint32_t bw_hz;          /* 7800 .. 500000 */
    uint8_t  sf;             /* 5 .. 12 */
    uint8_t  cr_denom;       /* 5 .. 8, coding rate 4/x */
    int8_t   power_dbm;      /* -9 .. 22 */
    uint16_t sync_word;      /* register value, 0x1424 = private */
    uint16_t preamble;
} cybiko_lora_profile_t;

/* The compiled defaults from radio_transport.h. */
void cybiko_lora_profile_default(cybiko_lora_profile_t *p);

typedef struct {
    uint32_t tx_done;
    uint32_t tx_timeout;
    uint32_t rx_ok;
    uint32_t rx_crc;         /* LoRa CRC or header error: dropped */
    uint32_t rx_overflow;    /* inbox full: dropped */
    uint32_t bus_waits;      /* poll/tx skipped because the bus was busy */
    int16_t  last_rssi_dbm;
    int8_t   last_snr_db;
    uint8_t  status;         /* last GetStatus byte */
    uint16_t device_errors;
} cybiko_sx1262_stats_t;

/* 0 = configured and listening. Negative = setup failed; see last_error. */
int cybiko_sx1262_phy_begin(const cybiko_sx1262_pins_t *pins,
                            const cybiko_lora_profile_t *profile);
cybiko_radio_phy_t *cybiko_sx1262_phy(void);
const cybiko_sx1262_stats_t *cybiko_sx1262_phy_stats(void);
const char *cybiko_sx1262_phy_error(void);

/* GetStatus over SPI: the status byte (chip mode in bits 6:4, 5 = RX), or
 * -1 if the bus is busy or the chip did not answer. A health check. */
int cybiko_sx1262_phy_probe(void);

/* Rough LoRa airtime in ms for a packet of `len` bytes, explicit header
 * and CRC on, for logging and duty-cycle budgeting. */
uint32_t cybiko_lora_airtime_ms(const cybiko_lora_profile_t *p, unsigned len);

#ifdef __cplusplus
}
#endif

#endif
