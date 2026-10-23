/* Two Cybiko Classics on the in-process radio (radio_sim), the V1 version
 * of cybiko-c-emulator/tools/two_boot. No keys are pressed: this checks
 * the radio comes up on both and beacons cross.
 *   v1_two <boot.bin> <flash.bin> [frames]
 * Build with tools/v1_two.sh. */
#include <stdio.h>
#include <stdlib.h>
#include "core/emulator.h"
#include "radio_coproc.h"
#include "radio_sim.h"

#define ID_A 0x4C000001u
#define ID_B 0x4C000002u

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) { perror(p); exit(1); }
    fclose(f);
    return b;
}

static void report(const char *tag, int frame, cybiko_emu_t *e, const cybiko_radio_coproc_t *c) {
    static uint64_t last_insns[2], last_sleep[2];
    int k = tag[0] == 'A' ? 0 : 1;
    cybiko_v1_radio_status_t st;
    cybiko_stats_t ss;
    cybiko_v1_radio_status(e, &st);
    cybiko_get_stats(e, &ss);
    printf("frame %5d %s: rf %s%s boot %s  frames sent %u, coproc tx %u rx %u, delivered %u, ch %u"
           "  | %llu insns/frame, asleep %.0f%%\n",
           frame, tag, st.rf_armed ? "armed" : "-", st.rf_resolved ? "+resolved" : "",
           st.bootstrapped ? "yes" : "no", st.tx_frames, c->stat_tx, c->stat_rx,
           st.rx_injected, c->current_channel,
           (unsigned long long)((ss.instructions - last_insns[k]) / 600),
           100.0 * (double)(ss.sleep_cycles - last_sleep[k]) / (600.0 * CYBIKO_CYCLES_PER_FRAME));
    last_insns[k] = ss.instructions;
    last_sleep[k] = ss.sleep_cycles;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s boot.bin flash.bin [frames]\n", argv[0]); return 2; }
    size_t bn, fn;
    uint8_t *boot = slurp(argv[1], &bn), *flash = slurp(argv[2], &fn);
    int frames = argc > 3 ? atoi(argv[3]) : 6000;
    cybiko_hal_t hal = {0};
    cybiko_emu_t *a = cybiko_create(&hal), *b = cybiko_create(&hal);
    cybiko_load_boot_rom(a, boot, bn); cybiko_load_flash_rom(a, flash, fn);
    cybiko_load_boot_rom(b, boot, bn); cybiko_load_flash_rom(b, flash, fn);

    static radio_sim_bus_t bus;
    static radio_sim_node_t na, nb;
    static cybiko_radio_coproc_t ca, cb;
    radio_sim_bus_init(&bus);
    radio_sim_node_init(&na, &bus, ID_A, 255);
    radio_sim_node_init(&nb, &bus, ID_B, 255);
    radio_sim_bus_add(&bus, &na);
    radio_sim_bus_add(&bus, &nb);
    cybiko_radio_coproc_init(&ca);
    cybiko_radio_coproc_init(&cb);
    cybiko_radio_coproc_set_transport(&ca, &na.base);
    cybiko_radio_coproc_set_transport(&cb, &nb.base);
    cybiko_attach_radio_id(a, &ca, ID_A);
    cybiko_attach_radio_id(b, &cb, ID_B);
    cybiko_reset(a);
    cybiko_reset(b);

    for (int f = 1; f <= frames; f++) {
        cybiko_run_frame(a);
        cybiko_run_frame(b);
        radio_sim_bus_pump(&bus);
        if (f % 600 == 0 || f == frames) {
            report("A", f, a, &ca);
            report("B", f, b, &cb);
        }
    }
    cybiko_v1_radio_status_t sa, sb;
    cybiko_v1_radio_status(a, &sa);
    cybiko_v1_radio_status(b, &sb);
    int ok = ca.stat_rx > 0 && cb.stat_rx > 0 && sa.rx_injected > 0 && sb.rx_injected > 0;
    printf(ok ? "PASS: both Classics sent beacons and delivered the other's to CyOS\n"
              : "FAIL: no frames crossed both ways\n");
    return ok ? 0 : 1;
}
