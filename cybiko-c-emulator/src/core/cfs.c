#include "core/cfs.h"
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/* CRC16 algorithm from MAME cybikoxt.cpp */
uint16_t cfs_compute_crc16(const uint8_t *data, int length)
{
    uint32_t val = 0;
    for (int i = 0; i < length; i++) {
        val = (val ^ data[i] ^ i) << 1;
        val = val | ((val >> 16) & 0x0001);
    }
    return (uint16_t)(val & 0xFFFF);
}

/* Return pointer to the start of page N within the image */
static uint8_t *page_ptr(cfs_image_t *img, int page)
{
    return &img->data[page * CFS_PAGE_SIZE];
}

static const uint8_t *page_ptr_const(const cfs_image_t *img, int page)
{
    return &img->data[page * CFS_PAGE_SIZE];
}

/* Return pointer to the 256-byte block data within a page */
static uint8_t *block_data(cfs_image_t *img, int page)
{
    return page_ptr(img, page) + 2;
}

static const uint8_t *block_data_const(const cfs_image_t *img, int page)
{
    return page_ptr_const(img, page) + 2;
}

/* Write big-endian CRC16 at the start of a page */
static void page_write_crc(cfs_image_t *img, int page)
{
    const uint8_t *blk = block_data_const(img, page);
    uint16_t crc = cfs_compute_crc16(blk, CFS_BLOCK_DATA_SIZE);
    uint8_t *p = page_ptr(img, page);
    p[0] = (uint8_t)(crc >> 8);
    p[1] = (uint8_t)(crc & 0xFF);
}

/* Write a big-endian u16 into a buffer */
static void write_be16(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val >> 8);
    dst[1] = (uint8_t)(val & 0xFF);
}

/* Write a big-endian u32 into a buffer */
static void write_be32(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val >> 24);
    dst[1] = (uint8_t)((val >> 16) & 0xFF);
    dst[2] = (uint8_t)((val >> 8) & 0xFF);
    dst[3] = (uint8_t)(val & 0xFF);
}

/* Read a big-endian u16 from a buffer */
static uint16_t read_be16(const uint8_t *src)
{
    return (uint16_t)((src[0] << 8) | src[1]);
}

/* File block page index: block_index 0..1999 maps to page 5..2004 */
static int file_block_page(int block_index)
{
    return CFS_BOOT_BLOCKS + block_index;
}

/* ------------------------------------------------------------------ */
/*  cfs_format                                                        */
/* ------------------------------------------------------------------ */

void cfs_format(cfs_image_t *img)
{
    /* 1. Fill entire image with 0xFF */
    memset(img->data, 0xFF, CFS_IMAGE_SIZE);

    /* 2. Boot blocks (pages 0-4): already all 0xFF, CRC = 0xFFFF
     *    which is 0xFF 0xFF big-endian -- already correct from memset. */

    /* 3. File blocks (pages 5-2004): mark as unused, compute CRC */
    for (int i = 0; i < CFS_FILE_BLOCKS; i++) {
        int page = file_block_page(i);
        uint8_t *blk = block_data(img, page);
        blk[0] = 0x7F;  /* clear bit 7 = unused */
        page_write_crc(img, page);
    }
}

/* ------------------------------------------------------------------ */
/*  cfs_add_file                                                      */
/* ------------------------------------------------------------------ */

