#include "acutest.h"
#include "tdeck_keymap.h"

static void test_letters_and_shift(void) {
    tdeck_key_event_t ev;
    TEST_CHECK(tdeck_map_ascii('p', &ev));
    TEST_CHECK(ev.col == 9 && ev.mask == 0x0010);
    TEST_CHECK(!ev.needs_fn && !ev.needs_shift);

    TEST_CHECK(tdeck_map_ascii('P', &ev));
    TEST_CHECK(ev.needs_shift);
    TEST_CHECK(ev.col == 9);

    TEST_CHECK(tdeck_map_ascii('a', &ev));
    TEST_CHECK(ev.col == 3 && ev.mask == 0x0004);
}

static void test_fn_digits(void) {
    tdeck_key_event_t ev;
    TEST_CHECK(tdeck_map_ascii('1', &ev));
    TEST_CHECK(ev.needs_fn);
    TEST_CHECK(ev.col == TDECK_FN_COL && ev.mask == TDECK_FN_MASK);
    TEST_CHECK(ev.fn_col == 3 && ev.fn_mask == 0x0002); /* Q */

    TEST_CHECK(tdeck_map_ascii('0', &ev));
    TEST_CHECK(ev.fn_col == 9 && ev.fn_mask == 0x0010); /* P */
}

static void test_controls_and_unmapped(void) {
    tdeck_key_event_t ev;
    TEST_CHECK(tdeck_map_ascii('\n', &ev));
    TEST_CHECK(ev.col == 4 && ev.mask == 0x0008);
    TEST_CHECK(tdeck_map_ascii(' ', &ev));
    TEST_CHECK(ev.col == 4 && ev.mask == 0x0040);
    TEST_CHECK(tdeck_map_ascii(0x1B, &ev));
    TEST_CHECK(ev.col == 5 && ev.mask == 0x0400);
    TEST_CHECK(!tdeck_map_ascii('`', &ev));
    TEST_CHECK(!ev.found);
}

static void test_trackball_constants(void) {
    TEST_CHECK(TDECK_PAD_UP_COL == 6 && TDECK_PAD_UP_MASK == 0x0800);
    TEST_CHECK(TDECK_PAD_DOWN_MASK == 0x2000);
    TEST_CHECK(TDECK_PAD_LEFT_MASK == 0x4000);
    TEST_CHECK(TDECK_PAD_RIGHT_MASK == 0x1000);
    TEST_CHECK(TDECK_SELECT_COL == 4 && TDECK_SELECT_MASK == 0x0010);
    TEST_CHECK(TDECK_SHIFT_COL == 8 && TDECK_FN_COL == 7);
}

TEST_LIST = {
    { "letters and shift", test_letters_and_shift },
    { "fn digit layer", test_fn_digits },
    { "controls and unmapped", test_controls_and_unmapped },
    { "trackball matrix", test_trackball_constants },
    { NULL, NULL }
};
