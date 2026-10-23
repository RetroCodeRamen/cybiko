/*
 * AT45DB041 SPI DataFlash. See at45db041.h.
 */
#include "core/at45db041.h"
#include <string.h>

#define CMD_STATUS  0x57
#define CMD_READ    0x52
#define CMD_COMPARE 0x60
#define CMD_PROGRAM 0x82

#define STATUS_READY     0x80
#define STATUS_MISMATCH  0x40
#define DEVICE_ID_BITS   0x18   /* AT45DB041 density code in bits 5:3 */

void at45_init(at45db041_t *f, uint8_t *data) {
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->status = STATUS_READY;
    f->prog_pos = -1;
}

/* 11-bit page address and 9-bit byte address from the command bytes */
static int page_addr(const at45db041_t *f) {
    return ((f->cmd[1] & 0x0F) << 7) | ((f->cmd[2] & 0xFE) >> 1);
}

static int byte_addr(const at45db041_t *f) {
    return ((f->cmd[2] & 0x01) << 8) | f->cmd[3];
}

void at45_cs(at45db041_t *f, bool selected) {
    if (f->selected == selected)
        return;
    if (!selected) {
        /* CS high: a program command writes buffer 1 to its page */
        if (f->cmd_size >= 4 && f->cmd[0] == CMD_PROGRAM && f->data) {
            int page = page_addr(f);
            if (page < AT45_PAGES) {
                memcpy(f->data + page * AT45_PAGE_SIZE, f->buffer1, AT45_PAGE_SIZE);
                f->writes++;
            }
        }
        f->cmd_size = 0;
        f->output = false;
        f->io_page = NULL;
        f->io_pos = 0;
        f->prog_pos = -1;
    }
    f->selected = selected;
}

uint8_t at45_transfer(at45db041_t *f, uint8_t mosi) {
    if (!f->selected)
        return 0xFF;   /* idle bus, MISO pulled high */

    if (f->output) {
        if (!f->io_page)
            return f->status;
        uint8_t v = f->io_page[f->io_pos];
        f->io_pos = (f->io_pos + 1) % AT45_PAGE_SIZE;   /* wraps within the page */
        return v;
    }

    if (f->cmd_size < 8) {
        f->cmd[f->cmd_size++] = mosi;
        switch (f->cmd[0]) {
        case CMD_STATUS:
            if (f->cmd_size == 1) {
                f->status = (f->status & 0xC7) | DEVICE_ID_BITS;
                f->output = true;
                f->io_page = NULL;
                f->cmd_size = 8;
            }
            break;
        case CMD_READ:
            /* 4 command bytes and 4 don't-care bytes, then page data */
            if (f->cmd_size == 8 && f->data) {
                int page = page_addr(f);
                if (page >= AT45_PAGES) page = AT45_PAGES - 1;
                f->io_page = f->data + page * AT45_PAGE_SIZE;
                f->io_pos = byte_addr(f) % AT45_PAGE_SIZE;
                f->output = true;
            }
            break;
        case CMD_COMPARE:
            if (f->cmd_size == 4 && f->data) {
                int page = page_addr(f);
                bool match = page < AT45_PAGES &&
                    memcmp(f->data + page * AT45_PAGE_SIZE, f->buffer1, AT45_PAGE_SIZE) == 0;
                if (match) f->status &= ~STATUS_MISMATCH;
                else       f->status |= STATUS_MISMATCH;
                f->cmd_size = 8;
            }
            break;
        case CMD_PROGRAM:
            if (f->cmd_size == 4) {
                memset(f->buffer1, 0xFF, AT45_PAGE_SIZE);
                f->prog_pos = byte_addr(f) % AT45_PAGE_SIZE;
                f->cmd_size = 8;   /* data follows; stay in input mode */
            }
            break;
        }
    } else if (f->prog_pos >= 0) {
        f->buffer1[f->prog_pos] = mosi;
        f->prog_pos = (f->prog_pos + 1) % AT45_PAGE_SIZE;
    }
    return 0;
}
