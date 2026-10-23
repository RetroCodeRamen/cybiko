# Cybiko C Emulator - Design Document

## Overview

A C port of the Cybiko Java emulator targeting the Cybiko Xtreme (H8S/2323). The emulator is designed as a portable core library with a HAL (Hardware Abstraction Layer) callback interface, enabling it to run on both a Linux desktop (via SDL2) and an ESP32-S3 (T-Deck Pro, via PlatformIO) from the same codebase.

**First milestone:** Boot CyOS in an SDL2 window on Linux.

## Architecture

**Approach:** Monolithic core with HAL callbacks.

A single `cybiko_emu_t` struct owns all emulator state (CPU, memory, bus, LCD, timers, etc.). Platform interaction happens exclusively through a `cybiko_hal_t` struct of function pointers passed at creation time. The core has zero platform dependencies beyond standard C (stdint.h, string.h, stdlib.h).

## Target Hardware

**Cybiko Xtreme (XT) only:**

| Spec | Value |
|------|-------|
| CPU | Hitachi H8S/2323 |
| Clock | 18.432 MHz |
| Boot ROM | 32 KB (mirrored to 256KB) |
| External RAM | 2 MB |
| Flash ROM | 512 KB (memory-mapped) |
| On-chip RAM | 10 KB |
| LCD | 160x100, 2-bit grayscale (HD66421) |
| Keyboard | 15-column matrix |
| Timer8 | 2 channels |
| Timer16 | 6 channels |
| DMA | Yes (DTC) |
| Speaker | 1-bit digital, Port 1 bit 3 |
| RTC | PCF8593 via I2C (Port F) |

## Project Structure

```
cybiko-c-emulator/
├── CMakeLists.txt              # Linux build (links SDL2)
├── platformio.ini              # ESP32 build (future)
├── src/
│   ├── core/                   # Platform-independent emulator
│   │   ├── emulator.h          # Public API
│   │   ├── emulator.c          # Main orchestrator
│   │   ├── h8s_cpu.h/c         # H8S/2323 CPU core
│   │   ├── address_bus.h/c     # Memory-mapped I/O routing
│   │   ├── memory.h/c          # Byte-array memory regions
│   │   ├── hd66421.h/c         # LCD controller
│   │   ├── timer8.h/c          # 8-bit timers
│   │   ├── timer16.h/c         # 16-bit timers (TPU)
│   │   ├── rtc.h/c             # PCF8593 real-time clock
│   │   ├── speaker.h/c         # 1-bit speaker audio
│   │   ├── keyboard.h/c        # Keyboard matrix
│   │   └── cfs.h/c             # Cybiko File System
│   ├── hal/
│   │   └── hal.h               # HAL interface definition
│   └── linux/
│       └── main.c              # SDL2 desktop client
└── roms/                       # ROM files (gitignored)
```

## Public API (emulator.h)

```c
// HAL interface - platform provides these callbacks
typedef struct {
    void (*render_frame)(void *ctx, const uint8_t *pixels, int width, int height);
    void (*audio_output)(void *ctx, const uint8_t *samples, int count);
    void (*keyboard_poll)(void *ctx, uint16_t *matrix, int num_columns);
    void (*serial_output)(void *ctx, uint8_t byte);
    void *ctx;
} cybiko_hal_t;

// Lifecycle
cybiko_emu_t *cybiko_create(const cybiko_hal_t *hal);
void          cybiko_destroy(cybiko_emu_t *emu);

// ROM loading
bool cybiko_load_boot_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_flash_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_nvram(cybiko_emu_t *emu, const uint8_t *data, size_t len);

// Execution
void cybiko_reset(cybiko_emu_t *emu);
void cybiko_run_frame(cybiko_emu_t *emu);  // One 1/60s frame (~307,200 cycles)

// State
bool cybiko_is_running(const cybiko_emu_t *emu);
const uint8_t *cybiko_get_nvram(const cybiko_emu_t *emu, size_t *len);
```

## CPU Core (h8s_cpu)

Direct port of the Java H8SCpu.java (~2500 lines).

