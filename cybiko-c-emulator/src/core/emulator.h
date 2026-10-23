#ifndef CYBIKO_EMULATOR_H
#define CYBIKO_EMULATOR_H
#include "types.h"
#include "../hal/hal.h"

typedef struct cybiko_emu cybiko_emu_t;

cybiko_emu_t *cybiko_create(const cybiko_hal_t *hal);
void          cybiko_destroy(cybiko_emu_t *emu);
bool cybiko_load_boot_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_flash_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_nvram(cybiko_emu_t *emu, const uint8_t *data, size_t len);
void cybiko_reset(cybiko_emu_t *emu);
void cybiko_run_frame(cybiko_emu_t *emu);
struct cybiko_radio_coproc;
void cybiko_attach_radio(cybiko_emu_t *emu, struct cybiko_radio_coproc *coproc);
bool cybiko_is_running(const cybiko_emu_t *emu);
const uint8_t *cybiko_get_nvram(const cybiko_emu_t *emu, size_t *len);
uint8_t *cybiko_nvram(cybiko_emu_t *emu, size_t *len);
const uint8_t *cybiko_flash(const cybiko_emu_t *emu, size_t *len);
const uint8_t *cybiko_lcd_vram(const cybiko_emu_t *emu, size_t *len);
const uint8_t *cybiko_onchip(const cybiko_emu_t *emu, size_t *len);
uint32_t cybiko_cpu_pc(const cybiko_emu_t *emu);
bool cybiko_cpu_halted(const cybiko_emu_t *emu);

/* ISO-HDLC CRC-32, the same value java.util.zip.CRC32 produces. */
uint32_t cybiko_crc32(const uint8_t *data, size_t len);

/*
 * XT CyID patch. Flash offset 0x7F818, big-endian, then CRC-32 of the
 * 2044-byte config block at 0x7F800 stored at 0x7FFFC. This matches
 * CybikoEmulator.patchCyId. Returns false when the image is shorter
 * than the Xtreme config block.
 */
bool cybiko_patch_cyid_image(uint8_t *flash, size_t len, uint32_t cyid);
bool cybiko_patch_cyid(cybiko_emu_t *emu, uint32_t cyid);

#define CYBIKO_LCD_WIDTH   160
#define CYBIKO_LCD_HEIGHT  100
#define CYBIKO_CLOCK_HZ    18432000
#define CYBIKO_FPS         60
#define CYBIKO_CYCLES_PER_FRAME (CYBIKO_CLOCK_HZ / CYBIKO_FPS)
#define CYBIKO_KEYBOARD_COLUMNS 15  /* total matrix entries (HAL copies all) */
#define CYBIKO_KEYBOARD_SCAN_COLS 10 /* columns scanned by keyboard_read (0-9) */

#endif
