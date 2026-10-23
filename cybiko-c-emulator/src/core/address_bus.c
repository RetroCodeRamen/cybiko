/*
 * Cybiko address bus - memory-mapped I/O router (XT configuration).
 *
 * Ported from AddressBus.java. Routes CPU reads/writes to the correct
 * peripheral based on the 24-bit address.
 *
 * XT Memory map:
 *   0x000000-0x03FFFF  Boot ROM (32KB mirrored)
 *   0x100000-0x100001  LCD controller (HD66421)
 *   0x200000-0x200003  USB controller (stub)
 *   0x400000-0x5FFFFF  External RAM (2MB)
 *   0x600000-0x7FFFFF  Flash ROM (512KB mirrored)
 *   0xE00000-0xEFFFFF  Keyboard matrix
 *   0xFFDC00-0xFFFFFF  On-chip RAM & I/O registers
 */
#include "core/address_bus.h"
#include "core/h8s_cpu.h"
#include <stdio.h>
#include <string.h>

/* --- Address region enumeration --- */
typedef enum {
    REGION_BOOT_ROM,
    REGION_LCD,
    REGION_USB,
    REGION_EXT_RAM,
    REGION_FLASH,
    REGION_KEYBOARD,
    REGION_ON_CHIP,
    REGION_UNMAPPED
} bus_region_t;

/* --- SSR bit definitions --- */
#define SSR_TDRE 0x80
#define SSR_RDRF 0x40
#define SSR_TEND 0x04

/* --- XT memory map constants --- */
#define BOOT_ROM_MIRROR_END    0x03FFFF
#define LCD_BASE               0x100000
#define LCD_END                0x100001
#define USB_BASE               0x200000
#define USB_END                0x200003
#define EXT_RAM_BASE           0x400000
#define EXT_RAM_END            0x5FFFFF
#define EXT_RAM_SIZE           0x200000
#define FLASH_BASE             0x600000
#define FLASH_END              0x7FFFFF
#define FLASH_SIZE             0x80000
#define KEYBOARD_BASE          0xE00000
#define KEYBOARD_END           0xEFFFFF
#define ON_CHIP_RAM_BASE       0xFFDC00

/* --- Forward declarations --- */
static uint8_t  read_on_chip8(address_bus_t *bus, uint32_t address);
static uint16_t read_on_chip16(address_bus_t *bus, uint32_t address);
static void     write_on_chip8(address_bus_t *bus, uint32_t address, uint8_t value);
static void     write_on_chip16(address_bus_t *bus, uint32_t address, uint16_t value);
static void     execute_dma_transfer(address_bus_t *bus, int channel);

/* --- Address decoding --- */

static bus_region_t decode_region(uint32_t address)
{
    if (address <= BOOT_ROM_MIRROR_END)
        return REGION_BOOT_ROM;

    if (address >= LCD_BASE && address <= LCD_END)
        return REGION_LCD;

    if (address >= USB_BASE && address <= USB_END)
        return REGION_USB;

    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END)
        return REGION_EXT_RAM;

    if (address >= FLASH_BASE && address <= FLASH_END)
        return REGION_FLASH;

    if (address >= KEYBOARD_BASE && address <= KEYBOARD_END)
        return REGION_KEYBOARD;

    if (address >= ON_CHIP_RAM_BASE && address <= 0xFFFFFF)
        return REGION_ON_CHIP;

    return REGION_UNMAPPED;
}

static uint32_t ext_ram_offset(uint32_t address)
{
    return (address - EXT_RAM_BASE) % EXT_RAM_SIZE;
}

static uint32_t flash_offset(uint32_t address)
{
    return (address - FLASH_BASE) % FLASH_SIZE;
}

static uint32_t on_chip_offset(uint32_t address)
{
    return address - ON_CHIP_RAM_BASE;
}

static void log_unmapped(address_bus_t *bus, const char *op, uint32_t address)
{
    if (bus->unmapped_log_count < 200) {
        uint32_t pc = bus->cpu ? bus->cpu->pc : 0;
        fprintf(stderr, "Bus: unmapped %s at 0x%06X (PC=0x%06X)\n", op, address, pc);
        bus->unmapped_log_count++;
        if (bus->unmapped_log_count == 200) {
            fprintf(stderr, "Bus: suppressing further unmapped warnings (200 reached)\n");
        }
    }
}

