/* Headless boot trace for comparing the C core with the Java emulator.
 * v1_boot <boot.bin> <flash.bin> <frames> [screen.pgm]
 * Every 60 frames prints the line Java's STATUS log has:
 *   frame=N PC=0x... halted=b vram=CRC32
 * where vram is CRC-32 of the HD66421's 4000 bytes, as Java computes it.
 * Build with tools/v1_boot.sh (MACHINE=V1 by default). */
#include <stdio.h>
#include <stdlib.h>
#include "core/emulator.h"

static const uint8_t *last_px;
/* PRESS_AT env: from that frame, tap pad-right (V1 column 4 bit 0) for 4
 * frames out of every PRESS_EVERY (default 30). */
static int frame_no, press_at = -1, press_every = 30;
static void kb_poll(void *ctx, uint16_t *m, int n) {
    (void)ctx;
    for (int i = 0; i < n; i++) m[i] = 0;
    if (press_at >= 0 && frame_no >= press_at && (frame_no - press_at) % press_every < 4)
        m[4] = 0x01;
}
static void render(void *ctx, const uint8_t *px, int w, int h) {
    (void)ctx; (void)w; (void)h;
    last_px = px;
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
    if (argc < 4) {
        fprintf(stderr, "usage: %s boot.bin flash.bin frames [screen.pgm]\n", argv[0]);
        return 2;
    }
    size_t bn, fn;
    uint8_t *boot = slurp(argv[1], &bn), *flash = slurp(argv[2], &fn);
    int frames = atoi(argv[3]);
    cybiko_hal_t hal = {0};
    hal.render_frame = render;
    hal.keyboard_poll = kb_poll;
    if (getenv("PRESS_AT")) press_at = atoi(getenv("PRESS_AT"));
    if (getenv("PRESS_EVERY")) press_every = atoi(getenv("PRESS_EVERY"));
    cybiko_emu_t *emu = cybiko_create(&hal);
    cybiko_load_boot_rom(emu, boot, bn);
    cybiko_load_flash_rom(emu, flash, fn);
    /* ADC0 / ADC1 / ADC2 env: Classic ADC channels, 10-bit (1 and 2 are
     * the battery) */
    if (getenv("ADC0")) cybiko_set_adc(emu, 0, (uint16_t)strtol(getenv("ADC0"), NULL, 0));
    if (getenv("ADC1")) cybiko_set_adc(emu, 1, (uint16_t)strtol(getenv("ADC1"), NULL, 0));
    if (getenv("ADC2")) cybiko_set_adc(emu, 2, (uint16_t)strtol(getenv("ADC2"), NULL, 0));
    cybiko_reset(emu);
    cybiko_stats_t prev = {0};
    for (int f = 1; f <= frames; f++) {
        frame_no = f;
        cybiko_run_frame(emu);
        if (f % 60 == 0) {
            cybiko_stats_t st;
            cybiko_get_stats(emu, &st);
            printf("frame=%d PC=0x%06X halted=%s vram=%08X insns=%llu I=%d\n", f, cybiko_get_pc(emu),
                   cybiko_is_halted(emu) ? "true" : "false",
                   cybiko_crc32(cybiko_get_vram(emu), 4000),
                   (unsigned long long)(st.instructions - prev.instructions),
                   (cybiko_get_ccr(emu) >> 7) & 1);
            prev = st;
        }
    }
    /* RAMDUMP env: write guest external RAM (CyOS runs from it) */
    if (getenv("RAMDUMP")) {
        size_t rl;
        const uint8_t *ram = cybiko_get_nvram(emu, &rl);
        FILE *o = fopen(getenv("RAMDUMP"), "wb");
        fwrite(ram, 1, rl, o);
        fclose(o);
    }
    if (argc > 4 && last_px) {
        FILE *o = fopen(argv[4], "wb");
        fprintf(o, "P5\n%d %d\n255\n", CYBIKO_LCD_WIDTH, CYBIKO_LCD_HEIGHT);
        fwrite(last_px, 1, CYBIKO_LCD_WIDTH * CYBIKO_LCD_HEIGHT, o);
        fclose(o);
    }
    return 0;
}
