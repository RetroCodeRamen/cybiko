/*
 * Heltec WiFi LoRa 32 V3 as a packet modem for the PC Cybiko emulator.
 * USB serial (CP2102, UART0) carries SLIP frames, see modem_proto.h; the
 * SX1262 is driven by the same sx1262_phy as the T-Deck, so both ends
 * use one radio profile.
 */
#include <Arduino.h>
#include <SPI.h>

#include "modem_proto.h"
#include "radio_link.h"
#include "sx1262_phy.h"

#define PIN_LORA_CS   8
#define PIN_LORA_SCK  9
#define PIN_LORA_MOSI 10
#define PIN_LORA_MISO 11
#define PIN_LORA_RST  12
#define PIN_LORA_BUSY 13
#define PIN_LORA_DIO1 14
#define PIN_LED       35

static cybiko_radio_phy_t *phy;
static cybiko_lora_profile_t prof;
static slip_decoder_t dec;
static bool tx_pending;
static uint32_t led_off_ms;

static void send_frame(const uint8_t *body, size_t len) {
    uint8_t out[2 * MODEM_MAX_FRAME + 2];
    Serial.write(out, slip_encode(body, len, out));
}

static void send_text(char type, const char *text) {
    uint8_t body[128];
    size_t n = strnlen(text, sizeof body - 1);
    body[0] = (uint8_t)type;
    memcpy(body + 1, text, n);
    send_frame(body, n + 1);
}

static void send_done(uint8_t code) {
    uint8_t body[2] = { 'D', code };
    send_frame(body, 2);
}

static void blink(void) {
    digitalWrite(PIN_LED, HIGH);
    led_off_ms = millis() + 30;
}

static void send_status(void) {
    cybiko_sx1262_phy_probe();  /* refresh the status byte */
    const cybiko_sx1262_stats_t *s = cybiko_sx1262_phy_stats();
    char line[128];
    snprintf(line, sizeof line,
             "%s %lu.%03lu MHz SF%u BW%lu %d dBm st=%02X err=%04X "
             "rx %lu crc %lu ovf %lu tx %lu tmo %lu rssi %d snr %d",
             cybiko_sx1262_phy_error(),
             (unsigned long)(prof.freq_hz / 1000000), (unsigned long)(prof.freq_hz / 1000 % 1000),
             prof.sf, (unsigned long)(prof.bw_hz / 1000), prof.power_dbm,
             s->status, s->device_errors,
             (unsigned long)s->rx_ok, (unsigned long)s->rx_crc, (unsigned long)s->rx_overflow,
             (unsigned long)s->tx_done, (unsigned long)s->tx_timeout,
             s->last_rssi_dbm, s->last_snr_db);
    send_text('S', line);
}

static void handle_frame(const uint8_t *f, size_t len) {
    switch (f[0]) {
    case 'T':
        if (!phy || tx_pending || len < 2 || phy->tx(phy, f + 1, len - 1) != 0) {
            send_done(MODEM_TX_DONE_REFUSED);
            return;
        }
        tx_pending = true;
        blink();
        break;
    case 'S':
        send_status();
        break;
    }
}

void setup() {
    Serial.setRxBufferSize(4096);
    Serial.begin(MODEM_BAUD);
    pinMode(PIN_LED, OUTPUT);
    SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

    cybiko_sx1262_pins_t pins = {};
    pins.cs = PIN_LORA_CS;
    pins.rst = PIN_LORA_RST;
    pins.busy = PIN_LORA_BUSY;
    pins.dio1 = PIN_LORA_DIO1;
    pins.hold_high[0] = -1;
    pins.hold_high[1] = -1;
    pins.spi = &SPI;
    pins.bus_free = NULL;
    cybiko_lora_profile_default(&prof);
    int rc = cybiko_sx1262_phy_begin(&pins, &prof);
    phy = rc == 0 ? cybiko_sx1262_phy() : NULL;

    char hello[96];
    snprintf(hello, sizeof hello, "cybiko-heltec-modem begin %d (%s)", rc,
             cybiko_sx1262_phy_error());
    send_text('H', hello);
    send_status();
}

void loop() {
    while (Serial.available()) {
        size_t n = slip_feed(&dec, (uint8_t)Serial.read());
        if (n)
            handle_frame(dec.buf, n);
    }
    if (!phy)
        return;

    phy->poll(phy);
    if (tx_pending && !phy->tx_busy(phy)) {
        const cybiko_sx1262_stats_t *s = cybiko_sx1262_phy_stats();
        static uint32_t seen_timeout;
        uint8_t code = s->tx_timeout != seen_timeout ? MODEM_TX_DONE_TIMEOUT : MODEM_TX_DONE_OK;
        seen_timeout = s->tx_timeout;
        tx_pending = false;
        send_done(code);
    }

    uint8_t pkt[CYBIKO_LINK_MAX_PHYS];
    size_t len;
    while (phy->rx(phy, pkt, sizeof pkt, &len)) {
        const cybiko_sx1262_stats_t *s = cybiko_sx1262_phy_stats();
        uint8_t body[4 + CYBIKO_LINK_MAX_PHYS];
        body[0] = 'R';
        body[1] = (uint8_t)s->last_rssi_dbm;
        body[2] = (uint8_t)((uint16_t)s->last_rssi_dbm >> 8);
        body[3] = (uint8_t)s->last_snr_db;
        memcpy(body + 4, pkt, len);
        send_frame(body, 4 + len);
        blink();
    }

    if (led_off_ms && (int32_t)(millis() - led_off_ms) >= 0) {
        digitalWrite(PIN_LED, LOW);
        led_off_ms = 0;
    }
}
