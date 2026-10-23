#include "radio_serial_phy.h"
#include "modem_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Longer than the modem's own 4 s SetTx timeout */
#define TX_LOST_MS 5000

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                usleep(200);
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_frame(cybiko_serial_phy_t *s, const uint8_t *body, size_t len) {
    uint8_t out[2 * MODEM_MAX_FRAME + 2];
    return write_all(s->fd, out, slip_encode(body, len, out));
}

static void handle_frame(cybiko_serial_phy_t *s, const uint8_t *f, size_t len) {
    switch (f[0]) {
    case 'R':
        if (len < 5)
            return;
        s->stats.last_rssi_dbm = (int16_t)(f[1] | (f[2] << 8));
        s->stats.last_snr_db = (int8_t)f[3];
        if (s->in_count == CYBIKO_SERIAL_PHY_INBOX) {
            s->stats.rx_overflow++;
            return;
        }
        {
            int slot = (s->in_head + s->in_count) % CYBIKO_SERIAL_PHY_INBOX;
            size_t n = len - 4;
            if (n > sizeof s->inbox[slot].pkt)
                n = sizeof s->inbox[slot].pkt;
            memcpy(s->inbox[slot].pkt, f + 4, n);
            s->inbox[slot].len = (uint8_t)n;
            s->in_count++;
            s->stats.rx_pkts++;
        }
        break;
    case 'D':
        if (len >= 2) {
            if (f[1] == MODEM_TX_DONE_OK) s->stats.tx_sent++;
            else if (f[1] == MODEM_TX_DONE_TIMEOUT) s->stats.tx_timeout++;
            else s->stats.tx_refused++;
        }
        s->tx_busy = 0;
        break;
    case 'H':
    case 'S': {
        char *dst = f[0] == 'H' ? s->hello : s->status;
        size_t cap = f[0] == 'H' ? sizeof s->hello : sizeof s->status;
        size_t n = len - 1 < cap - 1 ? len - 1 : cap - 1;
        memcpy(dst, f + 1, n);
        dst[n] = 0;
        if (f[0] == 'H')
            s->tx_busy = 0;   /* modem rebooted: nothing in flight */
        if (s->on_text)
            s->on_text(s->user, (char)f[0], dst);
        break;
    }
    }
}

static void phy_poll(cybiko_radio_phy_t *self) {
    cybiko_serial_phy_t *s = (cybiko_serial_phy_t *)self->impl;
    uint8_t buf[1024];
    for (;;) {
        ssize_t n = read(s->fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            size_t len = slip_feed((slip_decoder_t *)s->dec, buf[i]);
            if (len)
                handle_frame(s, ((slip_decoder_t *)s->dec)->buf, len);
        }
    }
    if (s->tx_busy && now_ms() - s->tx_started_ms > TX_LOST_MS) {
        s->stats.tx_lost++;
        s->tx_busy = 0;
    }
}

static int phy_tx(cybiko_radio_phy_t *self, const uint8_t *pkt, size_t len) {
    cybiko_serial_phy_t *s = (cybiko_serial_phy_t *)self->impl;
    if (s->tx_busy || len == 0 || len > 255)
        return -1;
    uint8_t body[1 + 255];
    body[0] = 'T';
    memcpy(body + 1, pkt, len);
    if (send_frame(s, body, len + 1) != 0)
        return -1;
    s->tx_busy = 1;
    s->tx_started_ms = now_ms();
    return 0;
}

static int phy_tx_busy(cybiko_radio_phy_t *self) {
    return ((cybiko_serial_phy_t *)self->impl)->tx_busy;
}

static int phy_rx(cybiko_radio_phy_t *self, uint8_t *buf, size_t cap, size_t *len) {
    cybiko_serial_phy_t *s = (cybiko_serial_phy_t *)self->impl;
    if (s->in_count == 0)
        return 0;
    size_t n = s->inbox[s->in_head].len;
    if (n > cap)
        n = cap;
    memcpy(buf, s->inbox[s->in_head].pkt, n);
    *len = n;
    s->in_head = (s->in_head + 1) % CYBIKO_SERIAL_PHY_INBOX;
    s->in_count--;
    return 1;
}

int cybiko_serial_phy_open(cybiko_serial_phy_t *s, const char *path) {
    memset(s, 0, sizeof *s);
    s->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s->fd < 0)
        return -1;
    struct termios tio;
    if (tcgetattr(s->fd, &tio) != 0)
        goto fail;
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CRTSCTS | HUPCL);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetspeed(&tio, B921600) != 0 || tcsetattr(s->fd, TCSANOW, &tio) != 0)
        goto fail;
    tcflush(s->fd, TCIOFLUSH);
    s->dec = calloc(1, sizeof(slip_decoder_t));
    if (!s->dec)
        goto fail;
    s->phy.impl = s;
    s->phy.tx = phy_tx;
    s->phy.tx_busy = phy_tx_busy;
    s->phy.rx = phy_rx;
    s->phy.poll = phy_poll;
    s->phy.mtu = 255;
    return 0;
fail:
    {
        int e = errno;
        close(s->fd);
        s->fd = -1;
        errno = e;
    }
    return -1;
}

void cybiko_serial_phy_close(cybiko_serial_phy_t *s) {
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
    free(s->dec);
    s->dec = NULL;
}

void cybiko_serial_phy_request_status(cybiko_serial_phy_t *s) {
    static const uint8_t q = 'S';
    send_frame(s, &q, 1);
}
