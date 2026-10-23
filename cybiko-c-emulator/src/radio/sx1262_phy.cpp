/*
 * SX1262 packet radio. See sx1262_phy.h. Command values are from the
 * Semtech SX1261/2 datasheet (rev 2.1); register workarounds are from its
 * section 15.
 */
#include "sx1262_phy.h"
#include "radio_link.h"
#include "radio_transport.h"

#include <Arduino.h>
#include <SPI.h>
#include <string.h>

#define CMD_SET_STANDBY          0x80
#define CMD_SET_RX               0x82
#define CMD_SET_TX               0x83
#define CMD_SET_RF_FREQUENCY     0x86
#define CMD_CALIBRATE            0x89
#define CMD_SET_PACKET_TYPE      0x8A
#define CMD_SET_MOD_PARAMS       0x8B
#define CMD_SET_PACKET_PARAMS    0x8C
#define CMD_SET_TX_PARAMS        0x8E
#define CMD_SET_BUFFER_BASE      0x8F
#define CMD_SET_PA_CONFIG        0x95
#define CMD_SET_REGULATOR        0x96
#define CMD_SET_DIO3_TCXO        0x97
#define CMD_CALIBRATE_IMAGE      0x98
#define CMD_SET_DIO2_RF_SWITCH   0x9D
#define CMD_SET_DIO_IRQ          0x08
#define CMD_CLEAR_IRQ            0x02
#define CMD_CLEAR_DEVICE_ERRORS  0x07
#define CMD_WRITE_REGISTER       0x0D
#define CMD_WRITE_BUFFER         0x0E
#define CMD_GET_IRQ_STATUS       0x12
#define CMD_GET_RX_BUFFER_STATUS 0x13
#define CMD_GET_PACKET_STATUS    0x14
#define CMD_GET_DEVICE_ERRORS    0x17
#define CMD_READ_REGISTER        0x1D
#define CMD_READ_BUFFER          0x1E
#define CMD_GET_STATUS           0xC0

#define IRQ_TX_DONE    0x0001
#define IRQ_RX_DONE    0x0002
#define IRQ_PREAMBLE   0x0004
#define IRQ_HEADER_OK  0x0010
#define IRQ_HEADER_ERR 0x0020
#define IRQ_CRC_ERR    0x0040
#define IRQ_TIMEOUT    0x0200
#define IRQ_USED (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT)
/* Latched but not on DIO1: someone else's packet is arriving */
#define IRQ_CARRIER (IRQ_PREAMBLE | IRQ_HEADER_OK)

#define REG_SYNC_WORD   0x0740
#define REG_IQ_POLARITY 0x0736
#define REG_TX_CLAMP    0x08D8
#define REG_OCP         0x08E7

#define STDBY_RC    0x00
#define TCXO_1V8    0x02
#define SPI_HZ      8000000

#define BUSY_INIT_MS 250
#define BUSY_RUN_MS  10
/* SetTx safety timeout, 15.625 us steps: 4 s. Longer than any packet at
 * the default profile; at SF12 a 255-byte packet is ~10 s, so tx() refuses
 * profiles where the packet would not fit. */
#define TX_TIMEOUT_TICKS 256000u
#define TX_TIMEOUT_MS    4000u

#define INBOX 4

/* Listen before talk. A send waits a random 0..TX_JITTER_MS so two radios
 * that decide together rarely start together, then waits while a packet
 * is arriving (preamble or header seen): one longest packet plus a random
 * part of another, then looks again. A carrier flag that never clears
 * (a preamble with no packet) stops blocking after LBT_GIVE_UP_MS. */
#define TX_JITTER_MS   30
#define LBT_GIVE_UP_MS 2000

static cybiko_sx1262_pins_t pins;
static cybiko_lora_profile_t prof;
static cybiko_sx1262_stats_t stats;
static cybiko_radio_phy_t phy;
static char last_error[40] = "not started";
static bool started;
static bool tx_inflight;
static uint32_t tx_started_ms;
static uint8_t tx_buf[CYBIKO_LINK_MAX_PHYS];
static uint8_t tx_len;
static bool tx_waiting;            /* accepted, not on the air yet */
static uint32_t tx_wait_since, tx_not_before;
static uint32_t busy_ms = BUSY_INIT_MS;

static struct {
    uint8_t pkt[CYBIKO_LINK_MAX_PHYS];
    uint8_t len;
} inbox[INBOX];
static int in_head, in_count;

static SPIClass *bus(void) { return (SPIClass *)pins.spi; }

static void set_error(const char *msg) {
    strncpy(last_error, msg, sizeof last_error - 1);
    last_error[sizeof last_error - 1] = 0;
}

