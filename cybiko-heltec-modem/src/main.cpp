/*
 * Heltec WiFi LoRa 32 V3 as a packet modem for the PC Cybiko emulator.
 * USB serial (CP2102, UART0) carries SLIP frames, see modem_proto.h; the
 * SX1262 is driven by the same sx1262_phy as the T-Deck, so both ends
 * use one radio profile.
 */
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>

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
#define PIN_OLED_SDA  17
#define PIN_OLED_SCL  18
#define PIN_OLED_RST  21
#define PIN_VEXT      36

static cybiko_radio_phy_t *phy;
static cybiko_lora_profile_t prof;
static slip_decoder_t dec;
static bool tx_pending;
static uint32_t led_off_ms;
static uint32_t oled_at;
static uint32_t oled_pulse_ms;
static uint32_t oled_drawn;
static uint32_t oled_rx, oled_tx;
static bool oled_linked;
static bool oled_pulse_on;
static uint32_t host_at;
static bool oled_ok;

/* PC sends 'S' about every 2 s while v1_emu is attached. */
#define HOST_LINK_MS 4000

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, PIN_OLED_RST);

static bool host_linked(void) {
    return host_at && (int32_t)(millis() - host_at) < HOST_LINK_MS;
}

static void note_host(void) {
    bool was = host_linked();
    host_at = millis();
    if (!was)
        oled_at = 0;
}

static void oled_draw(void) {
    const cybiko_sx1262_stats_t *s = phy ? cybiko_sx1262_phy_stats() : NULL;
    char line[24];
    const char *st;
    bool linked = host_linked();
    bool pulse = oled_pulse_ms && (int32_t)(millis() - oled_pulse_ms) < 0;

    if (!oled_ok) return;
    if (!phy)
        st = "FAIL";
    else if (linked)
        st = "EMU ON";
    else
        st = "NO EMU";

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 12, "ReBiko Radio");   /* 72 px */
    u8g2.drawStr(128 - u8g2.getStrWidth(st), 12, st);
    if (pulse)
        u8g2.drawBox(82, 3, 8, 8);
    if (linked)
        u8g2.drawBox(76, 4, 4, 6);

    snprintf(line, sizeof line, "%lu.%03lu MHz SF%u",
             (unsigned long)(prof.freq_hz / 1000000),
             (unsigned long)(prof.freq_hz / 1000 % 1000),
             prof.sf);
    u8g2.drawStr(0, 28, line);

    if (s) {
        snprintf(line, sizeof line, "RX %lu  TX %lu",
                 (unsigned long)s->rx_ok, (unsigned long)s->tx_done);
        u8g2.drawStr(0, 44, line);
        if (!linked)
            snprintf(line, sizeof line, "waiting for PC");
        else if (s->rx_ok == 0 && s->tx_done == 0)
            snprintf(line, sizeof line, "listening  %ddBm", prof.power_dbm);
        else
            snprintf(line, sizeof line, "%d dBm  SNR %d",
                     s->last_rssi_dbm, s->last_snr_db);
        u8g2.drawStr(0, 60, line);
    } else {
        const char *err = cybiko_sx1262_phy_error();
        u8g2.drawStr(0, 44, err && err[0] ? err : "no SX1262");
        u8g2.drawStr(0, 60, "radio down");
    }
    u8g2.sendBuffer();
}

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

static bool oled_needs_draw(void) {
    const cybiko_sx1262_stats_t *s = phy ? cybiko_sx1262_phy_stats() : NULL;
    uint32_t rx = s ? s->rx_ok : 0, tx = s ? s->tx_done : 0;
    bool linked = host_linked();
    bool pulse = oled_pulse_ms && (int32_t)(millis() - oled_pulse_ms) < 0;
    bool dirty = rx != oled_rx || tx != oled_tx || linked != oled_linked ||
                 pulse != oled_pulse_on;
    oled_rx = rx;
    oled_tx = tx;
    oled_linked = linked;
    oled_pulse_on = pulse;
    return dirty || (int32_t)(millis() - oled_drawn) >= 1000;
}

static void blink(void) {
    digitalWrite(PIN_LED, HIGH);
    led_off_ms = millis() + 30;
    oled_pulse_ms = millis() + 250;
    oled_at = 0;
}

static void send_status(void) {
    cybiko_sx1262_phy_probe();  /* refresh the status byte */
    const cybiko_sx1262_stats_t *s = cybiko_sx1262_phy_stats();
    char line[128];
    snprintf(line, sizeof line,
             "%s %lu.%03lu MHz SF%u BW%lu %d dBm st=%02X err=%04X "
             "rx %lu crc %lu ovf %lu tx %lu tmo %lu wait %lu rssi %d snr %d",
             cybiko_sx1262_phy_error(),
             (unsigned long)(prof.freq_hz / 1000000), (unsigned long)(prof.freq_hz / 1000 % 1000),
             prof.sf, (unsigned long)(prof.bw_hz / 1000), prof.power_dbm,
             s->status, s->device_errors,
             (unsigned long)s->rx_ok, (unsigned long)s->rx_crc, (unsigned long)s->rx_overflow,
             (unsigned long)s->tx_done, (unsigned long)s->tx_timeout,
             (unsigned long)s->tx_deferred, s->last_rssi_dbm, s->last_snr_db);
    send_text('S', line);
}

static void handle_frame(const uint8_t *f, size_t len) {
    note_host();
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
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(50);
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    Wire.setClock(400000);
    oled_ok = u8g2.begin();
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
    snprintf(hello, sizeof hello, "ReBiko Radio begin %d (%s)", rc,
             cybiko_sx1262_phy_error());
    send_text('H', hello);
    send_status();
    oled_draw();
    oled_drawn = millis();
}

void loop() {
    bool work = false;
    while (Serial.available()) {
        size_t n = slip_feed(&dec, (uint8_t)Serial.read());
        if (n)
            handle_frame(dec.buf, n);
        work = true;
    }
    if (phy) {
    phy->poll(phy);
    if (tx_pending && !phy->tx_busy(phy)) {
        const cybiko_sx1262_stats_t *s = cybiko_sx1262_phy_stats();
        static uint32_t seen_timeout;
        uint8_t code = s->tx_timeout != seen_timeout ? MODEM_TX_DONE_TIMEOUT : MODEM_TX_DONE_OK;
        seen_timeout = s->tx_timeout;
        tx_pending = false;
        send_done(code);
        work = true;
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
        work = true;
    }
    }

    if (led_off_ms && (int32_t)(millis() - led_off_ms) >= 0) {
        digitalWrite(PIN_LED, LOW);
        led_off_ms = 0;
    }

    bool irq = phy && digitalRead(PIN_LORA_DIO1) == HIGH;
    if (!irq && (int32_t)(millis() - oled_at) >= 0) {
        if (oled_needs_draw()) {
            oled_draw();
            oled_drawn = millis();
        }
        oled_at = millis() + 250;
    }

    /* Idle: yield so we are not a 240 MHz spin loop. IRQ or serial work
     * stays hot; DIO1 high never waits. */
    if (!work && !irq && !tx_pending)
        delay(1);
}
