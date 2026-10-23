#ifndef CYBIKO_MEMORY_H
#define CYBIKO_MEMORY_H

#include "types.h"

typedef struct {
    uint8_t *data;
    size_t   size;
    bool     writable;
} memory_t;

void     memory_init(memory_t *mem, size_t size, bool writable);
void     memory_free(memory_t *mem);
void     memory_load(memory_t *mem, const uint8_t *src, size_t src_len, size_t offset);

uint8_t  memory_read8(const memory_t *mem, uint32_t offset);
uint16_t memory_read16(const memory_t *mem, uint32_t offset);
uint32_t memory_read32(const memory_t *mem, uint32_t offset);

void     memory_write8(memory_t *mem, uint32_t offset, uint8_t value);
void     memory_write16(memory_t *mem, uint32_t offset, uint16_t value);
void     memory_write32(memory_t *mem, uint32_t offset, uint32_t value);

uint8_t *memory_raw(memory_t *mem);

#endif