static bool bus_ok(void) {
    return !pins.bus_free || pins.bus_free();
}

static bool wait_busy(void) {
    uint32_t start = millis();
    while (digitalRead(pins.busy) == HIGH) {
        if (millis() - start >= busy_ms)
            return false;
        delayMicroseconds(20);
    }
    return true;
}

/* One command: out[0] is the opcode. in[] receives the bytes clocked back
 * after the out bytes. */
static bool xfer(const uint8_t *out, size_t out_len, uint8_t *in, size_t in_len) {
    for (int i = 0; i < 2; i++)
        if (pins.hold_high[i] >= 0)
            digitalWrite(pins.hold_high[i], HIGH);
    if (!wait_busy()) {
        set_error("busy timeout");
        return false;
    }
    bus()->beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(pins.cs, LOW);
    for (size_t i = 0; i < out_len; i++)
        bus()->transfer(out[i]);
    for (size_t i = 0; i < in_len; i++)
        in[i] = bus()->transfer(0x00);
    digitalWrite(pins.cs, HIGH);
    bus()->endTransaction();
    return true;
}

static bool cmd(uint8_t op, const uint8_t *args, size_t n) {
    uint8_t buf[10];
    if (n + 1 > sizeof buf)
        return false;
    buf[0] = op;
    if (n)
        memcpy(buf + 1, args, n);
    return xfer(buf, n + 1, NULL, 0);
}

static bool cmd1(uint8_t op, uint8_t a) { return cmd(op, &a, 1); }

/* Read commands return a status byte first, then the data. */
static bool get(uint8_t op, uint8_t *out, size_t n) {
    uint8_t raw[4];
    if (n + 1 > sizeof raw)
        return false;
    if (!xfer(&op, 1, raw, n + 1))
        return false;
    stats.status = raw[0];
    memcpy(out, raw + 1, n);
    return true;
}

static bool write_reg(uint16_t addr, const uint8_t *data, size_t n) {
    uint8_t buf[8];
    if (n + 3 > sizeof buf)
        return false;
    buf[0] = CMD_WRITE_REGISTER;
    buf[1] = (uint8_t)(addr >> 8);
    buf[2] = (uint8_t)addr;
    memcpy(buf + 3, data, n);
    return xfer(buf, n + 3, NULL, 0);
}

static bool read_reg(uint16_t addr, uint8_t *out) {
    uint8_t buf[4] = { CMD_READ_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr, 0 };
    return xfer(buf, 4, out, 1);
}

static bool clear_irq(uint16_t mask) {
    uint8_t a[2] = { (uint8_t)(mask >> 8), (uint8_t)mask };
    return cmd(CMD_CLEAR_IRQ, a, 2);
}

static bool get_irq(uint16_t *irq) {
    uint8_t v[2];
    if (!get(CMD_GET_IRQ_STATUS, v, 2))
        return false;
    *irq = (uint16_t)((v[0] << 8) | v[1]);
    return true;
}

static bool packet_params(uint8_t payload_len) {
    uint8_t a[6] = {
        (uint8_t)(prof.preamble >> 8), (uint8_t)prof.preamble,
        0x00,          /* explicit header */
        payload_len,   /* RX: maximum accepted */
        0x01,          /* CRC on */
        0x00,          /* standard IQ */
    };
    return cmd(CMD_SET_PACKET_PARAMS, a, sizeof a);
}

static bool start_rx(void) {
    static const uint8_t continuous[3] = { 0xFF, 0xFF, 0xFF };
    return packet_params(CYBIKO_LINK_MAX_PHYS) && cmd(CMD_SET_RX, continuous, 3);
}

static uint8_t bw_code(uint32_t bw) {
    if (bw <= 7800) return 0x00;
    if (bw <= 10400) return 0x08;
    if (bw <= 15600) return 0x01;
    if (bw <= 20800) return 0x09;
    if (bw <= 31250) return 0x02;
    if (bw <= 41700) return 0x0A;
    if (bw <= 62500) return 0x03;
    if (bw <= 125000) return 0x04;
    if (bw <= 250000) return 0x05;
    return 0x06;
}

static bool needs_ldro(const cybiko_lora_profile_t *p) {
    /* Low data-rate optimize above 16 ms per symbol */
    return ((1000000ull << p->sf) / p->bw_hz) > 16000;
}