**Registers:**
- ER0-ER7: 32-bit general purpose (ER7 = SP)
- PC: 24-bit program counter
- CCR: 8-bit condition code (I, UI, H, U, N, Z, V, C)
- EXR: extended control register

**Register access macros:** `ER(n)`, `E(n)` (upper 16), `R(n)` (lower 16), `RH(n)` (high byte), `RL(n)` (low byte).

**Instruction decode:** Switch-based decoder matching Java's approach. Variable-length instructions (2-10 bytes). Big-endian memory.

**Interrupts:** Priority queue of pending vectors. Check after each instruction. Push PC (32-bit) + CCR (16-bit) to stack, jump to vector address (read from vector * 4).

**Debug:** Compile-time `CYBIKO_TRACE` flag for instruction-level tracing. PC histogram sampled every 1024 steps, dumped on exit.

## Memory Map (XT)

| Address Range | Region | Behavior |
|---|---|---|
| `0x000000-0x03FFFF` | Boot ROM | 32KB mirrored to 256KB, read-only |
| `0x100000-0x100001` | LCD | Index register / data register |
| `0x200000-0x200003` | USB | Stub (reads return 0) |
| `0x400000-0x5FFFFF` | External RAM | 2MB read/write |
| `0x600000-0x7FFFFF` | Flash ROM | 512KB mirrored across 2MB |
| `0xE00000-0xEFFFFF` | Keyboard | Matrix read via column addressing |
| `0xFFDC00-0xFFFFFF` | On-chip | RAM + I/O registers (10KB) |

## Peripherals

### HD66421 LCD Controller
- 18 registers (R0-R17) accessed via index/data ports
- 4000-byte VRAM (160x100 / 4 pixels per byte, 2 bits each)
- Drawing order: bottom-to-top (VRAM byte 0 = row 99)
- Palette, reverse, blink support
- Renders to 160x100 uint8_t array at frame end

### Timer8 (2 channels)
- 8-bit up-counter with prescaler (div 8, 64, 8192)
- Compare match A/B and overflow interrupts
- Ticked every CPU cycle

### Timer16 (6 channels, TPU)
- 16-bit counter with per-channel clock source
- Compare match and overflow interrupts
- Channel chaining (ch1, ch4)
- Ticked every CPU cycle through prescaler

### PCF8593 RTC
- I2C via Port F bit-bang (bit 1 = SCL, bit 6 = SDA)
- Ticked once per frame
- BCD time registers

### Speaker
- 1-bit output via Port 1 bit 3
- Generates 44100Hz 8-bit unsigned mono PCM
- Tracks level transitions, fills samples between them

### Keyboard
- 15-column matrix at 0xE00000-0xEFFFFF
- Each address bit selects a column, returns row data
- HAL callback fills matrix before each frame

### CFS (Cybiko File System)
- 524544-byte NVRAM images (2005 pages x 258 bytes + padding)
- Page: 2-byte CRC + 256-byte block
- File blocks: flags, file ID, part ID, filename, timestamp, data
- Load/save to file for persistence

## Linux SDL2 Client

- **Display:** SDL window at 3x scale (480x300), 4 grayscale colors mapped to RGB
- **Keyboard:** SDL keycodes mapped to Cybiko 15-column matrix
- **Audio:** SDL audio device, 44100Hz 8-bit mono
- **NVRAM:** Load from file at startup, save on exit
- **CLI:** `./cybiko-emu <boot.bin> <flash.bin> [--nvram file.nvram]`
- **Frame rate:** 60 FPS with SDL_Delay timing

## ROM Files

Located in the Java emulator project at:
- Boot ROM: `emulator/src/main/resources/cybikoxt/cyrom150.bin` (32KB)
- Flash ROM: `emulator/src/main/resources/cybikoxt/flash_v1358.bin` (512KB)

These will be symlinked or copied into the `roms/` directory (gitignored).

## Build Systems

**Linux (CMake):**
- C11 standard
- Links SDL2 (libsdl2-dev)
- Debug build includes CYBIKO_TRACE

**ESP32 (PlatformIO, future):**
- ESP32-S3 target (T-Deck Pro)
- Uses ESP-IDF framework via PlatformIO
- Display via SPI (ST7789), keyboard via I2C (0x55), audio via I2S
