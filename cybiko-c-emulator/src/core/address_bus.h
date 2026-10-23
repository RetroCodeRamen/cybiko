#ifndef CYBIKO_ADDRESS_BUS_H
#define CYBIKO_ADDRESS_BUS_H

#include "types.h"
#include "memory.h"
#include "hd66421.h"
#include "timer8.h"
#include "timer16.h"
#include "rtc.h"
#include "speaker.h"
#include "keyboard.h"
#include "sci2_xt.h"

/* Forward declare CPU */
typedef struct h8s_cpu h8s_cpu_t;

typedef struct address_bus {
    /* Memory regions */
    memory_t  boot_rom;       /* 32KB, read-only */
    memory_t  external_ram;   /* 2MB, writable */
    memory_t  flash_rom;      /* 512KB, read-only */
    memory_t  on_chip_ram;    /* 10KB (0x2400 bytes), writable */

    /* Peripherals (pointers, owned by emulator) */
    hd66421_t  *lcd;
    timer8_t   *timer8[2];
    timer16_t  *timer16[6];
    rtc_t      *rtc;
    speaker_t  *speaker;
    keyboard_t *keyboard;
    h8s_cpu_t  *cpu;

    /* I/O state */
    uint8_t tstr;            /* Timer start register (0xFFFFC0) */
    uint8_t ier;             /* Interrupt enable register (0xFFFF2E) */
    uint8_t isr;             /* Interrupt status register (0xFFFF2F) */
    uint8_t adcsr;           /* ADC status register */
    uint8_t adcr;            /* ADC control register */

    /* DMA registers (XT) */
    uint8_t dma_regs[32];    /* 0xFFFEE0-0xFFFEFF */
    uint8_t dma_ctrl[8];     /* 0xFFFF00-0xFFFF07 */

    /* Serial output buffers */
    char    sci_output[3][256];
    int     sci_output_pos[3];

    /* DMA completion tracking */
    int     dma_completion_delay;
    int     dma_completion_vector;

    /* Debug log counters */
    int     unmapped_log_count;
    int     io_fallthrough_log;
    int     dma_debug_log;
    bool    in_dma;  /* true during DMA transfers */

    /* SCI2 radio DTC. Idle until bus_attach_radio(). */
    cybiko_sci2_xt_t sci2;
} address_bus_t;

void     bus_init(address_bus_t *bus);
void     bus_free(address_bus_t *bus);

uint8_t  bus_read8(address_bus_t *bus, uint32_t address);
uint16_t bus_read16(address_bus_t *bus, uint32_t address);
uint32_t bus_read32(address_bus_t *bus, uint32_t address);
void     bus_write8(address_bus_t *bus, uint32_t address, uint8_t value);
void     bus_write16(address_bus_t *bus, uint32_t address, uint16_t value);
void     bus_write32(address_bus_t *bus, uint32_t address, uint32_t value);

void     bus_tick_dma_completion(address_bus_t *bus);
void     bus_tick_rtc(address_bus_t *bus);

/* Attach the XT radio co-processor. NULL restores the hard-wired SCI2. */
void     bus_attach_radio(address_bus_t *bus, struct cybiko_radio_coproc *coproc);
/* Advance SCI2 by guest cycles. No work when no co-processor is attached. */
void     bus_tick_sci2(address_bus_t *bus, int cycles);

#endif