uint32_t cybiko_lora_airtime_ms(const cybiko_lora_profile_t *p, unsigned len) {
    double tsym = (double)(1u << p->sf) / (double)p->bw_hz;
    int de = needs_ldro(p) ? 1 : 0;
    double num = 8.0 * len - 4.0 * p->sf + 28 + 16;
    double den = 4.0 * (p->sf - 2 * de);
    double blocks = num > 0 ? (double)(long)((num + den - 1) / den) : 0;
    double nsym = (p->preamble + 4.25) + 8 + blocks * p->cr_denom;
    return (uint32_t)(nsym * tsym * 1000.0 + 0.5);
}

void cybiko_lora_profile_default(cybiko_lora_profile_t *p) {
    p->freq_hz = CYBIKO_LORA_FREQ_HZ;
    p->bw_hz = CYBIKO_LORA_BW_HZ;
    p->sf = CYBIKO_LORA_SF;
    p->cr_denom = CYBIKO_LORA_CR_DENOM;
    p->power_dbm = CYBIKO_LORA_POWER_DBM;
    p->sync_word = CYBIKO_LORA_SYNC_WORD;
    p->preamble = CYBIKO_LORA_PREAMBLE;
}

/* ---- PHY ops ---- */

static void start_tx(void) {
    uint8_t hdr[2] = { CMD_WRITE_BUFFER, 0x00 };
    uint8_t buf[2 + CYBIKO_LINK_MAX_PHYS];
    memcpy(buf, hdr, 2);
    memcpy(buf + 2, tx_buf, tx_len);
    static const uint8_t timeout[3] = {
        (uint8_t)(TX_TIMEOUT_TICKS >> 16), (uint8_t)(TX_TIMEOUT_TICKS >> 8),
        (uint8_t)TX_TIMEOUT_TICKS };
    tx_waiting = false;
    if (!cmd1(CMD_SET_STANDBY, STDBY_RC) ||
        !xfer(buf, 2 + (size_t)tx_len, NULL, 0) ||
        !packet_params(tx_len) ||
        !clear_irq(0xFFFF) ||
        !cmd(CMD_SET_TX, timeout, 3)) {
        stats.tx_timeout++;   /* counted as lost; the link has no retry */
        start_rx();
        return;
    }
    tx_inflight = true;
    tx_started_ms = millis();
}

/* Start the waiting packet if its jitter is over and the air is free */
static void try_tx(void) {
    if (!tx_waiting || tx_inflight)
        return;
    uint32_t now = millis();
    if ((int32_t)(now - tx_not_before) < 0)
        return;
    if (!bus_ok()) {
        stats.bus_waits++;
        return;
    }
    uint16_t irq = 0;
    if (!get_irq(&irq))
        return;
    if ((irq & IRQ_CARRIER) && !(irq & IRQ_RX_DONE) &&
        now - tx_wait_since < LBT_GIVE_UP_MS) {
        uint32_t longest = cybiko_lora_airtime_ms(&prof, CYBIKO_LINK_MAX_PHYS);
        clear_irq(IRQ_CARRIER);
        tx_not_before = now + longest + (uint32_t)random((long)longest + 1);
        stats.tx_deferred++;
        return;
    }
    start_tx();
}

static int phy_tx(cybiko_radio_phy_t *self, const uint8_t *pkt, size_t len) {
    (void)self;
    if (!started || tx_inflight || tx_waiting || len == 0 || len > CYBIKO_LINK_MAX_PHYS)
        return -1;
    if (cybiko_lora_airtime_ms(&prof, (unsigned)len) >= TX_TIMEOUT_MS)
        return -1;
    memcpy(tx_buf, pkt, len);
    tx_len = (uint8_t)len;
    tx_waiting = true;
    tx_wait_since = millis();
    tx_not_before = tx_wait_since + (uint32_t)random(TX_JITTER_MS + 1);
    return 0;
}

static int phy_tx_busy(cybiko_radio_phy_t *self) {
    (void)self;
    return tx_inflight || tx_waiting;
}

static int phy_rx(cybiko_radio_phy_t *self, uint8_t *buf, size_t cap, size_t *len) {
    (void)self;
    if (in_count == 0)
        return 0;
    size_t n = inbox[in_head].len;
    if (n > cap)
        n = cap;
    memcpy(buf, inbox[in_head].pkt, n);
    *len = n;
    in_head = (in_head + 1) % INBOX;
    in_count--;
    return 1;
}

