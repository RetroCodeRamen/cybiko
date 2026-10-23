#include "core/memory.h"
#include <stdlib.h>
#include <string.h>

void memory_init(memory_t *mem, size_t size, bool writable) {
    mem->data     = (uint8_t *)calloc(size, 1);
    mem->size     = size;
    mem->writable = writable;
}

void memory_free(memory_t *mem) {
    free(mem->data);
    mem->data = NULL;
    mem->size = 0;
}

void memory_load(memory_t *mem, const uint8_t *src, size_t src_len, size_t offset) {
    if (offset >= mem->size) {
        return;
    }
    size_t avail = mem->size - offset;
    size_t count = src_len < avail ? src_len : avail;
    memcpy(mem->data + offset, src, count);
}

/* ---------- Big-endian reads ---------- */

uint8_t memory_read8(const memory_t *mem, uint32_t offset) {
    if (offset >= mem->size) {
        return 0;
    }
    return mem->data[offset];
}

uint16_t memory_read16(const memory_t *mem, uint32_t offset) {
    if (offset + 1 >= mem->size) {
        return 0;
    }
    return (uint16_t)((mem->data[offset] << 8) | mem->data[offset + 1]);
}

uint32_t memory_read32(const memory_t *mem, uint32_t offset) {
    if (offset + 3 >= mem->size) {
        return 0;
    }
    return ((uint32_t)mem->data[offset]     << 24) |
           ((uint32_t)mem->data[offset + 1] << 16) |
           ((uint32_t)mem->data[offset + 2] <<  8) |
           ((uint32_t)mem->data[offset + 3]);
}

/* ---------- Big-endian writes ---------- */

void memory_write8(memory_t *mem, uint32_t offset, uint8_t value) {
    if (!mem->writable || offset >= mem->size) {
        return;
    }
    mem->data[offset] = value;
}

void memory_write16(memory_t *mem, uint32_t offset, uint16_t value) {
    if (!mem->writable || offset + 1 >= mem->size) {
        return;
    }
    mem->data[offset]     = (uint8_t)(value >> 8);
    mem->data[offset + 1] = (uint8_t)(value);
}

void memory_write32(memory_t *mem, uint32_t offset, uint32_t value) {
    if (!mem->writable || offset + 3 >= mem->size) {
        return;
    }
    mem->data[offset]     = (uint8_t)(value >> 24);
    mem->data[offset + 1] = (uint8_t)(value >> 16);
    mem->data[offset + 2] = (uint8_t)(value >>  8);
    mem->data[offset + 3] = (uint8_t)(value);
}

uint8_t *memory_raw(memory_t *mem) {
    return mem->data;
}
