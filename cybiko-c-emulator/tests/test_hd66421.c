#include "acutest.h"
#include "core/hd66421.h"
#include <string.h>

static void test_init_defaults(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    TEST_CHECK(lcd.reg_index == 0);
    TEST_CHECK(lcd.x_addr == 0);
    TEST_CHECK(lcd.y_addr == 0);
    /* Palette defaults */
    TEST_CHECK(lcd.regs[0x0C] == 0);
    TEST_CHECK(lcd.regs[0x0D] == 10);
    TEST_CHECK(lcd.regs[0x0E] == 20);
    TEST_CHECK(lcd.regs[0x0F] == 31);
    TEST_CHECK(lcd.regs[0x10] == 0);
}

static void test_vram_zeroed(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    for (int i = 0; i < HD66421_VRAM_SIZE; i++) {
        TEST_CHECK(lcd.vram[i] == 0);
        if (lcd.vram[i] != 0) break;
    }
}

static void test_reg_index_write_read(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Write to offset 0 sets reg_index */
    hd66421_write8(&lcd, 0, 0x05);
    TEST_CHECK(hd66421_read8(&lcd, 0) == 0x05);
    /* Only 5 bits */
    hd66421_write8(&lcd, 0, 0xFF);
    TEST_CHECK(hd66421_read8(&lcd, 0) == 0x1F);
}

static void test_x_y_addr_writes(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Write X addr */
    hd66421_write8(&lcd, 0, 0x02); /* Select X_ADDR register */
    hd66421_write8(&lcd, 1, 10);   /* Write value */
    TEST_CHECK(lcd.x_addr == 10);
    /* Write Y addr */
    hd66421_write8(&lcd, 0, 0x03); /* Select Y_ADDR register */
    hd66421_write8(&lcd, 1, 50);
    TEST_CHECK(lcd.y_addr == 50);
}

static void test_vram_write_read(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Set position */
    hd66421_write8(&lcd, 0, 0x02); /* X_ADDR */
    hd66421_write8(&lcd, 1, 5);
    hd66421_write8(&lcd, 0, 0x03); /* Y_ADDR */
    hd66421_write8(&lcd, 1, 10);
    /* Write to VRAM */
    hd66421_write8(&lcd, 0, 0x04); /* RAM register */
    hd66421_write8(&lcd, 1, 0xAB);
    /* VRAM at y*40+x = 10*40+5 = 405 */
    TEST_CHECK(lcd.vram[405] == 0xAB);
    /* Read it back (reset position since Y auto-incremented) */
    hd66421_write8(&lcd, 0, 0x02);
    hd66421_write8(&lcd, 1, 5);
    hd66421_write8(&lcd, 0, 0x03);
    hd66421_write8(&lcd, 1, 10);
    hd66421_write8(&lcd, 0, 0x04); /* Select RAM */
    uint8_t val = hd66421_read8(&lcd, 1);
    TEST_CHECK_(val == 0xAB, "expected 0xAB, got 0x%02X", val);
}

static void test_auto_increment_y(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Default: Y increment (CONTROL2 INC bit = 0) */
    hd66421_write8(&lcd, 0, 0x02);
    hd66421_write8(&lcd, 1, 0); /* x=0 */
    hd66421_write8(&lcd, 0, 0x03);
    hd66421_write8(&lcd, 1, 0); /* y=0 */
    /* Write to RAM */
    hd66421_write8(&lcd, 0, 0x04);
    hd66421_write8(&lcd, 1, 0xFF);
    /* Y should have incremented to 1 */
    TEST_CHECK(lcd.y_addr == 1);
    TEST_CHECK(lcd.x_addr == 0);
}

static void test_auto_increment_x(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Set INC bit in CONTROL2 */
    hd66421_write8(&lcd, 0, 0x01); /* CONTROL2 */
    hd66421_write8(&lcd, 1, 0x02); /* INC=1 */
    /* Set position */
    hd66421_write8(&lcd, 0, 0x02);
    hd66421_write8(&lcd, 1, 0);
    hd66421_write8(&lcd, 0, 0x03);
    hd66421_write8(&lcd, 1, 0);
    /* Write to RAM */
    hd66421_write8(&lcd, 0, 0x04);
    hd66421_write8(&lcd, 1, 0xFF);
    /* X should have incremented */
    TEST_CHECK(lcd.x_addr == 1);
    TEST_CHECK(lcd.y_addr == 0);
}

static void test_display_off_renders_white(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Display is off by default (CONTROL1 DISP bit = 0) */
    const uint8_t *fb = hd66421_render(&lcd);
    for (int i = 0; i < HD66421_WIDTH * HD66421_HEIGHT; i++) {
        TEST_CHECK(fb[i] == 255);
        if (fb[i] != 255) break;
    }
}

static void test_display_on_renders_vram(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Enable display */
    hd66421_write8(&lcd, 0, 0x00); /* CONTROL1 */
    hd66421_write8(&lcd, 1, 0x40); /* DISP=1 */
    /* Write a known pattern to first VRAM byte (bottom-left of screen) */
    lcd.vram[0] = 0xE4; /* Pixels: 11, 10, 01, 00 */
    const uint8_t *fb = hd66421_render(&lcd);
    /* Bottom-left row is y=99, rendering goes bottom-to-top */
    int base = 99 * HD66421_WIDTH;
    /* Palette 0=0→white(231), 1=10→gray, 2=20→dark gray, 3=31→black(0) */
    /* pixel 0: palette[3]=31 → 0 */
    TEST_CHECK_(fb[base] == 0, "pixel 0: expected 0, got %d", fb[base]);
    /* pixel 3: palette[0]=0 → 231 (white) */
    TEST_CHECK_(fb[base + 3] > 200, "pixel 3: expected white (~231), got %d", fb[base + 3]);
}

static void test_y_overflow_wraps(void) {
    hd66421_t lcd;
    hd66421_init(&lcd);
    /* Set Y to 99 (last row) */
    hd66421_write8(&lcd, 0, 0x03);
    hd66421_write8(&lcd, 1, 99);
    /* Write to RAM → Y increments to 100 → wraps to 0 */
    hd66421_write8(&lcd, 0, 0x04);
    hd66421_write8(&lcd, 1, 0x42);
    TEST_CHECK(lcd.y_addr == 0);
}

TEST_LIST = {
    { "init_defaults",              test_init_defaults },
    { "vram_zeroed",                test_vram_zeroed },
    { "reg_index_write_read",       test_reg_index_write_read },
    { "x_y_addr_writes",           test_x_y_addr_writes },
    { "vram_write_read",           test_vram_write_read },
    { "auto_increment_y",          test_auto_increment_y },
    { "auto_increment_x",          test_auto_increment_x },
    { "display_off_renders_white", test_display_off_renders_white },
    { "display_on_renders_vram",   test_display_on_renders_vram },
    { "y_overflow_wraps",          test_y_overflow_wraps },
    { NULL, NULL }
};
