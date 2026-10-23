#include "acutest.h"
#include "core/cfs.h"
#include <string.h>
#include <stdlib.h>

static void test_crc16_deterministic(void) {
    uint8_t data[256];
    memset(data, 0, sizeof(data));
    uint16_t crc1 = cfs_compute_crc16(data, 256);
    uint16_t crc2 = cfs_compute_crc16(data, 256);
    TEST_CHECK(crc1 == crc2);
}

static void test_crc16_all_ff(void) {
    uint8_t data[256];
    memset(data, 0xFF, sizeof(data));
    uint16_t crc = cfs_compute_crc16(data, 256);
    TEST_CHECK(crc == cfs_compute_crc16(data, 256));
}

static void test_crc16_sensitive_to_data(void) {
    uint8_t a[256], b[256];
    memset(a, 0x00, sizeof(a));
    memcpy(b, a, sizeof(a));
    b[128] = 0x01; /* Change one byte */
    TEST_CHECK(cfs_compute_crc16(a, 256) != cfs_compute_crc16(b, 256));
}

static void test_format_empty(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);
    char names[10][64];
    int count = cfs_list_files(img, names, 10);
    TEST_CHECK(count == 0);
    free(img);
}

static void test_add_small_file(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);
    uint8_t data[] = "Hello, Cybiko!";
    bool ok = cfs_add_file(img, "test.txt", data, sizeof(data) - 1);
    TEST_CHECK(ok);
    char names[10][64];
    int count = cfs_list_files(img, names, 10);
    TEST_CHECK(count == 1);
    TEST_CHECK(strcmp(names[0], "test.txt") == 0);
    free(img);
}

static void test_add_multiple_files(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);
    uint8_t data1[] = "File 1";
    uint8_t data2[] = "File 2";
    uint8_t data3[] = "File 3";
    TEST_CHECK(cfs_add_file(img, "one.txt", data1, sizeof(data1) - 1));
    TEST_CHECK(cfs_add_file(img, "two.txt", data2, sizeof(data2) - 1));
    TEST_CHECK(cfs_add_file(img, "three.txt", data3, sizeof(data3) - 1));
    char names[10][64];
    int count = cfs_list_files(img, names, 10);
    TEST_CHECK(count == 3);
    free(img);
}

static void test_add_large_file(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);
    /* 500 bytes needs 1 header block (178 bytes) + 2 continuation blocks (250+72) */
    uint8_t data[500];
    for (int i = 0; i < 500; i++) data[i] = (uint8_t)(i & 0xFF);
    bool ok = cfs_add_file(img, "big.bin", data, 500);
    TEST_CHECK(ok);
    char names[10][64];
    int count = cfs_list_files(img, names, 10);
    TEST_CHECK(count == 1);
    TEST_CHECK(strcmp(names[0], "big.bin") == 0);
    free(img);
}

static void test_exactly_first_block_capacity(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);
    uint8_t data[CFS_FIRST_BLOCK_CAPACITY];
    memset(data, 0xAA, sizeof(data));
    bool ok = cfs_add_file(img, "exact.bin", data, CFS_FIRST_BLOCK_CAPACITY);
    TEST_CHECK(ok);
    char names[10][64];
    int count = cfs_list_files(img, names, 10);
    TEST_CHECK(count == 1);
    free(img);
}

static void test_filesystem_full(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);
    /* Fill all 2000 file blocks with single-block files */
    uint8_t data[1] = {0x42};
    int added = 0;
    char name[64];
    for (int i = 0; i < CFS_FILE_BLOCKS; i++) {
        snprintf(name, sizeof(name), "f%d", i);
        if (!cfs_add_file(img, name, data, 1)) break;
        added++;
    }
    TEST_CHECK(added == CFS_FILE_BLOCKS);
    /* Next add should fail */
    bool ok = cfs_add_file(img, "overflow.txt", data, 1);
    TEST_CHECK(!ok);
    free(img);
}

static void test_app_roundtrip(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);

    uint8_t app[400];
    for (int i = 0; i < 400; i++)
        app[i] = (uint8_t)(0xA0 + (i % 17));
    TEST_CHECK(cfs_add_file(img, "demo.app", app, sizeof app));
    TEST_CHECK(cfs_has_file(img, "demo.app"));
    TEST_CHECK(!cfs_has_file(img, "missing.app"));

    size_t got_len = 0;
    TEST_CHECK(cfs_read_file(img, "demo.app", NULL, 0, &got_len));
    TEST_CHECK(got_len == sizeof app);

    uint8_t back[400];
    memset(back, 0, sizeof back);
    TEST_CHECK(cfs_read_file(img, "demo.app", back, sizeof back, &got_len));
    TEST_CHECK(got_len == sizeof app);
    TEST_CHECK(memcmp(back, app, sizeof app) == 0);

    uint8_t tiny[4] = {1, 2, 3, 4};
    TEST_CHECK(!cfs_read_file(img, "demo.app", tiny, sizeof tiny, &got_len));
    TEST_CHECK(got_len == sizeof app);

    uint8_t note[] = "keep";
    TEST_CHECK(cfs_add_file(img, "note.app", note, sizeof note - 1));
    uint8_t note_back[8];
    TEST_CHECK(cfs_read_file(img, "note.app", note_back, sizeof note_back, &got_len));
    TEST_CHECK(got_len == sizeof note - 1);
    TEST_CHECK(memcmp(note_back, note, got_len) == 0);
    TEST_CHECK(cfs_read_file(img, "demo.app", back, sizeof back, &got_len));
    TEST_CHECK(memcmp(back, app, sizeof app) == 0);

    int page = CFS_BOOT_BLOCKS;
    img->data[page * CFS_PAGE_SIZE + 2] ^= 0x01;
    TEST_CHECK(!cfs_read_file(img, "demo.app", back, sizeof back, &got_len));
    free(img);
}

static void test_poke_file_byte(void) {
    cfs_image_t *img = calloc(1, sizeof(cfs_image_t));
    TEST_ASSERT(img != NULL);
    cfs_format(img);
    uint8_t body[80];
    memset(body, 0x11, sizeof body);
    body[12] = 0x00;
    body[41] = 0x00;
    TEST_CHECK(cfs_add_file(img, "settings.dat", body, sizeof body));
    TEST_CHECK(cfs_poke_file(img, "settings.dat", 12, 0x01));
    TEST_CHECK(cfs_poke_file(img, "settings.dat", 41, 0x01));
    uint8_t back[80];
    size_t n = 0;
    TEST_CHECK(cfs_read_file(img, "settings.dat", back, sizeof back, &n));
    TEST_CHECK(n == sizeof body);
    TEST_CHECK(back[12] == 0x01);
    TEST_CHECK(back[41] == 0x01);
    TEST_CHECK(back[0] == 0x11);
    TEST_CHECK(!cfs_poke_file(img, "missing.dat", 0, 1));
    free(img);
}

TEST_LIST = {
    { "crc16_deterministic",         test_crc16_deterministic },
    { "crc16_all_ff",                test_crc16_all_ff },
    { "crc16_sensitive_to_data",     test_crc16_sensitive_to_data },
    { "format_empty",                test_format_empty },
    { "add_small_file",              test_add_small_file },
    { "add_multiple_files",          test_add_multiple_files },
    { "add_large_file",              test_add_large_file },
    { "exactly_first_block_capacity", test_exactly_first_block_capacity },
    { "filesystem_full",             test_filesystem_full },
    { "app bytes round-trip",        test_app_roundtrip },
    { "poke one file byte",          test_poke_file_byte },
    { NULL, NULL }
};