/* --- Timer16 routing helpers --- */

static int route_timer16_read8(address_bus_t *bus, uint32_t address)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5])
        return timer16_read8(bus->timer16[5], address - 0xFFFEA0);
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4])
        return timer16_read8(bus->timer16[4], address - 0xFFFE90);
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3])
        return timer16_read8(bus->timer16[3], address - 0xFFFE80);
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0])
        return timer16_read8(bus->timer16[0], address - 0xFFFFD0);
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1])
        return timer16_read8(bus->timer16[1], address - 0xFFFFE0);
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2])
        return timer16_read8(bus->timer16[2], address - 0xFFFFF0);
    return -1;
}

static int route_timer16_read16(address_bus_t *bus, uint32_t address)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5])
        return timer16_read16(bus->timer16[5], address - 0xFFFEA0);
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4])
        return timer16_read16(bus->timer16[4], address - 0xFFFE90);
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3])
        return timer16_read16(bus->timer16[3], address - 0xFFFE80);
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0])
        return timer16_read16(bus->timer16[0], address - 0xFFFFD0);
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1])
        return timer16_read16(bus->timer16[1], address - 0xFFFFE0);
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2])
        return timer16_read16(bus->timer16[2], address - 0xFFFFF0);
    return -1;
}

static bool route_timer16_write8(address_bus_t *bus, uint32_t address, uint8_t value)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5]) {
        timer16_write8(bus->timer16[5], address - 0xFFFEA0, value); return true;
    }
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4]) {
        timer16_write8(bus->timer16[4], address - 0xFFFE90, value); return true;
    }
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3]) {
        timer16_write8(bus->timer16[3], address - 0xFFFE80, value); return true;
    }
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0]) {
        timer16_write8(bus->timer16[0], address - 0xFFFFD0, value); return true;
    }
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1]) {
        timer16_write8(bus->timer16[1], address - 0xFFFFE0, value); return true;
    }
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2]) {
        timer16_write8(bus->timer16[2], address - 0xFFFFF0, value); return true;
    }
    return false;
}

static bool route_timer16_write16(address_bus_t *bus, uint32_t address, uint16_t value)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5]) {
        timer16_write16(bus->timer16[5], address - 0xFFFEA0, value); return true;
    }
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4]) {
        timer16_write16(bus->timer16[4], address - 0xFFFE90, value); return true;
    }
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3]) {
        timer16_write16(bus->timer16[3], address - 0xFFFE80, value); return true;
    }
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0]) {
        timer16_write16(bus->timer16[0], address - 0xFFFFD0, value); return true;
    }
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1]) {
        timer16_write16(bus->timer16[1], address - 0xFFFFE0, value); return true;
    }
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2]) {
        timer16_write16(bus->timer16[2], address - 0xFFFFF0, value); return true;
    }
    return false;
}

/* --- Timer8 routing helpers --- */

static uint8_t read_timer8(address_bus_t *bus, int bus_off)
{
    timer8_t *ch = (bus_off % 2 == 0) ? bus->timer8[0] : bus->timer8[1];
    int reg_off = bus_off & ~1;
    return ch ? timer8_read(ch, reg_off) : 0;
}

static void write_timer8(address_bus_t *bus, int bus_off, uint8_t value)
{
    timer8_t *ch = (bus_off % 2 == 0) ? bus->timer8[0] : bus->timer8[1];
    int reg_off = bus_off & ~1;
    if (ch) timer8_write(ch, reg_off, value);
}

/* --- Init / Free --- */

void bus_init(address_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
    bus->adcr = 0x7E;
    cybiko_sci2_init(&bus->sci2);
}

void bus_free(address_bus_t *bus)
{
    memory_free(&bus->boot_rom);
    memory_free(&bus->external_ram);
    memory_free(&bus->flash_rom);
    memory_free(&bus->on_chip_ram);
}

/* --- Read operations --- */