static void read_packet(void) {
    uint8_t st[2];
    if (!get(CMD_GET_RX_BUFFER_STATUS, st, 2))
        return;
    uint8_t len = st[0], start = st[1];
    uint8_t ps[3];
    if (get(CMD_GET_PACKET_STATUS, ps, 3)) {
        stats.last_rssi_dbm = -(int16_t)ps[0] / 2;
        stats.last_snr_db = (int8_t)((int8_t)ps[1] / 4);
    }
    if (len == 0)
        return;
    if (in_count == INBOX) {
        stats.rx_overflow++;
        return;
    }
    int slot = (in_head + in_count) % INBOX;
    uint8_t op[3] = { CMD_READ_BUFFER, start, 0x00 };  /* 0x00 = status NOP */
    if (!xfer(op, 3, inbox[slot].pkt, len))
        return;
    inbox[slot].len = len;
    in_count++;
    stats.rx_ok++;
}

static void phy_poll(cybiko_radio_phy_t *self) {
    (void)self;
    if (!started)
        return;
    bool irq_line = digitalRead(pins.dio1) == HIGH;
    bool overdue = tx_inflight && millis() - tx_started_ms > TX_TIMEOUT_MS + 500;
    if (!irq_line && !overdue) {
        try_tx();
        return;
    }
    if (!bus_ok()) {
        stats.bus_waits++;
        return;
    }
    uint16_t irq = 0;
    if (!get_irq(&irq))
        return;
    clear_irq(irq ? irq : 0xFFFF);
    if (irq & IRQ_RX_DONE) {
        if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR))
            stats.rx_crc++;
        else
            read_packet();
    } else if (irq & IRQ_HEADER_ERR) {
        stats.rx_crc++;
    }
    if (tx_inflight && (irq & (IRQ_TX_DONE | IRQ_TIMEOUT) || overdue)) {
        if (irq & IRQ_TX_DONE)
            stats.tx_done++;
        else
            stats.tx_timeout++;
        tx_inflight = false;
        start_rx();
    }
    try_tx();
}

/* ---- setup ---- */

static bool calibrate_image(uint32_t f) {
    uint8_t a[2];
    if (f >= 902000000u)      { a[0] = 0xE1; a[1] = 0xE9; }
    else if (f >= 863000000u) { a[0] = 0xD7; a[1] = 0xDB; }
    else if (f >= 779000000u) { a[0] = 0xC1; a[1] = 0xC5; }
    else if (f >= 470000000u) { a[0] = 0x75; a[1] = 0x81; }
    else                      { a[0] = 0x6B; a[1] = 0x6F; }
    return cmd(CMD_CALIBRATE_IMAGE, a, 2);
}

#define STEP(ok, msg, rc) do { if (!(ok)) { set_error(msg); return (rc); } } while (0)

