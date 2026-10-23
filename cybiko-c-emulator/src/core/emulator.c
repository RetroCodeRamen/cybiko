/*
 * Cybiko emulator orchestrator.
 *
 * Ties together CPU, address bus, LCD, timers, RTC, speaker, and keyboard
 * into a single runnable emulator driven by a platform HAL.
 */
#include "emulator.h"
#include "h8s_cpu.h"
#include "address_bus.h"
#include "hd66421.h"
#include "timer8.h"
#include "timer16.h"
#include "rtc.h"
#include "speaker.h"
#include "keyboard.h"
#include "cfs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct cybiko_emu {
    cybiko_hal_t hal;

    h8s_cpu_t     cpu;
    address_bus_t bus;
    hd66421_t     lcd;
    timer8_t      timer8[2];
    timer16_t     timer16[6];
    rtc_t         rtc;
    speaker_t     speaker;
    keyboard_t    keyboard;

    bool     running;
    uint64_t total_steps;
    uint32_t frame_count;
};

/* Timer16 channel 1 TIOCB1 output compare → speaker (Port 1 bit 3) */
static void timer16_ch1_tiocb_cb(void *ctx, int level) {
    cybiko_emu_t *emu = ctx;
    speaker_set_level(&emu->speaker, level);
}

/* ---------- lifecycle ---------- */

cybiko_emu_t *cybiko_create(const cybiko_hal_t *hal) {
    cybiko_emu_t *emu = calloc(1, sizeof(cybiko_emu_t));
    if (!emu) return NULL;

    /* Copy the HAL callbacks */
    emu->hal = *hal;

    /* Initialize the bus itself (must come before memory_init - zeroes struct) */
    bus_init(&emu->bus);

    /* Initialize memory regions in the bus */
    memory_init(&emu->bus.boot_rom,     0x8000,   false);  /* 32 KB, read-only  */
    memory_init(&emu->bus.external_ram, 0x200000, true);   /* 2 MB, writable    */
    memory_init(&emu->bus.flash_rom,    0x80000,  false);  /* 512 KB, read-only */
    memory_init(&emu->bus.on_chip_ram,  0x2400,   true);   /* ~10 KB, writable  */

    /* Wire peripherals into bus */
    emu->bus.lcd       = &emu->lcd;
    emu->bus.timer8[0] = &emu->timer8[0];
    emu->bus.timer8[1] = &emu->timer8[1];
    for (int i = 0; i < 6; i++) {
        emu->bus.timer16[i] = &emu->timer16[i];
    }
    emu->bus.rtc      = &emu->rtc;
    emu->bus.speaker  = &emu->speaker;
    emu->bus.keyboard = &emu->keyboard;

    /* Initialize peripherals */
    hd66421_init(&emu->lcd);

    timer8_init(&emu->timer8[0], 0, &emu->cpu);
    timer8_init(&emu->timer8[1], 1, &emu->cpu);

    /* Timer16 channels: (channel, tgrCount, baseVector, cpu) */
    timer16_init(&emu->timer16[0], 0, 4, 32, &emu->cpu);
    timer16_init(&emu->timer16[1], 1, 2, 40, &emu->cpu);
    timer16_init(&emu->timer16[2], 2, 2, 44, &emu->cpu);
    timer16_init(&emu->timer16[3], 3, 4, 48, &emu->cpu);
    timer16_init(&emu->timer16[4], 4, 2, 56, &emu->cpu);
    timer16_init(&emu->timer16[5], 5, 2, 60, &emu->cpu);

    /* Wire Timer16 ch1 TIOCB1 output compare to speaker */
    emu->timer16[1].output_b_cb = timer16_ch1_tiocb_cb;
    emu->timer16[1].output_b_ctx = emu;

    rtc_init(&emu->rtc);
    speaker_init(&emu->speaker, CYBIKO_CLOCK_HZ);
    keyboard_init(&emu->keyboard, CYBIKO_KEYBOARD_SCAN_COLS);

    /* Initialize CPU (needs bus already wired) */
    h8s_cpu_init(&emu->cpu, &emu->bus);

    /* Wire CPU back into bus (for interrupt delivery, etc.) */
    emu->bus.cpu = &emu->cpu;

    emu->running = true;

    return emu;
}

void cybiko_destroy(cybiko_emu_t *emu) {
    if (!emu) return;
    memory_free(&emu->bus.boot_rom);
    memory_free(&emu->bus.external_ram);
    memory_free(&emu->bus.flash_rom);
    memory_free(&emu->bus.on_chip_ram);
    free(emu);
}

/* ---------- ROM / NVRAM loading ---------- */

bool cybiko_load_boot_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len) {
    memory_load(&emu->bus.boot_rom, data, len, 0);
    return true;
}

bool cybiko_load_flash_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len) {
    memory_load(&emu->bus.flash_rom, data, len, 0);
    return true;
}

bool cybiko_load_nvram(cybiko_emu_t *emu, const uint8_t *data, size_t len) {
    memory_load(&emu->bus.external_ram, data, len, 0);
    return true;
}

/* ---------- reset ---------- */

void cybiko_reset(cybiko_emu_t *emu) {
    h8s_cpu_reset(&emu->cpu);
}

/* ---------- main frame loop ---------- */

