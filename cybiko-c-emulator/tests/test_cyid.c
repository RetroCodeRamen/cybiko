#include "acutest.h"
#include "core/emulator.h"
#include "hal/hal.h"

#include <stdlib.h>
#include <string.h>

static void test_crc32_known_vector(void) {
    const uint8_t msg[] = "123456789";
    TEST_CHECK(cybiko_crc32(msg, 9) == 0xCBF43926u);
}

static void test_short_image_is_rejected(void) {
    uint8_t small[256];
    memset(small, 0xA5, sizeof small);
    TEST_CHECK(!cybiko_patch_cyid_image(small, sizeof small, 0x10000001u));
    TEST_CHECK(small[0] == 0xA5);
}

static void test_patch_rewrites_id_and_checksum(void) {
    uint8_t *flash = calloc(0x80000, 1);
    TEST_CHECK(flash != NULL);
    memset(flash + 0x7F800, 0x5A, 2048);
    uint32_t before = cybiko_crc32(flash + 0x7F800, 2044);

    TEST_CHECK(cybiko_patch_cyid_image(flash, 0x80000, 0x10000001u));
    TEST_CHECK(flash[0x7F818] == 0x10);
    TEST_CHECK(flash[0x7F819] == 0x00);
    TEST_CHECK(flash[0x7F81A] == 0x00);
    TEST_CHECK(flash[0x7F81B] == 0x01);
    uint32_t sum = cybiko_crc32(flash + 0x7F800, 2044);
    TEST_CHECK(sum != before);
    TEST_CHECK(flash[0x7FFFC] == (uint8_t)(sum >> 24));
    TEST_CHECK(flash[0x7FFFD] == (uint8_t)(sum >> 16));
    TEST_CHECK(flash[0x7FFFE] == (uint8_t)(sum >> 8));
    TEST_CHECK(flash[0x7FFFF] == (uint8_t)sum);

    TEST_CHECK(cybiko_patch_cyid_image(flash, 0x80000, 0x10000002u));
    TEST_CHECK(flash[0x7F81B] == 0x02);
    uint32_t sum2 = cybiko_crc32(flash + 0x7F800, 2044);
    TEST_CHECK(sum2 != sum);
    TEST_CHECK(flash[0x7FFFF] == (uint8_t)sum2);
    free(flash);
}

static void test_emulator_flash_is_patched(void) {
    cybiko_hal_t hal;
    memset(&hal, 0, sizeof hal);
    cybiko_emu_t *emu = cybiko_create(&hal);
    TEST_CHECK(emu != NULL);

    size_t len = 0;
    const uint8_t *flash = cybiko_flash(emu, &len);
    TEST_CHECK(len == 0x80000);
    TEST_CHECK(cybiko_patch_cyid(emu, 0x0A0B0C0Du));
    TEST_CHECK(flash[0x7F818] == 0x0A);
    TEST_CHECK(flash[0x7F819] == 0x0B);
    TEST_CHECK(flash[0x7F81A] == 0x0C);
    TEST_CHECK(flash[0x7F81B] == 0x0D);
    uint32_t sum = cybiko_crc32(flash + 0x7F800, 2044);
    TEST_CHECK(flash[0x7FFFC] == (uint8_t)(sum >> 24));
    TEST_CHECK(flash[0x7FFFF] == (uint8_t)sum);

    size_t vlen = 0;
    TEST_CHECK(cybiko_lcd_vram(emu, &vlen) != NULL);
    TEST_CHECK(vlen == 4000);
    TEST_CHECK(cybiko_cpu_pc(emu) == 0);
    TEST_CHECK(!cybiko_cpu_halted(emu));
    cybiko_destroy(emu);
}

TEST_LIST = {
    { "crc32 matches the ISO-HDLC vector", test_crc32_known_vector },
    { "short flash is left unchanged", test_short_image_is_rejected },
    { "patch stores CyID and a new checksum", test_patch_rewrites_id_and_checksum },
    { "emulator flash image is patched in place", test_emulator_flash_is_patched },
    { NULL, NULL }
};
