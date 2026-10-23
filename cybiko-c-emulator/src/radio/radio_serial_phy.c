#include "radio_serial_phy.h"
#include "modem_proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

/* Longer than the modem's own 4 s SetTx timeout */
#define TX_LOST_MS 5000

static uint64_t now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

#ifdef _WIN32
static int write_all(intptr_t fd, const uint8_t *p, size_t n) {
    HANDLE h = (HANDLE)fd;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, p, (DWORD)n, &w, NULL) || w == 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
#else
static int write_all(intptr_t fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write((int)fd, p, n);
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
#endif

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

#ifdef _WIN32
static int serial_read(intptr_t fd, uint8_t *buf, size_t cap) {
    DWORD n = 0;
    if (fd == (intptr_t)INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile((HANDLE)fd, buf, (DWORD)cap, &n, NULL))
        return 0;
    return (int)n;
}
#else
static int serial_read(intptr_t fd, uint8_t *buf, size_t cap) {
    ssize_t n = read((int)fd, buf, cap);
    return n > 0 ? (int)n : 0;
}
#endif

static void phy_poll(cybiko_radio_phy_t *self) {
    cybiko_serial_phy_t *s = (cybiko_serial_phy_t *)self->impl;
    uint8_t buf[1024];
    for (;;) {
        int n = serial_read(s->fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++) {
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

#ifdef _WIN32
static void win_serial_path(const char *path, char *out, size_t cap) {
    if (strncmp(path, "\\\\.\\", 4) == 0 || strncmp(path, "//./", 4) == 0) {
        snprintf(out, cap, "%s", path);
        return;
    }
    snprintf(out, cap, "\\\\.\\%s", path);
}

int cybiko_serial_phy_open(cybiko_serial_phy_t *s, const char *path) {
    char winpath[96];
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    memset(s, 0, sizeof *s);
    s->fd = (intptr_t)INVALID_HANDLE_VALUE;
    win_serial_path(path, winpath, sizeof winpath);
    h = CreateFileA(winpath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            ? ENOENT : (e == ERROR_ACCESS_DENIED ? EACCES : EIO);
        return -1;
    }
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb))
        goto fail;
    dcb.BaudRate = 921600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb))
        goto fail;
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;
    if (!SetCommTimeouts(h, &to))
        goto fail;
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s->fd = (intptr_t)h;
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
    CloseHandle(h);
    s->fd = (intptr_t)INVALID_HANDLE_VALUE;
    errno = EIO;
    return -1;
}

void cybiko_serial_phy_close(cybiko_serial_phy_t *s) {
    if (s->fd != (intptr_t)INVALID_HANDLE_VALUE && s->fd != 0)
        CloseHandle((HANDLE)s->fd);
    s->fd = (intptr_t)INVALID_HANDLE_VALUE;
    free(s->dec);
    s->dec = NULL;
}
#else
int cybiko_serial_phy_open(cybiko_serial_phy_t *s, const char *path) {
    memset(s, 0, sizeof *s);
    s->fd = -1;
    s->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s->fd < 0)
        return -1;
    struct termios tio;
    if (tcgetattr((int)s->fd, &tio) != 0)
        goto fail;
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CRTSCTS | HUPCL);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetspeed(&tio, B921600) != 0 || tcsetattr((int)s->fd, TCSANOW, &tio) != 0)
        goto fail;
    tcflush((int)s->fd, TCIOFLUSH);
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
        close((int)s->fd);
        s->fd = -1;
        errno = e;
    }
    return -1;
}

void cybiko_serial_phy_close(cybiko_serial_phy_t *s) {
    if (s->fd >= 0)
        close((int)s->fd);
    s->fd = -1;
    free(s->dec);
    s->dec = NULL;
}
#endif

void cybiko_serial_phy_request_status(cybiko_serial_phy_t *s) {
    static const uint8_t q = 'S';
    send_frame(s, &q, 1);
}