uint8_t bus_read8(address_bus_t *bus, uint32_t address)
{
    address &= 0xFFFFFF;
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        return memory_read8(&bus->boot_rom, address & 0x7FFF);
    case REGION_LCD:
        return bus->lcd ? hd66421_read8(bus->lcd, address & 1) : 0;
    case REGION_USB:
        return 0;
    case REGION_EXT_RAM:
        return memory_read8(&bus->external_ram, ext_ram_offset(address));
    case REGION_FLASH:
        return memory_read8(&bus->flash_rom, flash_offset(address));
    case REGION_KEYBOARD: {
        if (!bus->keyboard) return 0xFF;
        uint16_t kb_val = keyboard_read(bus->keyboard, address & ~1);
        return (address & 1) == 0 ? (kb_val >> 8) & 0xFF : kb_val & 0xFF;
    }
    case REGION_ON_CHIP:
        return read_on_chip8(bus, address);
    case REGION_UNMAPPED:
        log_unmapped(bus, "read8", address);
        return 0;
    }
    return 0;
}

uint16_t bus_read16(address_bus_t *bus, uint32_t address)
{
    address &= 0xFFFFFF;
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        return memory_read16(&bus->boot_rom, address & 0x7FFF);
    case REGION_LCD:
        if (!bus->lcd) return 0;
        return (hd66421_read8(bus->lcd, 0) << 8) | hd66421_read8(bus->lcd, 1);
    case REGION_USB:
        return 0;
    case REGION_EXT_RAM:
        return memory_read16(&bus->external_ram, ext_ram_offset(address));
    case REGION_FLASH:
        return memory_read16(&bus->flash_rom, flash_offset(address));
    case REGION_KEYBOARD:
        if (!bus->keyboard) return 0xFFFF;
        return keyboard_read(bus->keyboard, address);
    case REGION_ON_CHIP:
        return read_on_chip16(bus, address);
    case REGION_UNMAPPED:
        log_unmapped(bus, "read16", address);
        return 0;
    }
    return 0;
}

uint32_t bus_read32(address_bus_t *bus, uint32_t address)
{
    return ((uint32_t)bus_read16(bus, address) << 16) | bus_read16(bus, address + 2);
}

/* --- Write operations --- */

void bus_write8(address_bus_t *bus, uint32_t address, uint8_t value)
{
    address &= 0xFFFFFF;
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        /* Read-only, ignore */
        break;
    case REGION_LCD:
        if (bus->lcd) hd66421_write8(bus->lcd, address & 1, value);
        break;
    case REGION_USB:
        /* Stub, ignore */
        break;
    case REGION_EXT_RAM:
        memory_write8(&bus->external_ram, ext_ram_offset(address), value);
        break;
    case REGION_FLASH:
        memory_write8(&bus->flash_rom, flash_offset(address), value);
        break;
    case REGION_KEYBOARD:
        /* Read-only, ignore */
        break;
    case REGION_ON_CHIP:
        write_on_chip8(bus, address, value);
        break;
    case REGION_UNMAPPED:
        log_unmapped(bus, "write8", address);
        break;
    }
}

void bus_write16(address_bus_t *bus, uint32_t address, uint16_t value)
{
    address &= 0xFFFFFF;
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        break;
    case REGION_LCD:
        if (bus->lcd) {
            hd66421_write8(bus->lcd, 0, value >> 8);
            hd66421_write8(bus->lcd, 1, value & 0xFF);
        }
        break;
    case REGION_USB:
        break;
    case REGION_EXT_RAM:
        memory_write16(&bus->external_ram, ext_ram_offset(address), value);
        break;
    case REGION_FLASH:
        memory_write16(&bus->flash_rom, flash_offset(address), value);
        break;
    case REGION_KEYBOARD:
        break;
    case REGION_ON_CHIP:
        write_on_chip16(bus, address, value);
        break;
    case REGION_UNMAPPED:
        log_unmapped(bus, "write16", address);
        break;
    }
}

void bus_write32(address_bus_t *bus, uint32_t address, uint32_t value)
{
    bus_write16(bus, address, (uint16_t)(value >> 16));
    bus_write16(bus, address + 2, (uint16_t)(value & 0xFFFF));
}