void cybiko_run_frame(cybiko_emu_t *emu) {
    /* Poll keyboard state from the HAL */
    if (emu->hal.keyboard_poll) {
        emu->hal.keyboard_poll(emu->hal.ctx,
                               emu->keyboard.columns,
                               CYBIKO_KEYBOARD_COLUMNS);
    }

    /* Cache which timers are running to avoid per-cycle checks */
    bool t8_0 = timer8_is_running(&emu->timer8[0]);
    bool t8_1 = timer8_is_running(&emu->timer8[1]);
    bool t16[6];
    for (int i = 0; i < 6; i++) {
        t16[i] = timer16_is_running(&emu->timer16[i]);
    }

    /* Begin speaker frame (record start level, reset transitions) */
    speaker_begin_frame(&emu->speaker);

    /* Execute one frame worth of CPU cycles */
    for (int cycle = 0; cycle < CYBIKO_CYCLES_PER_FRAME; cycle++) {
        /* Track cycle position for speaker transition timestamps */
        emu->speaker.frame_cycle = cycle;

        /* Tick timers */
        if (t8_0) timer8_tick(&emu->timer8[0]);
        if (t8_1) timer8_tick(&emu->timer8[1]);
        for (int i = 0; i < 6; i++) {
            if (t16[i]) timer16_tick(&emu->timer16[i]);
        }

        /* CPU step */
        h8s_cpu_step(&emu->cpu);

        /* SCI2 byte time and idle-frame injection. One guest cycle. */
        bus_tick_sci2(&emu->bus, 1);

        /* DMA completion */
        bus_tick_dma_completion(&emu->bus);

        emu->total_steps++;
    }

    /* Render the frame via HAL */
    if (emu->hal.render_frame) {
        const uint8_t *pixels = hd66421_render(&emu->lcd);
        emu->hal.render_frame(emu->hal.ctx, pixels,
                              CYBIKO_LCD_WIDTH, CYBIKO_LCD_HEIGHT);
    }

    /* Generate audio samples for this frame */
    if (emu->hal.audio_output) {
        uint8_t audio_buf[8192];
        int samples = speaker_generate_samples(&emu->speaker,
                                               CYBIKO_CYCLES_PER_FRAME,
                                               audio_buf, sizeof(audio_buf));
        if (samples > 0) {
            emu->hal.audio_output(emu->hal.ctx, audio_buf, samples);
        }
    }

    /* Tick RTC once per frame */
    bus_tick_rtc(&emu->bus);

    /* Drain serial output buffers to the HAL */
    if (emu->hal.serial_output) {
        for (int ch = 0; ch < 3; ch++) {
            for (int i = 0; i < emu->bus.sci_output_pos[ch]; i++) {
                emu->hal.serial_output(emu->hal.ctx,
                                       (uint8_t)emu->bus.sci_output[ch][i]);
            }
            emu->bus.sci_output_pos[ch] = 0;
        }
    }

    emu->frame_count++;
}

/* ---------- queries ---------- */

bool cybiko_is_running(const cybiko_emu_t *emu) {
    return emu->running && !emu->cpu.halted;
}

void cybiko_attach_radio(cybiko_emu_t *emu, struct cybiko_radio_coproc *coproc) {
    if (!emu) return;
    bus_attach_radio(&emu->bus, coproc);
}

const uint8_t *cybiko_get_nvram(const cybiko_emu_t *emu, size_t *len) {
    if (len) *len = emu->bus.external_ram.size;
    return memory_raw((memory_t *)&emu->bus.external_ram);
}

uint8_t *cybiko_nvram(cybiko_emu_t *emu, size_t *len) {
    if (len) *len = emu->bus.external_ram.size;
    return memory_raw(&emu->bus.external_ram);
}

const uint8_t *cybiko_flash(const cybiko_emu_t *emu, size_t *len) {
    if (len) *len = emu->bus.flash_rom.size;
    return memory_raw((memory_t *)&emu->bus.flash_rom);
}

const uint8_t *cybiko_lcd_vram(const cybiko_emu_t *emu, size_t *len) {
    if (len) *len = HD66421_VRAM_SIZE;
    return emu->lcd.vram;
}

const uint8_t *cybiko_onchip(const cybiko_emu_t *emu, size_t *len) {
    if (len) *len = emu->bus.on_chip_ram.size;
    return memory_raw((memory_t *)&emu->bus.on_chip_ram);
}

uint32_t cybiko_cpu_pc(const cybiko_emu_t *emu) {
    return emu->cpu.pc;
}

bool cybiko_cpu_halted(const cybiko_emu_t *emu) {
    return emu->cpu.halted;
}

uint32_t cybiko_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool cybiko_patch_cyid_image(uint8_t *flash, size_t len, uint32_t cyid) {
    const size_t block = 0x7F800;
    const size_t id_at = 0x7F818;
    if (!flash || id_at + 4 > len || block + 2048 > len)
        return false;
    flash[id_at]     = (uint8_t)(cyid >> 24);
    flash[id_at + 1] = (uint8_t)(cyid >> 16);
    flash[id_at + 2] = (uint8_t)(cyid >> 8);
    flash[id_at + 3] = (uint8_t)cyid;
    uint32_t sum = cybiko_crc32(flash + block, 2044);
    flash[block + 2044] = (uint8_t)(sum >> 24);
    flash[block + 2045] = (uint8_t)(sum >> 16);
    flash[block + 2046] = (uint8_t)(sum >> 8);
    flash[block + 2047] = (uint8_t)sum;
    return true;
}

bool cybiko_patch_cyid(cybiko_emu_t *emu, uint32_t cyid) {
    if (!emu) return false;
    return cybiko_patch_cyid_image(memory_raw(&emu->bus.flash_rom),
                                   emu->bus.flash_rom.size, cyid);
}
