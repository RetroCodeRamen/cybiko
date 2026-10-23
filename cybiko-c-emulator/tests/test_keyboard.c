#include "acutest.h"
#include "core/keyboard.h"
#include <string.h>

static void test_init_zeroes_columns(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    for (int i = 0; i < KEYBOARD_MAX_COLUMNS; i++) {
        TEST_CHECK(kb.columns[i] == 0);
    }
    TEST_CHECK(kb.num_columns == 10);
}

static void test_set_key_sets_bit(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    keyboard_set_key(&kb, 3, 0x0010, true);
    TEST_CHECK(kb.columns[3] == 0x0010);
}

static void test_set_key_clears_bit(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    keyboard_set_key(&kb, 3, 0x0010, true);
    keyboard_set_key(&kb, 3, 0x0010, false);
    TEST_CHECK(kb.columns[3] == 0);
}

static void test_read_no_keys_returns_ffff(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    /* Address 0xE00000 → word_offset 0 → all columns selected (bit i == 0) */
    uint16_t val = keyboard_read(&kb, 0xE00000);
    TEST_CHECK(val == 0xFFFF);
}

static void test_read_with_key_pressed(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    /* Set a key on column 0 */
    keyboard_set_key(&kb, 0, 0x0004, true);
    /* Read with column 0 selected (bit 0 = 0): address 0xE00000 has word_offset 0 */
    uint16_t val = keyboard_read(&kb, 0xE00000);
    /* Column 0 selected (bit 0=0) → data &= ~0x0004 → 0xFFFB */
    TEST_CHECK_(val == 0xFFFB, "expected 0xFFFB, got 0x%04X", val);
}

static void test_read_unselected_column(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    keyboard_set_key(&kb, 0, 0x0004, true);
    /* Word offset with bit 0 = 1 (column 0 deselected) but bit 1 = 0 (column 1 selected):
       address = 0xE00000 + (1 * 2) = 0xE00002 → word_offset = 1, bit 0 = 1 */
    uint16_t val = keyboard_read(&kb, 0xE00002);
    /* Column 0 not selected (bit 0=1), column 1 selected (bit 1=0) but has no keys */
    TEST_CHECK_(val == 0xFFFF, "expected 0xFFFF, got 0x%04X", val);
}

static void test_multiple_columns_or(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    keyboard_set_key(&kb, 0, 0x0001, true);
    keyboard_set_key(&kb, 1, 0x0002, true);
    /* Word offset 0 → all 10 columns selected → both keys visible */
    uint16_t val = keyboard_read(&kb, 0xE00000);
    TEST_CHECK_((val & 0x0001) == 0, "bit 0 should be clear (key pressed)");
    TEST_CHECK_((val & 0x0002) == 0, "bit 1 should be clear (key pressed)");
}

static void test_set_matrix(void) {
    keyboard_t kb;
    keyboard_init(&kb, 10);
    uint16_t matrix[15] = {0};
    matrix[0] = 0x1234;
    matrix[5] = 0xABCD;
    matrix[9] = 0x5678;
    keyboard_set_matrix(&kb, matrix);
    TEST_CHECK(kb.columns[0] == 0x1234);
    TEST_CHECK(kb.columns[5] == 0xABCD);
    TEST_CHECK(kb.columns[9] == 0x5678);
}

TEST_LIST = {
    { "init_zeroes_columns",      test_init_zeroes_columns },
    { "set_key_sets_bit",         test_set_key_sets_bit },
    { "set_key_clears_bit",       test_set_key_clears_bit },
    { "read_no_keys_returns_ffff", test_read_no_keys_returns_ffff },
    { "read_with_key_pressed",    test_read_with_key_pressed },
    { "read_unselected_column",   test_read_unselected_column },
    { "multiple_columns_or",      test_multiple_columns_or },
    { "set_matrix",               test_set_matrix },
    { NULL, NULL }
};
