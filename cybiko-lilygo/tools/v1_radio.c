/* One Cybiko Classic on a Heltec LoRa modem. Host only.
 *   v1_radio boot.bin flash.bin --radio /dev/ttyUSB0 [--cyid 4C000001] [--seconds N]
 * Build with tools/v1_radio.sh. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "core/emulator.h"
#include "radio_coproc.h"
#include "radio_lora.h"
#include "radio_serial_phy.h"

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (!b || fread(b, 1, *n, f) != *n) { perror(p); exit(1); }
    fclose(f);
    return b;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void radio_on_text(void *user, char type, const char *text) {
    (void)user;
    fprintf(stderr, "modem %s: %s\n", type == 'H' ? "hello" : "status", text);
}

static void report(int frame, cybiko_emu_t *e, const cybiko_radio_coproc_t *c,
                   const cybiko_lora_t *lt, const cybiko_serial_phy_t *sp) {
    cybiko_v1_radio_status_t st;
    cybiko_v1_radio_status(e, &st);
    printf("frame %5d: rf %s%s boot %s  guest sent %u delivered %u  "
           "coproc tx %u rx %u  lora tx msgs %u pkts %u held %u  "
           "rx pkts %u msgs %u bad %u  rssi %d snr %d\n",
           frame,
           st.rf_armed ? "armed" : "-", st.rf_resolved ? "+up" : "",
           st.bootstrapped ? "yes" : "no",
           st.tx_frames, st.rx_injected,
           c->stat_tx, c->stat_rx,
           lt->stats.tx_msgs, lt->stats.tx_pkts, lt->stats.tx_held,
           lt->stats.rx_pkts, lt->stats.rx_msgs, lt->stats.rx_bad,
           sp->stats.last_rssi_dbm, sp->stats.last_snr_db);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *boot_path = NULL, *flash_path = NULL, *radio_path = NULL;
    uint32_t cyid = 0x4C000001u;
    int seconds = 90;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--radio") == 0 && i + 1 < argc)
            radio_path = argv[++i];
        else if (strcmp(argv[i], "--cyid") == 0 && i + 1 < argc)
            cyid = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (!boot_path)
            boot_path = argv[i];
        else if (!flash_path)
            flash_path = argv[i];
        else {
            fprintf(stderr, "unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!boot_path || !flash_path || !radio_path) {
        fprintf(stderr, "usage: %s boot.bin flash.bin --radio /dev/ttyUSB0 "
                "[--cyid 4C000001] [--seconds 90]\n", argv[0]);
        return 2;
    }

    size_t bn, fn;
    uint8_t *boot = slurp(boot_path, &bn), *flash = slurp(flash_path, &fn);
    cybiko_hal_t hal = {0};
    cybiko_emu_t *emu = cybiko_create(&hal);
    if (!emu || !cybiko_load_boot_rom(emu, boot, bn) ||
        !cybiko_load_flash_rom(emu, flash, fn)) {
        fprintf(stderr, "failed to create emulator or load ROMs\n");
        return 1;
    }
    free(boot);
    free(flash);

    static cybiko_serial_phy_t phy;
    static cybiko_lora_t link;
    static cybiko_radio_coproc_t cop;
    if (cybiko_serial_phy_open(&phy, radio_path) != 0) {
        fprintf(stderr, "radio '%s': %s\n", radio_path, strerror(errno));
        return 1;
    }
    phy.on_text = radio_on_text;
    cybiko_lora_init(&link, &phy.phy, cyid, true);
    cybiko_radio_coproc_init(&cop);
    cybiko_radio_coproc_set_transport(&cop, &link.base);
    cybiko_attach_radio_id(emu, &cop, cyid);
    cybiko_serial_phy_request_status(&phy);
    cybiko_reset(emu);
    fprintf(stderr, "Classic on %s, CyID %08X, %d s at 60 fps\n",
            radio_path, (unsigned)cyid, seconds);

    uint64_t start = now_ms(), next = start, last_rep = start;
    int frame = 0;
    while ((int)(now_ms() - start) < seconds * 1000) {
        cybiko_run_frame(emu);
        link.base.pump(&link.base);
        frame++;
        uint64_t t = now_ms();
        if (t - last_rep >= 5000) {
            report(frame, emu, &cop, &link, &phy);
            last_rep = t;
        }
        next += 16;
        if (next > t)
            usleep((useconds_t)((next - t) * 1000));
        else
            next = t;
    }
    report(frame, emu, &cop, &link, &phy);
    cybiko_v1_radio_status_t st;
    cybiko_v1_radio_status(emu, &st);
    int ok = cop.stat_rx > 0 && st.rx_injected > 0 && link.stats.tx_pkts > 0;
    printf(ok ? "PASS: this Classic put frames on the air and CyOS took a peer frame\n"
              : "FAIL: no two-way guest frames (tx pkts %u, coproc rx %u, delivered %u)\n",
           link.stats.tx_pkts, cop.stat_rx, st.rx_injected);
    cybiko_serial_phy_close(&phy);
    return ok ? 0 : 1;
}