/* --- On-chip 8-bit read --- */

static uint8_t read_on_chip8(address_bus_t *bus, uint32_t address)
{
    /* Timer16 channels (non-contiguous, check first) */
    int t16 = route_timer16_read8(bus, address);
    if (t16 >= 0) return (uint8_t)t16;

    /* Timer8 Channel 0/1: 0xFFFFB0-0xFFFFB9 */
    if (address >= 0xFFFFB0 && address <= 0xFFFFB9) {
        return read_timer8(bus, address - 0xFFFFB0);
    }

    /* TSTR - Timer Start Register */
    if (address == 0xFFFFC0) return bus->tstr;
    /* TSYR stub */
    if (address == 0xFFFFC1) return 0;

    /* IER / ISR */
    if (address == 0xFFFF2E) return bus->ier;
    if (address == 0xFFFF2F) return bus->isr;

    /* SCI2 SCR/SSR/RDR come from the co-processor once one is attached.
     * Until then SCI2 SSR stays transmit-ready, which is what CyOS boot sees. */
    if (bus->sci2.bound) {
        uint8_t sci_val;
        if (cybiko_sci2_read(&bus->sci2, address, &sci_val))
            return sci_val;
    }

    /* SCI SSR registers - always report transmit ready */
    if (address == 0xFFFF7C) return SSR_TDRE | SSR_TEND;  /* SCI0 SSR */
    if (address == 0xFFFF84) return SSR_TDRE | SSR_TEND;  /* SCI1 SSR */
    if (address == 0xFFFF85) return 0;                     /* SCI1 RDR */
    if (address == 0xFFFF8C) return SSR_TDRE | SSR_TEND;  /* SCI2 SSR */

    /* DMA registers (XT) */
    if (address >= 0xFFFEE0 && address <= 0xFFFEFF) {
        return bus->dma_regs[address - 0xFFFEE0];
    }
    if (address >= 0xFFFF00 && address <= 0xFFFF07) {
        return bus->dma_ctrl[address - 0xFFFF00];
    }

    /* System control stubs */
    if (address == 0xFFFF38) return 0;  /* SBYCR */
    if (address == 0xFFFF39) return 0;  /* SYSCR */
    if (address == 0xFFFF3B) return 0;  /* MDCR */

    /* Port Input Data Registers (0xFFFF50-0xFFFF5E) */
    if (address >= 0xFFFF50 && address <= 0xFFFF5E) {
        /* Port A input (0xFFFF59) - power status for XT */
        if (address == 0xFFFF59) return 0xC0;
        /* Port F input (0xFFFF5E) - RTC SDA on bit 6 */
        if (address == 0xFFFF5E) {
            if (bus->rtc && rtc_sda_r(bus->rtc))
                return 0xFF;
            else
                return 0xFF & ~0x40;
        }
        return memory_read8(&bus->on_chip_ram, on_chip_offset(address));
    }

    /* Port Data Registers (0xFFFF60-0xFFFF6F) */
    if (address >= 0xFFFF60 && address <= 0xFFFF6F) {
        /* Port A DR (0xFFFF69) - power status for XT */
        if (address == 0xFFFF69) return 0xC0;
        /* Port F DR (0xFFFF6E) - RTC SDA on bit 6 */
        if (address == 0xFFFF6E) {
            if (bus->rtc && rtc_sda_r(bus->rtc))
                return 0xFF;
            else
                return 0xFF & ~0x40;
        }
        return memory_read8(&bus->on_chip_ram, on_chip_offset(address));
    }

    /* Watchdog stub */
    if (address >= 0xFFFFBC && address <= 0xFFFFBF) return 0;

    /* ADC registers (0xFFFF90-0xFFFF99) */
    if (address >= 0xFFFF90 && address <= 0xFFFF99) {
        /* ADC data registers (0xFFFF90-0xFFFF97) */
        if (address <= 0xFFFF97) {
            /* XT: even=0xCC, odd=0x00 */
            return (address % 2 == 0) ? 0xCC : 0x00;
        }
        if (address == 0xFFFF98) return bus->adcsr;
        return bus->adcr;  /* 0xFFFF99 */
    }

    /* Default: on-chip RAM fallthrough */
    return memory_read8(&bus->on_chip_ram, on_chip_offset(address));
}