bool cfs_add_file(cfs_image_t *img, const char *name,
                  const uint8_t *file_data, size_t len)
{
    /* Calculate blocks needed */
    int blocks_needed;
    if (len <= CFS_FIRST_BLOCK_CAPACITY) {
        blocks_needed = 1;
    } else {
        size_t remaining = len - CFS_FIRST_BLOCK_CAPACITY;
        blocks_needed = 1 + (int)((remaining + CFS_CONT_BLOCK_CAPACITY - 1)
                                  / CFS_CONT_BLOCK_CAPACITY);
    }

    /* Find free blocks */
    int free_blocks[2048];
    int free_count = 0;

    for (int i = 0; i < CFS_FILE_BLOCKS && free_count < blocks_needed; i++) {
        const uint8_t *blk = block_data_const(img, file_block_page(i));
        if ((blk[0] & 0x80) == 0) {
            free_blocks[free_count++] = i;
        }
    }

    if (free_count < blocks_needed) {
        return false;  /* not enough space */
    }

    /* Timestamp: seconds since 1900-01-01 */
    uint32_t timestamp = (uint32_t)(time(NULL)) + 2208988800U;

    /* Filename length (capped at 58 chars to fit bytes 7-64) */
    size_t name_len = strlen(name);
    if (name_len > 58) {
        name_len = 58;
    }

    size_t data_offset = 0;  /* how much file data we have written */

    for (int n = 0; n < blocks_needed; n++) {
        int block_idx = free_blocks[n];
        int page = file_block_page(block_idx);
        uint8_t *blk = block_data(img, page);

        /* Clear block to 0x00 so unused bytes are deterministic */
        memset(blk, 0x00, CFS_BLOCK_DATA_SIZE);

        /* Common header: bytes 0-5 */
        blk[0] = 0x80;                              /* flags: BLOCK_USED */
        /* [1] data_count -- filled below */
        write_be16(&blk[2], (uint16_t)free_blocks[0]); /* file_id = first block index */
        write_be16(&blk[4], (uint16_t)n);               /* part_id */

        if (n == 0) {
            /* Part 0: file header block */
            blk[6] = 0x20;  /* type marker */

            /* Filename at bytes 7-64 (null-terminated) */
            memcpy(&blk[7], name, name_len);
            blk[7 + name_len] = '\0';

            /* Timestamp at bytes 74-77 */
            write_be32(&blk[74], timestamp);

            /* File data at bytes 78-255 (up to 178 bytes) */
            size_t chunk = len;
            if (chunk > CFS_FIRST_BLOCK_CAPACITY) {
                chunk = CFS_FIRST_BLOCK_CAPACITY;
            }
            if (chunk > 0) {
                memcpy(&blk[78], file_data, chunk);
            }
            blk[1] = (uint8_t)chunk;
            data_offset = chunk;
        } else {
            /* Continuation block: data at bytes 6-255 (up to 250 bytes) */
            size_t remaining = len - data_offset;
            size_t chunk = remaining;
            if (chunk > CFS_CONT_BLOCK_CAPACITY) {
                chunk = CFS_CONT_BLOCK_CAPACITY;
            }
            if (chunk > 0) {
                memcpy(&blk[6], &file_data[data_offset], chunk);
            }
            blk[1] = (uint8_t)chunk;
            data_offset += chunk;
        }

        /* Update page CRC */
        page_write_crc(img, page);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  cfs_list_files                                                    */
/* ------------------------------------------------------------------ */

int cfs_list_files(const cfs_image_t *img, char names[][64], int max_files)
{
    int count = 0;

    for (int i = 0; i < CFS_FILE_BLOCKS && count < max_files; i++) {
        const uint8_t *blk = block_data_const(img, file_block_page(i));

        /* Check: used block with part_id == 0 */
        if ((blk[0] & 0x80) && read_be16(&blk[4]) == 0) {
            /* Extract filename from bytes 7-64 */
            memset(names[count], 0, 64);
            memcpy(names[count], &blk[7], 58);
            names[count][58] = '\0';  /* ensure termination */
            count++;
        }
    }

    return count;
}

static bool block_name_is(const uint8_t *blk, const char *name)
{
    char stored[59];
    memcpy(stored, blk + 7, 58);
    stored[58] = '\0';
    return strcmp(stored, name) == 0;
}

static int find_part(const cfs_image_t *img, uint16_t file_id, uint16_t part)
{
    for (int i = 0; i < CFS_FILE_BLOCKS; i++) {
        int page = file_block_page(i);
        const uint8_t *blk = block_data_const(img, page);
        if ((blk[0] & 0x80) == 0)
            continue;
        if (read_be16(&blk[2]) != file_id || read_be16(&blk[4]) != part)
            continue;
        const uint8_t *page_bytes = page_ptr_const(img, page);
        uint16_t stored = read_be16(page_bytes);
        uint16_t crc = cfs_compute_crc16(blk, CFS_BLOCK_DATA_SIZE);
        if (stored != crc)
            return -2;
        return i;
    }
    return -1;
}

bool cfs_has_file(const cfs_image_t *img, const char *name)
{
    if (!img || !name)
        return false;
    for (int i = 0; i < CFS_FILE_BLOCKS; i++) {
        const uint8_t *blk = block_data_const(img, file_block_page(i));
        if ((blk[0] & 0x80) && read_be16(&blk[4]) == 0 && block_name_is(blk, name))
            return true;
    }
    return false;
}

bool cfs_read_file(const cfs_image_t *img, const char *name,
                   uint8_t *dst, size_t cap, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!img || !name)
        return false;

    int head = -1;
    for (int i = 0; i < CFS_FILE_BLOCKS; i++) {
        const uint8_t *blk = block_data_const(img, file_block_page(i));
        if ((blk[0] & 0x80) && read_be16(&blk[4]) == 0 && block_name_is(blk, name)) {
            head = i;
            break;
        }
    }
    if (head < 0)
        return false;

    uint16_t file_id = read_be16(block_data_const(img, file_block_page(head)) + 2);
    size_t total = 0;
    for (uint16_t part = 0;; part++) {
        int idx = find_part(img, file_id, part);
        if (idx == -2)
            return false;
        if (idx < 0)
            break;
        total += block_data_const(img, file_block_page(idx))[1];
    }
    if (out_len)
        *out_len = total;
    if (!dst)
        return true;
    if (cap < total)
        return false;

    size_t at = 0;
    for (uint16_t part = 0;; part++) {
        int idx = find_part(img, file_id, part);
        if (idx < 0)
            break;
        const uint8_t *blk = block_data_const(img, file_block_page(idx));
        size_t chunk = blk[1];
        const uint8_t *src = blk + (part == 0 ? 78 : 6);
        if (chunk)
            memcpy(dst + at, src, chunk);
        at += chunk;
    }
    return true;
}

bool cfs_poke_file(cfs_image_t *img, const char *name, size_t off, uint8_t value)
{
    if (!img || !name)
        return false;
    int head = -1;
    for (int i = 0; i < CFS_FILE_BLOCKS; i++) {
        const uint8_t *blk = block_data_const(img, file_block_page(i));
        if ((blk[0] & 0x80) && read_be16(&blk[4]) == 0 && block_name_is(blk, name)) {
            head = i;
            break;
        }
    }
    if (head < 0)
        return false;
    uint16_t file_id = read_be16(block_data_const(img, file_block_page(head)) + 2);
    size_t at = 0;
    for (uint16_t part = 0;; part++) {
        int idx = find_part(img, file_id, part);
        if (idx == -2 || idx < 0)
            return false;
        int page = file_block_page(idx);
        uint8_t *blk = block_data(img, page);
        size_t chunk = blk[1];
        if (off < at + chunk) {
            size_t local = off - at;
            uint8_t *src = blk + (part == 0 ? 78 : 6);
            src[local] = value;
            page_write_crc(img, page);
            return true;
        }
        at += chunk;
    }
}
