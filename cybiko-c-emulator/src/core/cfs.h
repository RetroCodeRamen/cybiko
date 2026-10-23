#ifndef CYBIKO_CFS_H
#define CYBIKO_CFS_H

#include "types.h"

#define CFS_PAGE_COUNT      2005
#define CFS_PAGE_SIZE       258
#define CFS_PADDING         7254
#define CFS_IMAGE_SIZE      (CFS_PAGE_COUNT * CFS_PAGE_SIZE + CFS_PADDING)  // 524544
#define CFS_BOOT_BLOCKS     5
#define CFS_FILE_BLOCKS     2000
#define CFS_BLOCK_DATA_SIZE 256
#define CFS_FIRST_BLOCK_CAPACITY  178   // 256 - 78
#define CFS_CONT_BLOCK_CAPACITY   250   // 256 - 6

typedef struct {
    uint8_t data[CFS_IMAGE_SIZE];
} cfs_image_t;

void     cfs_format(cfs_image_t *img);
bool     cfs_add_file(cfs_image_t *img, const char *name, const uint8_t *file_data, size_t len);
bool     cfs_has_file(const cfs_image_t *img, const char *name);
/* Copy one file out. out_len receives the size. dst may be NULL to query size. */
bool     cfs_read_file(const cfs_image_t *img, const char *name,
                       uint8_t *dst, size_t cap, size_t *out_len);
/* Change one byte of an existing file and refresh that page's CRC. */
bool     cfs_poke_file(cfs_image_t *img, const char *name, size_t off, uint8_t value);
int      cfs_list_files(const cfs_image_t *img, char names[][64], int max_files);
uint16_t cfs_compute_crc16(const uint8_t *data, int length);

#endif