/* --- On-chip 16-bit read --- */

static uint16_t read_on_chip16(address_bus_t *bus, uint32_t address)
{
    /* Timer16 routing first */
    int t16 = route_timer16_read16(bus, address);
    if (t16 >= 0) return (uint16_t)t16;

    /* I/O register region: compose from two 8-bit reads */
    if (address >= 0xFFFE00) {
        return (read_on_chip8(bus, address) << 8) | read_on_chip8(bus, address + 1);
    }

    /* Below 0xFFFE00: direct on-chip RAM read */
    return memory_read16(&bus->on_chip_ram, on_chip_offset(address));
}

/* --- On-chip 8-bit write --- */

static void write_on_chip8(address_bus_t *bus, uint32_t address, uint8_t value)
{
    /* Timer16 routing (check first, non-contiguous addresses) */
    if (route_timer16_write8(bus, address, value)) return;

    /* Timer8 Channel 0/1: 0xFFFFB0-0xFFFFB9 */
    if (address >= 0xFFFFB0 && address <= 0xFFFFB9) {
        write_timer8(bus, address - 0xFFFFB0, value);
        return;
    }

    /* TSTR - Timer Start Register (0xFFFFC0) */
    if (address == 0xFFFFC0) {
        bus->tstr = value & 0xFF;
        if (bus->timer16[0]) timer16_set_enabled(bus->timer16[0], (bus->tstr & 0x01) != 0);
        if (bus->timer16[1]) timer16_set_enabled(bus->timer16[1], (bus->tstr & 0x02) != 0);
        if (bus->timer16[2]) timer16_set_enabled(bus->timer16[2], (bus->tstr & 0x04) != 0);
        if (bus->timer16[3]) timer16_set_enabled(bus->timer16[3], (bus->tstr & 0x08) != 0);
        if (bus->timer16[4]) timer16_set_enabled(bus->timer16[4], (bus->tstr & 0x10) != 0);
        if (bus->timer16[5]) timer16_set_enabled(bus->timer16[5], (bus->tstr & 0x20) != 0);
        return;
    }

    /* IER */
    if (address == 0xFFFF2E) { bus->ier = value & 0xFF; return; }
    /* ISR - write-0-to-clear */
    if (address == 0xFFFF2F) { bus->isr &= value; return; }

    /* SCI registers (0xFFFF78-0xFFFF8E) */
    if (address >= 0xFFFF78 && address <= 0xFFFF8E) {
        int channel, reg;
        if (address >= 0xFFFF88) {
            channel = 2; reg = address - 0xFFFF88;
        } else if (address >= 0xFFFF80) {
            channel = 1; reg = address - 0xFFFF80;
        } else {
            channel = 0; reg = address - 0xFFFF78;
        }
        /* TDR - Transmit Data Register (reg offset 3) */
        if (reg == 3) {
            char c = (char)(value & 0x7F);
            if (c >= 0x20 || c == '\n') {
                /* printable or newline */
            } else {
                c = '.';
            }
            if (bus->sci_output_pos[channel] < 255) {
                bus->sci_output[channel][bus->sci_output_pos[channel]++] = c;
                bus->sci_output[channel][bus->sci_output_pos[channel]] = '\0';
            }
        }
        if (channel == 2 && bus->sci2.bound)
            cybiko_sci2_on_reg(&bus->sci2, reg, value);
        /* Store to on-chip RAM regardless */
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

    /* Port DDR registers (0xFFFEB0-0xFFFEBF) */
    if (address >= 0xFFFEB0 && address <= 0xFFFEBF) {
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

    /* Port 1 write (0xFFFF60) - bit 3 drives speaker */
    if (address == 0xFFFF60) {
        int level = (value & 0x08) != 0 ? 1 : 0;
        if (bus->speaker) {
            speaker_set_level(bus->speaker, level);
        }
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

    /* Port F write (0xFFFF6E) - I2C RTC bit-banging */
    if (address == 0xFFFF6E) {
        if (bus->rtc) {
            rtc_scl_w(bus->rtc, (value & 0x02) != 0);
            /* XT: SDA is inverted - writing the bit = pull SDA low */
            rtc_sda_w(bus->rtc, (value & 0x40) == 0);
        }
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

    /* Port data register writes (0xFFFF60-0xFFFF6F) - catch remaining ports */
    if (address >= 0xFFFF60 && address <= 0xFFFF6F) {
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

    /* DMA registers (XT) */
    if (address >= 0xFFFEE0 && address <= 0xFFFEFF) {
        bus->dma_regs[address - 0xFFFEE0] = value & 0xFF;
        return;
    }
    if (address >= 0xFFFF00 && address <= 0xFFFF07) {
        int idx = address - 0xFFFF00;
        uint8_t old_val = bus->dma_ctrl[idx];
        bus->dma_ctrl[idx] = value & 0xFF;

        /* DMA trigger edge detection on index 7 */
        if (idx == 7) {
            bool ch0_triggered = false;
            /* Channel 0: bit 4 (0x10) or bit 5 (0x20) rising edge */
            if ((value & 0x10) && !(old_val & 0x10)) {
                execute_dma_transfer(bus, 0);
                ch0_triggered = true;
            }
            if (!ch0_triggered && (value & 0x20) && !(old_val & 0x20)) {
                execute_dma_transfer(bus, 0);
            }
            bool ch1_triggered = false;
            /* Channel 1: bit 6 (0x40) or bit 7 (0x80) rising edge */
            if ((value & 0x40) && !(old_val & 0x40)) {
                execute_dma_transfer(bus, 1);
                ch1_triggered = true;
            }
            if (!ch1_triggered && (value & 0x80) && !(old_val & 0x80)) {
                execute_dma_transfer(bus, 1);
            }
        }
        return;
    }

    /* System control, watchdog stubs */
    if (address == 0xFFFF38 || address == 0xFFFF39) return;
    if (address >= 0xFFFFBC && address <= 0xFFFFBF) return;

    /* ADC registers */
    if (address >= 0xFFFF90 && address <= 0xFFFF99) {
        if (address == 0xFFFF98) {
            if (value & 0x20) {
                bus->adcsr = (value & 0x7F) | 0x80;  /* auto-set ADF */
            } else {
                bus->adcsr = value & 0x7F;
            }
        } else if (address == 0xFFFF99) {
            bus->adcr = value & 0xFF;
        }
        return;
    }

    /* DTCERF. Store first so executeSci2Dtc reads the new bits.
     * The handler clears its own bit with a raw poke, then this write's
     * value (not the poked register) decides whether 0x03 is queued. */
    if (address == CYBIKO_SCI2_DTCERF && bus->sci2.bound) {
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        cybiko_sci2_on_dtcerf(&bus->sci2, value);
        return;
    }

    /* Default: write to on-chip RAM */
    memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
}

/* --- On-chip 16-bit write --- */

static void write_on_chip16(address_bus_t *bus, uint32_t address, uint16_t value)
{
    /* Timer16 routing first */
    if (route_timer16_write16(bus, address, value)) return;

    /* I/O register region: split into two 8-bit writes */
    if (address >= 0xFFFE00) {
        write_on_chip8(bus, address, (value >> 8) & 0xFF);
        write_on_chip8(bus, address + 1, value & 0xFF);
        return;
    }

    /* Below 0xFFFE00: direct on-chip RAM write */
    memory_write16(&bus->on_chip_ram, on_chip_offset(address), value);
}

/* --- DMA transfer (XT) --- */

static void execute_dma_transfer(address_bus_t *bus, int channel)
{
    int base = channel * 16;
    uint32_t src_addr = ((uint32_t)bus->dma_regs[base] << 24)
                      | ((uint32_t)bus->dma_regs[base + 1] << 16)
                      | ((uint32_t)bus->dma_regs[base + 2] << 8)
                      | bus->dma_regs[base + 3];
    uint32_t dst_addr = ((uint32_t)bus->dma_regs[base + 8] << 24)
                      | ((uint32_t)bus->dma_regs[base + 9] << 16)
                      | ((uint32_t)bus->dma_regs[base + 10] << 8)
                      | bus->dma_regs[base + 11];
    int count = (bus->dma_regs[base + 6] << 8) | bus->dma_regs[base + 7];

    uint8_t dmacr_h = bus->dma_ctrl[2 + channel * 2];
    uint8_t dmacr_l = bus->dma_ctrl[3 + channel * 2];
    bool mode16    = (dmacr_h & 0x80) != 0;  /* Bit 15: transfer size      */
    bool src_dec   = (dmacr_h & 0x40) != 0;  /* Bit 14: source decrement   */
    bool src_inc   = (dmacr_h & 0x20) != 0;  /* Bit 13: source inc enable  */
    bool dst_dec   = (dmacr_l & 0x40) != 0;  /* Bit  6: dest decrement     */
    bool dst_inc   = (dmacr_l & 0x20) != 0;  /* Bit  5: dest inc enable    */

    int step = mode16 ? 2 : 1;
    int src_step = src_inc ? (src_dec ? -step : step) : 0;
    int dst_step = dst_inc ? (dst_dec ? -step : step) : 0;

    src_addr &= 0xFFFFFF;
    dst_addr &= 0xFFFFFF;

    if (count == 0) {
        if (channel == 1) bus->dma_ctrl[7] &= ~0xC0;
        else bus->dma_ctrl[7] &= ~0x30;
        return;
    }

    bus->in_dma = true;
    if (mode16) {
        for (int i = 0; i < count; i++) {
            uint16_t val = bus_read16(bus, src_addr);
            bus_write16(bus, dst_addr, val);
            src_addr = (src_addr + src_step) & 0xFFFFFF;
            dst_addr = (dst_addr + dst_step) & 0xFFFFFF;
        }
    } else {
        for (int i = 0; i < count; i++) {
            uint8_t val = bus_read8(bus, src_addr);
            bus_write8(bus, dst_addr, val);
            src_addr = (src_addr + src_step) & 0xFFFFFF;
            dst_addr = (dst_addr + dst_step) & 0xFFFFFF;
        }
    }

    bus->in_dma = false;

    /* Clear trigger bits and count */
    if (channel == 1) bus->dma_ctrl[7] &= ~0xC0;
    else bus->dma_ctrl[7] &= ~0x30;
    bus->dma_regs[base + 6] = 0;
    bus->dma_regs[base + 7] = 0;
}

/* --- Tick functions --- */

void bus_tick_dma_completion(address_bus_t *bus)
{
    if (bus->dma_completion_delay > 0) {
        bus->dma_completion_delay--;
        if (bus->dma_completion_delay == 0 && bus->cpu) {
            h8s_cpu_request_interrupt(bus->cpu, bus->dma_completion_vector);
        }
    }
}

void bus_tick_rtc(address_bus_t *bus)
{
    if (bus->rtc) {
        rtc_tick(bus->rtc);
    }
}

static uint8_t sci2_mem_read(void *ctx, uint32_t address)
{
    return bus_read8((address_bus_t *)ctx, address);
}

static void sci2_mem_poke(void *ctx, uint32_t address, uint8_t value)
{
    address_bus_t *bus = ctx;
    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END) {
        memory_write8(&bus->external_ram, address - EXT_RAM_BASE, value);
        return;
    }
    if (address >= ON_CHIP_RAM_BASE)
        memory_write8(&bus->on_chip_ram, address - ON_CHIP_RAM_BASE, value);
}

static void sci2_raise(void *ctx, int vector)
{
    address_bus_t *bus = ctx;
    if (bus->cpu)
        h8s_cpu_request_interrupt(bus->cpu, vector);
}

void bus_attach_radio(address_bus_t *bus, cybiko_radio_coproc_t *coproc)
{
    cybiko_sci2_bind(&bus->sci2, coproc, sci2_mem_read, sci2_mem_poke, sci2_raise, bus);
}

void bus_tick_sci2(address_bus_t *bus, int cycles)
{
    if (!bus->sci2.bound || cycles <= 0)
        return;
    cybiko_sci2_advance(&bus->sci2, cycles);
}
