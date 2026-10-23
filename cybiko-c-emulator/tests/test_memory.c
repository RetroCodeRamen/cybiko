#include "acutest.h"
#include "core/memory.h"
#include <string.h>

static void test_init_zeros_memory(void) {
    memory_t mem;
    memory_init(&mem, 256, true);
    for (int i = 0; i < 256; i++) {
        TEST_CHECK(mem.data[i] == 0);
    }
    TEST_CHECK(mem.size == 256);
    TEST_CHECK(mem.writable == true);
    memory_free(&mem);
}

static void test_init_readonly(void) {
    memory_t mem;
    memory_init(&mem, 128, false);
    TEST_CHECK(mem.size == 128);
    TEST_CHECK(mem.writable == false);
    memory_free(&mem);
}

static void test_read8_write8_roundtrip(void) {
    memory_t mem;
    memory_init(&mem, 256, true);
    memory_write8(&mem, 0, 0x42);
    memory_write8(&mem, 255, 0xAB);
    TEST_CHECK(memory_read8(&mem, 0) == 0x42);
    TEST_CHECK(memory_read8(&mem, 255) == 0xAB);
    memory_free(&mem);
}

static void test_write16_big_endian(void) {
    memory_t mem;
    memory_init(&mem, 256, true);
    memory_write16(&mem, 0, 0x1234);
    TEST_CHECK(memory_read8(&mem, 0) == 0x12);
    TEST_CHECK(memory_read8(&mem, 1) == 0x34);
    memory_free(&mem);
}

static void test_write32_big_endian(void) {
    memory_t mem;
    memory_init(&mem, 256, true);
    memory_write32(&mem, 0, 0xDEADBEEF);
    TEST_CHECK(memory_read8(&mem, 0) == 0xDE);
    TEST_CHECK(memory_read8(&mem, 1) == 0xAD);
    TEST_CHECK(memory_read8(&mem, 2) == 0xBE);
    TEST_CHECK(memory_read8(&mem, 3) == 0xEF);
    memory_free(&mem);
}

static void test_read16_roundtrip(void) {
    memory_t mem;
    memory_init(&mem, 256, true);
    memory_write16(&mem, 10, 0xCAFE);
    TEST_CHECK(memory_read16(&mem, 10) == 0xCAFE);
    memory_free(&mem);
}

static void test_read32_roundtrip(void) {
    memory_t mem;
    memory_init(&mem, 256, true);
    memory_write32(&mem, 20, 0x12345678);
    TEST_CHECK(memory_read32(&mem, 20) == 0x12345678);
    memory_free(&mem);
}

static void test_write_readonly_ignored(void) {
    memory_t mem;
    memory_init(&mem, 256, false);
    memory_write8(&mem, 0, 0xFF);
    TEST_CHECK(memory_read8(&mem, 0) == 0x00);
    memory_write16(&mem, 0, 0xFFFF);
    TEST_CHECK(memory_read16(&mem, 0) == 0x0000);
    memory_write32(&mem, 0, 0xFFFFFFFF);
    TEST_CHECK(memory_read32(&mem, 0) == 0x00000000);
    memory_free(&mem);
}

static void test_memory_load(void) {
    memory_t mem;
    memory_init(&mem, 256, false);
    uint8_t src[] = {0xAA, 0xBB, 0xCC, 0xDD};
    memory_load(&mem, src, 4, 10);
    TEST_CHECK(memory_read8(&mem, 10) == 0xAA);
    TEST_CHECK(memory_read8(&mem, 11) == 0xBB);
    TEST_CHECK(memory_read8(&mem, 12) == 0xCC);
    TEST_CHECK(memory_read8(&mem, 13) == 0xDD);
    TEST_CHECK(memory_read8(&mem, 9) == 0x00);
    TEST_CHECK(memory_read8(&mem, 14) == 0x00);
    memory_free(&mem);
}

static void test_memory_load_clamps(void) {
    memory_t mem;
    memory_init(&mem, 4, false);
    uint8_t src[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    memory_load(&mem, src, 6, 2);
    /* Only 2 bytes fit at offset 2 in a 4-byte memory */
    TEST_CHECK(memory_read8(&mem, 2) == 0x11);
    TEST_CHECK(memory_read8(&mem, 3) == 0x22);
    memory_free(&mem);
}

static void test_oob_read8_returns_zero(void) {
    memory_t mem;
    memory_init(&mem, 16, true);
    TEST_CHECK(memory_read8(&mem, 16) == 0);
    TEST_CHECK(memory_read8(&mem, 1000) == 0);
    memory_free(&mem);
}

static void test_oob_read16_returns_zero(void) {
    memory_t mem;
    memory_init(&mem, 16, true);
    TEST_CHECK(memory_read16(&mem, 15) == 0);
    TEST_CHECK(memory_read16(&mem, 100) == 0);
    memory_free(&mem);
}

static void test_oob_read32_returns_zero(void) {
    memory_t mem;
    memory_init(&mem, 16, true);
    TEST_CHECK(memory_read32(&mem, 13) == 0);
    TEST_CHECK(memory_read32(&mem, 100) == 0);
    memory_free(&mem);
}

static void test_oob_write_ignored(void) {
    memory_t mem;
    memory_init(&mem, 16, true);
    memory_write8(&mem, 16, 0xFF);
    memory_write16(&mem, 15, 0xFFFF);
    memory_write32(&mem, 13, 0xFFFFFFFF);
    /* No crash = pass; verify in-bounds data untouched */
    TEST_CHECK(memory_read8(&mem, 15) == 0);
    memory_free(&mem);
}

static void test_memory_raw(void) {
    memory_t mem;
    memory_init(&mem, 32, true);
    uint8_t *raw = memory_raw(&mem);
    TEST_CHECK(raw != NULL);
    raw[5] = 0x42;
    TEST_CHECK(memory_read8(&mem, 5) == 0x42);
    memory_free(&mem);
}

TEST_LIST = {
    { "init_zeros_memory",       test_init_zeros_memory },
    { "init_readonly",           test_init_readonly },
    { "read8_write8_roundtrip",  test_read8_write8_roundtrip },
    { "write16_big_endian",      test_write16_big_endian },
    { "write32_big_endian",      test_write32_big_endian },
    { "read16_roundtrip",        test_read16_roundtrip },
    { "read32_roundtrip",        test_read32_roundtrip },
    { "write_readonly_ignored",  test_write_readonly_ignored },
    { "memory_load",             test_memory_load },
    { "memory_load_clamps",      test_memory_load_clamps },
    { "oob_read8_returns_zero",  test_oob_read8_returns_zero },
    { "oob_read16_returns_zero", test_oob_read16_returns_zero },
    { "oob_read32_returns_zero", test_oob_read32_returns_zero },
    { "oob_write_ignored",       test_oob_write_ignored },
    { "memory_raw",              test_memory_raw },
    { NULL, NULL }
};
