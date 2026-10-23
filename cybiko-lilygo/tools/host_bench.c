/* Host bench + regression check for the T-Deck vendored core.
 * bench <boot.bin> <flash.bin> <frames> [dump_addr dump_len]
 * Prints instruction count, wall time, and a hash of guest RAM + LCD
 * every 250 frames. Same ROMs and frame count must give the same hashes
 * before and after an interpreter change. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/emulator.h"
#include "radio_coproc.h"
#include "radio_sim.h"

static uint32_t lcd_hash;
static void render(void *ctx, const uint8_t *px, int w, int h) {
    (void)ctx;
    uint32_t x = 2166136261u;
    for (int i = 0; i < w * h; i++) x = (x ^ px[i]) * 16777619u;
    lcd_hash = x;
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) { perror(p); exit(1); }
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage\n"); return 2; }
    size_t bn, fn;
    uint8_t *boot = slurp(argv[1], &bn), *flash = slurp(argv[2], &fn);
    int frames = atoi(argv[3]);
    cybiko_hal_t hal = {0};
    hal.render_frame = render;
    cybiko_emu_t *emu = cybiko_create(&hal);
    cybiko_load_boot_rom(emu, boot, bn);
    cybiko_load_flash_rom(emu, flash, fn);
    /* BENCH_RADIO=1: attach a co-processor on a one-node sim bus, the way
     * the firmware will once the SX1262 transport is live. */
    static radio_sim_bus_t rbus; static radio_sim_node_t rnode;
    static cybiko_radio_coproc_t cop;
    if (getenv("BENCH_RADIO")) {
        radio_sim_bus_init(&rbus);
        radio_sim_node_init(&rnode, &rbus, 0x10000001u, 255);
        radio_sim_bus_add(&rbus, &rnode);
        cybiko_radio_coproc_init(&cop);
        cybiko_radio_coproc_set_transport(&cop, &rnode.base);
        cybiko_attach_radio(emu, &cop);
    }
    cybiko_reset(emu);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 1; f <= frames; f++) {
        cybiko_run_frame(emu);
        if (f % 250 == 0 || f == frames) {
            size_t len;
            const uint8_t *ram = cybiko_get_nvram(emu, &len);
            uint32_t x = 2166136261u;
            for (size_t i = 0; i < len; i++) x = (x ^ ram[i]) * 16777619u;
            cybiko_stats_t st;
            cybiko_get_stats(emu, &st);
            printf("frame %5d insns %10llu ram %08X lcd %08X\n", f,
                   (unsigned long long)st.instructions, x, lcd_hash);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    cybiko_stats_t st;
    cybiko_get_stats(emu, &st);
    fprintf(stderr, "%.3f s, %.1f MIPS\n", s, st.instructions / s / 1e6);
    if (argc >= 6) {
        unsigned a = strtoul(argv[4], 0, 16), n = strtoul(argv[5], 0, 16);
        size_t len;
        const uint8_t *ram = cybiko_get_nvram(emu, &len);
        FILE *o = fopen("dump.bin", "wb");
        fwrite(ram + (a - CYBIKO_EXT_RAM_BASE), 1, n, o);
        fclose(o);
    }
    return 0;
}