int cybiko_sx1262_phy_begin(const cybiko_sx1262_pins_t *p,
                            const cybiko_lora_profile_t *profile) {
    pins = *p;
    prof = *profile;
    memset(&stats, 0, sizeof stats);
    stats.device_errors = 0xFFFF;
    started = false;
    tx_inflight = false;
    tx_waiting = false;
    in_head = in_count = 0;
    busy_ms = BUSY_INIT_MS;

    pinMode(pins.cs, OUTPUT);
    digitalWrite(pins.cs, HIGH);
    pinMode(pins.busy, INPUT);
    pinMode(pins.dio1, INPUT);
    for (int i = 0; i < 2; i++) {
        if (pins.hold_high[i] >= 0) {
            pinMode(pins.hold_high[i], OUTPUT);
            digitalWrite(pins.hold_high[i], HIGH);
        }
    }
    pinMode(pins.rst, OUTPUT);
    digitalWrite(pins.rst, LOW);
    delay(2);
    digitalWrite(pins.rst, HIGH);
    delay(5);
    STEP(wait_busy(), "busy after reset", -1);

    STEP(cmd1(CMD_SET_STANDBY, STDBY_RC), "standby", -2);
    /* 1.8 V TCXO, 10 ms start-up (640 x 15.625 us) */
    static const uint8_t tcxo[4] = { TCXO_1V8, 0x00, 0x02, 0x80 };
    STEP(cmd(CMD_SET_DIO3_TCXO, tcxo, 4), "tcxo", -3);
    /* With a TCXO the chip must be recalibrated; XOSC_START from the
     * first attempt is expected and cleared here. */
    STEP(cmd1(CMD_CALIBRATE, 0x7F), "calibrate", -4);
    delay(5);
    STEP(wait_busy(), "busy after calibrate", -4);
    STEP(cmd(CMD_CLEAR_DEVICE_ERRORS, (const uint8_t *)"\0\0", 2), "clear errors", -4);

    STEP(cmd1(CMD_SET_REGULATOR, 0x01), "regulator", -5);         /* DC-DC */
    STEP(cmd1(CMD_SET_DIO2_RF_SWITCH, 0x01), "rf switch", -5);
    STEP(cmd1(CMD_SET_PACKET_TYPE, 0x01), "packet type", -5);     /* LoRa */

    STEP(calibrate_image(prof.freq_hz), "calibrate image", -6);
    STEP(wait_busy(), "busy after image cal", -6);
    uint32_t frf = (uint32_t)(((uint64_t)prof.freq_hz << 25) / 32000000ull);
    uint8_t fa[4] = { (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
                      (uint8_t)(frf >> 8), (uint8_t)frf };
    STEP(cmd(CMD_SET_RF_FREQUENCY, fa, 4), "frequency", -6);

    /* SX1262 high-power PA, up to +22 dBm */
    static const uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };
    STEP(cmd(CMD_SET_PA_CONFIG, pa, 4), "pa config", -7);
    int8_t pwr = prof.power_dbm;
    if (pwr > 22) pwr = 22;
    if (pwr < -9) pwr = -9;
    uint8_t txp[2] = { (uint8_t)pwr, 0x04 };                        /* 200 us ramp */
    STEP(cmd(CMD_SET_TX_PARAMS, txp, 2), "tx params", -7);
    static const uint8_t ocp = 0x38;                                /* 140 mA */
    STEP(write_reg(REG_OCP, &ocp, 1), "ocp", -7);
    uint8_t clamp;
    STEP(read_reg(REG_TX_CLAMP, &clamp), "tx clamp read", -7);
    clamp |= 0x1E;                                                  /* errata 15.2 */
    STEP(write_reg(REG_TX_CLAMP, &clamp, 1), "tx clamp", -7);

    static const uint8_t base[2] = { 0x00, 0x00 };
    STEP(cmd(CMD_SET_BUFFER_BASE, base, 2), "buffer base", -8);
    uint8_t mod[4] = { prof.sf, bw_code(prof.bw_hz),
                       (uint8_t)(prof.cr_denom - 4), needs_ldro(&prof) ? (uint8_t)1 : (uint8_t)0 };
    STEP(cmd(CMD_SET_MOD_PARAMS, mod, 4), "modulation", -8);
    STEP(packet_params(CYBIKO_LINK_MAX_PHYS), "packet params", -8);
    uint8_t iq;
    STEP(read_reg(REG_IQ_POLARITY, &iq), "iq read", -8);
    iq |= 0x04;                                                     /* errata 15.4, standard IQ */
    STEP(write_reg(REG_IQ_POLARITY, &iq, 1), "iq", -8);
    uint8_t sync[2] = { (uint8_t)(prof.sync_word >> 8), (uint8_t)prof.sync_word };
    STEP(write_reg(REG_SYNC_WORD, sync, 2), "sync word", -8);

    const uint16_t latched = IRQ_USED | IRQ_CARRIER;
    uint8_t dio[8] = { (uint8_t)(latched >> 8), (uint8_t)latched,
                       (uint8_t)(IRQ_USED >> 8), (uint8_t)IRQ_USED, 0, 0, 0, 0 };
    STEP(cmd(CMD_SET_DIO_IRQ, dio, 8), "dio irq", -9);
    STEP(clear_irq(0xFFFF), "clear irq", -9);

    uint8_t err[2];
    STEP(get(CMD_GET_DEVICE_ERRORS, err, 2), "device errors", -10);
    stats.device_errors = (uint16_t)((err[0] << 8) | err[1]);
    STEP(start_rx(), "start rx", -11);
    uint8_t st;
    STEP(xfer((const uint8_t *)"\xC0", 1, &st, 1), "status", -12);
    stats.status = st;
    /* Chip mode in bits 6:4; 5 = RX */
    STEP(((st >> 4) & 0x7) == 5, "not in rx", -12);

    phy.impl = NULL;
    phy.tx = phy_tx;
    phy.tx_busy = phy_tx_busy;
    phy.rx = phy_rx;
    phy.poll = phy_poll;
    phy.mtu = CYBIKO_LINK_MAX_PHYS;
    busy_ms = BUSY_RUN_MS;
    started = true;
    set_error(stats.device_errors ? "ok (device errors set)" : "ok");
    return 0;
}

cybiko_radio_phy_t *cybiko_sx1262_phy(void) { return started ? &phy : NULL; }
const cybiko_sx1262_stats_t *cybiko_sx1262_phy_stats(void) { return &stats; }
const char *cybiko_sx1262_phy_error(void) { return last_error; }

int cybiko_sx1262_phy_probe(void) {
    if (!started || !bus_ok())
        return -1;
    uint8_t op = CMD_GET_STATUS, st = 0;
    if (!xfer(&op, 1, &st, 1))
        return -1;
    stats.status = st;
    return st;
}
