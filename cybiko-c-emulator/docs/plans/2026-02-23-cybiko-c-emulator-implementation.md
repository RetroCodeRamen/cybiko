# Cybiko C Emulator - Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Port the Cybiko Java emulator to C, targeting the Cybiko Xtreme (H8S/2323), with an SDL2 Linux client that boots CyOS.

**Architecture:** Monolithic core library (`cybiko_emu_t`) with HAL callback interface. Platform-independent core in `src/core/`, HAL definition in `src/hal/`, SDL2 client in `src/linux/`. All core code uses only standard C11.

**Tech Stack:** C11, CMake, SDL2 (Linux client)

**Reference:** Java emulator at `/home/dan/Projects/cybiko/cybiko-java-emulator/emulator/src/main/java/com/github/daberkow/`

---

### Task 1: Project Scaffolding

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/core/types.h`
- Create: `src/hal/hal.h`
- Create: `src/core/emulator.h`
- Create: `.gitignore`

**Step 1: Create directory structure**

```bash
mkdir -p src/core src/hal src/linux roms
```

**Step 2: Create `.gitignore`**

```gitignore
build/
roms/*.bin
roms/*.nvram
*.o
*.d
.cache/
compile_commands.json
```

**Step 3: Create `src/core/types.h`**

Common type definitions used throughout the core:

```c
#ifndef CYBIKO_TYPES_H
#define CYBIKO_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#endif // CYBIKO_TYPES_H
```

**Step 4: Create `src/hal/hal.h`**

```c
#ifndef CYBIKO_HAL_H
#define CYBIKO_HAL_H

#include "../core/types.h"

typedef struct {
    void (*render_frame)(void *ctx, const uint8_t *pixels, int width, int height);
    void (*audio_output)(void *ctx, const uint8_t *samples, int count);
    void (*keyboard_poll)(void *ctx, uint16_t *matrix, int num_columns);
    void (*serial_output)(void *ctx, uint8_t byte);
    void *ctx;
} cybiko_hal_t;

#endif // CYBIKO_HAL_H
```

**Step 5: Create `src/core/emulator.h`**

```c
#ifndef CYBIKO_EMULATOR_H
#define CYBIKO_EMULATOR_H

#include "types.h"
#include "../hal/hal.h"

// Forward declaration - actual struct defined in emulator.c
typedef struct cybiko_emu cybiko_emu_t;

// Lifecycle
cybiko_emu_t *cybiko_create(const cybiko_hal_t *hal);
void          cybiko_destroy(cybiko_emu_t *emu);

// ROM loading
bool cybiko_load_boot_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_flash_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_nvram(cybiko_emu_t *emu, const uint8_t *data, size_t len);

// Execution
void cybiko_reset(cybiko_emu_t *emu);
void cybiko_run_frame(cybiko_emu_t *emu);

// State
bool cybiko_is_running(const cybiko_emu_t *emu);
const uint8_t *cybiko_get_nvram(const cybiko_emu_t *emu, size_t *len);

// Constants
#define CYBIKO_LCD_WIDTH   160
#define CYBIKO_LCD_HEIGHT  100
#define CYBIKO_CLOCK_HZ    18432000
#define CYBIKO_FPS         60
#define CYBIKO_CYCLES_PER_FRAME (CYBIKO_CLOCK_HZ / CYBIKO_FPS)
#define CYBIKO_KEYBOARD_COLUMNS 15

#endif // CYBIKO_EMULATOR_H
```

**Step 6: Create `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(cybiko-emulator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Core emulator library (no platform dependencies)
add_library(cybiko_core STATIC
    src/core/memory.c
    src/core/hd66421.c
    src/core/timer8.c
    src/core/timer16.c
    src/core/rtc.c
    src/core/speaker.c
    src/core/keyboard.c
    src/core/h8s_cpu.c
    src/core/address_bus.c
    src/core/cfs.c
    src/core/emulator.c
)
target_include_directories(cybiko_core PUBLIC src)

# Optional trace support
option(CYBIKO_TRACE "Enable CPU instruction tracing" OFF)
if(CYBIKO_TRACE)
    target_compile_definitions(cybiko_core PUBLIC CYBIKO_TRACE=1)
endif()

# Linux SDL2 client
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    add_executable(cybiko-emu src/linux/main.c)
    target_link_libraries(cybiko-emu cybiko_core SDL2::SDL2)
else()
    # Fallback for systems where SDL2 doesn't export CMake targets
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    add_executable(cybiko-emu src/linux/main.c)
    target_link_libraries(cybiko-emu cybiko_core ${SDL2_LIBRARIES})
    target_include_directories(cybiko-emu PRIVATE ${SDL2_INCLUDE_DIRS})
endif()
```

**Step 7: Create stub source files so CMake can configure**

Create minimal stub `.c` files for each source listed in CMakeLists.txt. Each should just include its own header (once those exist). This lets us build incrementally. Example for each:

```c
// src/core/memory.c
#include "memory.h"
```

(Same pattern for all .c files. Headers will be created in subsequent tasks.)

**Step 8: Commit**

```bash
git add -A && git commit -m "feat: project scaffolding with CMake, HAL, and directory structure"
```

---

### Task 2: Memory Module

**Files:**
- Create: `src/core/memory.h`
- Create: `src/core/memory.c`

**Reference:** `Memory.java`

Port the simple byte-array memory implementation. Big-endian read/write. Bounds checking returns 0 for out-of-range reads, silently discards out-of-range writes.

**Step 1: Create `src/core/memory.h`**

```c
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

#endif // CYBIKO_MEMORY_H
```

**Step 2: Implement `src/core/memory.c`**

```c
#include "memory.h"
#include <stdlib.h>
#include <string.h>

void memory_init(memory_t *mem, size_t size, bool writable) {
    mem->data = calloc(1, size);
    mem->size = size;
    mem->writable = writable;
}

void memory_free(memory_t *mem) {
    free(mem->data);
    mem->data = NULL;
    mem->size = 0;
}

void memory_load(memory_t *mem, const uint8_t *src, size_t src_len, size_t offset) {
    if (offset >= mem->size) return;
    size_t copy_len = src_len;
    if (offset + copy_len > mem->size) copy_len = mem->size - offset;
    memcpy(mem->data + offset, src, copy_len);
}

uint8_t memory_read8(const memory_t *mem, uint32_t offset) {
    if (offset >= mem->size) return 0;
    return mem->data[offset];
}

uint16_t memory_read16(const memory_t *mem, uint32_t offset) {
    if (offset + 1 >= mem->size) return 0;
    return (uint16_t)((mem->data[offset] << 8) | mem->data[offset + 1]);
}

uint32_t memory_read32(const memory_t *mem, uint32_t offset) {
    if (offset + 3 >= mem->size) return 0;
    return ((uint32_t)mem->data[offset] << 24) |
           ((uint32_t)mem->data[offset + 1] << 16) |
           ((uint32_t)mem->data[offset + 2] << 8) |
           (uint32_t)mem->data[offset + 3];
}

void memory_write8(memory_t *mem, uint32_t offset, uint8_t value) {
    if (!mem->writable || offset >= mem->size) return;
    mem->data[offset] = value;
}

void memory_write16(memory_t *mem, uint32_t offset, uint16_t value) {
    if (!mem->writable || offset + 1 >= mem->size) return;
    mem->data[offset] = (uint8_t)(value >> 8);
    mem->data[offset + 1] = (uint8_t)(value & 0xFF);
}

void memory_write32(memory_t *mem, uint32_t offset, uint32_t value) {
    if (!mem->writable || offset + 3 >= mem->size) return;
    mem->data[offset] = (uint8_t)(value >> 24);
    mem->data[offset + 1] = (uint8_t)(value >> 16);
    mem->data[offset + 2] = (uint8_t)(value >> 8);
    mem->data[offset + 3] = (uint8_t)(value & 0xFF);
}

uint8_t *memory_raw(memory_t *mem) {
    return mem->data;
}
```

**Step 3: Build and verify**

```bash
cd build && cmake .. && make cybiko_core
```

**Step 4: Commit**

```bash
git add src/core/memory.h src/core/memory.c && git commit -m "feat: memory module with big-endian read/write"
```

---

### Task 3: HD66421 LCD Controller

**Files:**
- Create: `src/core/hd66421.h`
- Create: `src/core/hd66421.c`

**Reference:** `HD66421Lcd.java`

**Step 1: Create `src/core/hd66421.h`**

```c
#ifndef CYBIKO_HD66421_H
#define CYBIKO_HD66421_H

#include "types.h"

#define HD66421_WIDTH  160
#define HD66421_HEIGHT 100
#define HD66421_VRAM_SIZE 4000  // 160*100/4

typedef struct {
    uint8_t regs[32];
    uint8_t vram[HD66421_VRAM_SIZE];
    uint8_t frame_buffer[HD66421_WIDTH * HD66421_HEIGHT]; // grayscale 0-255
    uint8_t reg_index;
    uint8_t x_addr;
    uint8_t y_addr;
} hd66421_t;

void    hd66421_init(hd66421_t *lcd);
uint8_t hd66421_read8(hd66421_t *lcd, int offset);
void    hd66421_write8(hd66421_t *lcd, int offset, uint8_t value);
const uint8_t *hd66421_render(hd66421_t *lcd);

#endif // CYBIKO_HD66421_H
```

**Step 2: Implement `src/core/hd66421.c`**

Key details from `HD66421Lcd.java`:
- Register indices: REG_CONTROL1=0x00, REG_CONTROL2=0x01, REG_X_ADDR=0x02, REG_Y_ADDR=0x03, REG_RAM=0x04, REG_START_LINE=0x05, REG_PALETTE1-4=0x0C-0x0F, REG_CONTRAST=0x10
- Control1 bits: R0_DISP=0x40, R0_REV=0x04
- Control2 bits: R1_INC=0x02
- Default palette: regs[0x0C]=0, regs[0x0D]=10, regs[0x0E]=20, regs[0x0F]=31
- VRAM: write8 with offset=1 and reg_index==REG_RAM writes `vram[y_addr*40 + x_addr]`, then auto-increments x or y based on R1_INC
- Rendering: bottom-to-top (y starts at 99, decrements), each byte = 4 pixels (bits 7-6, 5-4, 3-2, 1-0), palette maps 2-bit values to 0-255 grayscale: `clamp(0, 31, 31 - (regs[0x0C+i] - contrast + 3)) * 255 / 31`

Implement all of this matching the Java exactly. See `HD66421Lcd.java` for the complete rendering algorithm.

**Step 3: Build**

```bash
cd build && cmake .. && make cybiko_core
```

**Step 4: Commit**

```bash
git add src/core/hd66421.h src/core/hd66421.c && git commit -m "feat: HD66421 LCD controller with VRAM and rendering"
```

---

### Task 4: Timer8 (8-bit Timers)

**Files:**
- Create: `src/core/timer8.h`
- Create: `src/core/timer8.c`

**Reference:** `H8STimer8.java`

**Step 1: Create `src/core/timer8.h`**

```c
#ifndef CYBIKO_TIMER8_H
#define CYBIKO_TIMER8_H

#include "types.h"

// Forward declare CPU for interrupt requests
typedef struct h8s_cpu h8s_cpu_t;

typedef struct {
    uint8_t  tcr;         // Timer Control Register
    uint8_t  tcsr;        // Timer Control/Status Register
    uint8_t  tcora;       // Compare Match A
    uint8_t  tcorb;       // Compare Match B
    uint8_t  tcnt;        // Timer Counter
    int      prescale_counter;
    int      cached_divisor;
    int      vec_cmia;    // Compare Match A interrupt vector
    int      vec_cmib;    // Compare Match B interrupt vector
    int      vec_ovi;     // Overflow interrupt vector
    h8s_cpu_t *cpu;       // For requesting interrupts
} timer8_t;

void    timer8_init(timer8_t *t, int channel, h8s_cpu_t *cpu);
void    timer8_tick(timer8_t *t);
uint8_t timer8_read(timer8_t *t, int reg);
void    timer8_write(timer8_t *t, int reg, uint8_t value);
bool    timer8_is_running(const timer8_t *t);

#endif // CYBIKO_TIMER8_H
```

**Step 2: Implement `src/core/timer8.c`**

Key details from `H8STimer8.java`:
- Register offsets (after channel selection): 0=TCR, 2=TCSR, 4=TCORA, 6=TCORB, 8=TCNT
- TCR bits: CMIEB=0x80, CMIEA=0x40, OVIE=0x20, CCLR=bits 4-3, CKS=bits 2-0
- TCSR bits: CMFB=0x80, CMFA=0x40, OVF=0x20
- TCSR write: `tcsr = (tcsr & ~0x1F) | (value & 0x1F); tcsr &= value | 0x1F;`
- Clock divisors by CKS: {0, 8, 64, 8192, 256, 256, 256, 256}
- Interrupt vectors: ch0: CMIA=64, CMIB=65, OVI=66; ch1: CMIA=68, CMIB=69, OVI=70
- Tick: prescale, increment tcnt, check compare match A/B, check overflow, fire interrupts, handle CCLR

Implement exactly matching the Java. See `H8STimer8.java` for the complete tick and register logic.

**Step 3: Build and commit**

```bash
git add src/core/timer8.h src/core/timer8.c && git commit -m "feat: 8-bit timer with prescaler and compare match interrupts"
```

---

### Task 5: Timer16 (16-bit Timers, TPU)

**Files:**
- Create: `src/core/timer16.h`
- Create: `src/core/timer16.c`

**Reference:** `H8STimer16.java`

**Step 1: Create `src/core/timer16.h`**

```c
#ifndef CYBIKO_TIMER16_H
#define CYBIKO_TIMER16_H

#include "types.h"

typedef struct h8s_cpu h8s_cpu_t;

typedef struct {
    uint8_t  tcr;
    uint8_t  tmdr;
    uint8_t  tior;
    uint8_t  tier;
    uint8_t  tsr;
    uint16_t tcnt;
    uint16_t tgra;
    uint16_t tgrb;
    int      prescale_counter;
    int      cached_divisor;
    bool     enabled;
    int      channel;
    int      tgr_count;    // 4 for ch0/ch3, 2 for others
    int      vec_tgia;
    int      vec_tgib;
    int      vec_ovf;
    const int *clock_divisors;  // per-channel divisor table
    h8s_cpu_t *cpu;
} timer16_t;

void     timer16_init(timer16_t *t, int channel, int tgr_count, int base_vector, h8s_cpu_t *cpu);
void     timer16_tick(timer16_t *t);
uint8_t  timer16_read8(timer16_t *t, int reg);
uint16_t timer16_read16(timer16_t *t, int reg);
void     timer16_write8(timer16_t *t, int reg, uint8_t value);
void     timer16_write16(timer16_t *t, int reg, uint16_t value);
void     timer16_set_enabled(timer16_t *t, bool enabled);
bool     timer16_is_running(const timer16_t *t);

#endif // CYBIKO_TIMER16_H
```

**Step 2: Implement `src/core/timer16.c`**

Key details from `H8STimer16.java`:
- Register offsets: 0=TCR, 1=TMDR, 2=TIOR, 4=TIER, 5=TSR, 6-7=TCNT, 8-9=TGRA, 0xA-0xB=TGRB
- TIER bits: TGIEA=0x01, TGIEB=0x02, OVIE=0x10
- TSR bits: TGFA=0x01, TGFB=0x02, OVF=0x10
- TSR write: `tsr &= value | ~(0x01 | 0x02 | 0x10)`
- Clear mode: (tcr >> 5) & 0x03: 0=none, 1=clear on TGRA, 2=clear on TGRB
- Clock divisor tables per channel:
  - Ch0: {1, 4, 16, 64, 0, 0, 0, 0}
  - Ch1: {1, 4, 16, 64, 0, 0, 256, 0}
  - Ch2: {1, 4, 16, 64, 0, 0, 0, 1024}
  - Ch3: {1, 4, 16, 64, 0, 1024, 256, 4096}
  - Ch4: {1, 4, 16, 64, 0, 0, 1024, 0}
  - Ch5: {1, 4, 16, 64, 0, 0, 256, 0}
- Vectors: TGIA=baseVector, TGIB=baseVector+1, OVF=baseVector+tgrCount

Implement matching `H8STimer16.java`.

**Step 3: Build and commit**

```bash
git add src/core/timer16.h src/core/timer16.c && git commit -m "feat: 16-bit timer (TPU) with 6 channels"
```

---

### Task 6: PCF8593 Real-Time Clock

**Files:**
- Create: `src/core/rtc.h`
- Create: `src/core/rtc.c`

**Reference:** `PCF8593Rtc.java`

**Step 1: Create `src/core/rtc.h`**

```c
#ifndef CYBIKO_RTC_H
#define CYBIKO_RTC_H

#include "types.h"

typedef enum { RTC_MODE_RECV, RTC_MODE_SEND } rtc_mode_t;

typedef struct {
    uint8_t    data[16];       // RTC registers (BCD)
    bool       active;         // I2C transaction in progress
    rtc_mode_t mode;
    int        pin_scl;
    int        pin_sda;
    int        inp;            // SDA output from RTC (0=pull low, 1=release)
    int        bits;           // bit counter
    int        pos;            // register pointer
    int        data_recv_index;
    uint8_t    data_recv[50];
    uint64_t   last_tick_ns;   // for real-time tracking
    uint64_t   frac_ns;        // fractional nanoseconds accumulator
} rtc_t;

void rtc_init(rtc_t *rtc);
void rtc_scl_w(rtc_t *rtc, bool high);
void rtc_sda_w(rtc_t *rtc, bool high);
bool rtc_sda_r(const rtc_t *rtc);
void rtc_tick(rtc_t *rtc);  // Called once per frame

#endif // CYBIKO_RTC_H
```

**Step 2: Implement `src/core/rtc.c`**

Port the I2C state machine from `PCF8593Rtc.java`:
- I2C address 0xA2 (write) / 0xA3 (read)
- START: SDA falling while SCL high
- STOP: SDA rising while SCL high
- scl_w rising edge: clock in/out bits in RECV/SEND mode
- RECV: after 9 bits (8+ACK), process command. First byte 0xA3=switch to SEND, 0xA2+second byte=set register pointer, further bytes=write registers
- SEND: clock out register data bits, advance on ACK
- Time advancement: use real wall-clock time, increment BCD sec/min/hour/day/month

**Step 3: Build and commit**

```bash
git add src/core/rtc.h src/core/rtc.c && git commit -m "feat: PCF8593 RTC with I2C bit-bang interface"
```

---

### Task 7: Speaker Output

**Files:**
- Create: `src/core/speaker.h`
- Create: `src/core/speaker.c`

**Reference:** `SpeakerOutput.java`

**Step 1: Create `src/core/speaker.h`**

```c
#ifndef CYBIKO_SPEAKER_H
#define CYBIKO_SPEAKER_H

#include "types.h"

#define SPEAKER_SAMPLE_RATE 44100
#define SPEAKER_BUFFER_SIZE 2048

typedef struct {
    int      current_level;
    double   cycles_per_sample;  // clockHz / 44100.0
    double   cycle_fraction;
    uint8_t  buffer[SPEAKER_BUFFER_SIZE];
    int      buffer_pos;
} speaker_t;

void speaker_init(speaker_t *spk, int clock_hz);
void speaker_set_level(speaker_t *spk, int level);
int  speaker_generate_samples(speaker_t *spk, int cpu_cycles, uint8_t *out, int out_size);

#endif // CYBIKO_SPEAKER_H
```

**Step 2: Implement `src/core/speaker.c`**

From `SpeakerOutput.java`:
- `cycles_per_sample = clock_hz / 44100.0`
- `generate_samples(cpu_cycles)`: accumulate fractional cycles, compute samples to write, fill with `level == 0 ? 96 : 160`
- Return number of samples written to `out` buffer

**Step 3: Build and commit**

```bash
git add src/core/speaker.h src/core/speaker.c && git commit -m "feat: 1-bit speaker with PCM sample generation"
```

---

### Task 8: Keyboard Matrix

**Files:**
- Create: `src/core/keyboard.h`
- Create: `src/core/keyboard.c`

**Reference:** `AddressBus.java` readKeyboard method

**Step 1: Create `src/core/keyboard.h`**

```c
#ifndef CYBIKO_KEYBOARD_H
#define CYBIKO_KEYBOARD_H

#include "types.h"

#define KEYBOARD_MAX_COLUMNS 15

typedef struct {
    uint16_t columns[KEYBOARD_MAX_COLUMNS];
    int      num_columns;
} keyboard_t;

void     keyboard_init(keyboard_t *kb, int num_columns);
uint16_t keyboard_read(const keyboard_t *kb, uint32_t address);
void     keyboard_set_key(keyboard_t *kb, int column, uint16_t bitmask, bool pressed);
void     keyboard_set_matrix(keyboard_t *kb, const uint16_t *matrix);

#endif // CYBIKO_KEYBOARD_H
```

**Step 2: Implement `src/core/keyboard.c`**

From `AddressBus.java` readKeyboard:
```c
uint16_t keyboard_read(const keyboard_t *kb, uint32_t address) {
    uint32_t word_offset = ((address - 0xE00000) & 0xFFFFF) >> 1;
    uint16_t data = 0xFFFF;
    for (int i = 0; i < kb->num_columns; i++) {
        if ((word_offset & (1 << i)) == 0) {  // column selected (active low)
            data &= ~kb->columns[i];           // merge pressed keys
        }
    }
    return data;
}
```

**Step 3: Build and commit**

```bash
git add src/core/keyboard.h src/core/keyboard.c && git commit -m "feat: keyboard matrix with column select logic"
```

---

### Task 9: H8S CPU Core - Scaffold, Registers, Interrupts

**Files:**
- Create: `src/core/h8s_cpu.h`
- Create: `src/core/h8s_cpu.c`

**Reference:** `H8SCpu.java` - register access, interrupt handling, step() structure

This task creates the CPU scaffold with register access, fetch helpers, CCR flag helpers, interrupt handling, and the step() function skeleton. Instruction decode is added in Tasks 10-12.

**Step 1: Create `src/core/h8s_cpu.h`**

```c
#ifndef CYBIKO_H8S_CPU_H
#define CYBIKO_H8S_CPU_H

#include "types.h"

// Forward declare address bus
typedef struct address_bus address_bus_t;

// CCR flag bits
#define CCR_C  0x01
#define CCR_V  0x02
#define CCR_Z  0x04
#define CCR_N  0x08
#define CCR_U  0x10
#define CCR_H  0x20
#define CCR_UI 0x40
#define CCR_I  0x80

#define MAX_PENDING_IRQS 32
#define PC_HISTORY_SIZE 64

typedef struct h8s_cpu {
    uint32_t er[8];        // ER0-ER7 (ER7 = SP)
    uint32_t pc;           // Program counter (24-bit)
    uint8_t  ccr;          // Condition code register
    uint8_t  exr;          // Extended control register
    bool     halted;

    // Interrupts
    int      pending_irqs[MAX_PENDING_IRQS];
    int      pending_irq_count;

    // Bus interface
    address_bus_t *bus;

    // Timing
    uint64_t cycle_count;

    // Debug
    bool     tracing;
    uint32_t last_start_pc;
#ifdef CYBIKO_TRACE
    uint32_t pc_histogram[0x100000]; // 1M entries for 24-bit address space
    uint64_t trace_step_count;
#endif
} h8s_cpu_t;

void     h8s_cpu_init(h8s_cpu_t *cpu, address_bus_t *bus);
void     h8s_cpu_reset(h8s_cpu_t *cpu);
void     h8s_cpu_step(h8s_cpu_t *cpu);
void     h8s_cpu_request_interrupt(h8s_cpu_t *cpu, int vector);

// Register access
uint32_t h8s_cpu_get_er(const h8s_cpu_t *cpu, int n);
void     h8s_cpu_set_er(h8s_cpu_t *cpu, int n, uint32_t value);
uint16_t h8s_cpu_get_r(const h8s_cpu_t *cpu, int n);  // n<8: lower 16 of er[n], n>=8: upper 16 of er[n-8]
void     h8s_cpu_set_r(h8s_cpu_t *cpu, int n, uint16_t value);

// Debug
void     h8s_cpu_dump_registers(const h8s_cpu_t *cpu);
#ifdef CYBIKO_TRACE
void     h8s_cpu_dump_profile(const h8s_cpu_t *cpu);
#endif

#endif // CYBIKO_H8S_CPU_H
```

**Step 2: Implement `src/core/h8s_cpu.c` scaffold**

This includes:

1. **Register access helpers** (static inline or macros):
   - `get_er(n)`, `set_er(n, v)` - full 32-bit
   - `get_e(n)` = `(er[n] >> 16) & 0xFFFF`, `set_e(n, v)` - upper 16 bits
   - `get_r(n)` - n<8: `er[n] & 0xFFFF`, n>=8: `(er[n-8] >> 16) & 0xFFFF`
   - `set_r(n, v)` - matching set logic
   - `get_rh(n)` = `(er[n] >> 8) & 0xFF`, `set_rh(n, v)`
   - `get_rl(n)` = `er[n] & 0xFF`, `set_rl(n, v)`
   - `get_reg_b(n)` - n<8: `get_rh(n)`, n>=8: `get_rl(n-8)`
   - `set_reg_b(n, v)` - matching

2. **Fetch helpers:**
   - `fetch16(cpu)` - read 16-bit at PC, PC += 2
   - `fetch32(cpu)` - two fetch16 calls combined

3. **CCR flag helpers:**
   - `get_flag(cpu, bit)`, `set_flag(cpu, bit, val)`
   - `set_nz_b(cpu, result)` - set N=(result&0x80), Z=(result&0xFF)==0
   - `set_nz_w(cpu, result)` - set N=(result&0x8000), Z=(result&0xFFFF)==0
   - `set_nz_l(cpu, result)` - set N=(result&0x80000000), Z=result==0

4. **Interrupt handling:**
   - `request_interrupt(cpu, vector)` - insert into sorted pending_irqs array
   - `process_interrupts(cpu)` - if pending and I flag clear: pop highest priority, push PC and CCR to stack, set I, jump to handler

5. **step() skeleton:** call process_interrupts, check halted, fetch16, call decode (which will be a big switch - initially just prints unimplemented)

6. **reset():** clear all registers, set CCR=0x80 (I flag), read reset vector from address 0

Port these exactly from `H8SCpu.java`. See the reference for precise interrupt push order (push PC 32-bit, then CCR 16-bit) and reset vector loading.

**Step 3: Build and commit**

```bash
git add src/core/h8s_cpu.h src/core/h8s_cpu.c && git commit -m "feat: H8S CPU scaffold with registers, interrupts, and helpers"
```

---

### Task 10: H8S CPU - Instruction Decode Groups 0-3

**Files:**
- Modify: `src/core/h8s_cpu.c`

**Reference:** `H8SCpu.java` decode(), decode0(), decode1(), decode01(), decodeBcc()

Add instruction implementations for opcode groups 0x0-0x3:

**Group 0 (hi nibble 0x0):** NOP, MOV.B/W/L register, ADD.B/W register, ADDX, INC, ADDS, prefixed operations (0100/0110/0140/0180/01C0/01D0/01F0), ORC/XORC/ANDC/LDC CCR, STC, DAA

**Group 1 (hi nibble 0x1):** Shifts (SHLL/SHLR/SHAL/SHAR), Rotates (ROTL/ROTR/ROTXL/ROTXR) for B/W/L with #1 and #2, OR/XOR/AND.B register, NOT/EXTU/EXTS/NEG for B/W/L, SUB.B/W/L register, CMP.B/W register, SUBX, DEC, SUBS, DAS

**Group 2 (hi=0x2):** MOV.B @aa:8, Rd (short absolute read from 0xFFFF00+offset)

**Group 3 (hi=0x3):** MOV.B Rs, @aa:8 (short absolute write to 0xFFFF00+offset)

Key implementation patterns from the Java:

For ADD.B (byte) flag computation:
```c
int s = get_reg_b(rs) & 0xFF;
int d = get_reg_b(rd) & 0xFF;
int result = d + s;
set_flag(cpu, CCR_C, result > 0xFF);
set_flag(cpu, CCR_V, ((d ^ result) & (s ^ result) & 0x80) != 0);
set_flag(cpu, CCR_H, ((d ^ s ^ result) & 0x10) != 0);
set_nz_b(cpu, result);
set_reg_b(rd, (uint8_t)result);
```

For shifts (parameterized helper matching Java):
```c
static void shift_b(h8s_cpu_t *cpu, int rd, bool left, bool logical, int count) {
    int val = get_rh(cpu, rd) & 0xFF;
    bool carry = false;
    for (int i = 0; i < count; i++) {
        if (left) {
            carry = (val & 0x80) != 0;
            val = (val << 1) & 0xFF;
        } else {
            carry = (val & 1) != 0;
            val = logical ? (val >> 1) : ((int8_t)(val << 24) >> 25) & 0xFF; // arithmetic preserves sign
        }
    }
    set_flag(cpu, CCR_C, carry);
    set_flag(cpu, CCR_V, false);
    set_nz_b(cpu, val);
    set_rh(cpu, rd, val);
}
```

Port ALL instructions in these groups from `H8SCpu.java` `decode0()`, `decode1()`, `decode01()`.

IMPORTANT: The prefixed instructions (opcode 0x01xx) fetch a second word and sub-dispatch. This includes MOV.L @ERs/ERd, STM/LDM, LDC/STC word, MULXS/DIVXS, and extended OR/XOR/AND.L.

**Step: Build, test with trace, commit**

```bash
git add src/core/h8s_cpu.c && git commit -m "feat: H8S CPU instruction groups 0-3 (MOV, ADD, shifts, logic)"
```

---

### Task 11: H8S CPU - Instruction Decode Groups 4-7

**Files:**
- Modify: `src/core/h8s_cpu.c`

**Reference:** `H8SCpu.java` decodeBcc(), decode5(), decode6(), decode7()

**Group 4 (Bcc d:8):** All 16 conditional branches. Branch condition evaluation:
```c
static bool evaluate_condition(h8s_cpu_t *cpu, int cond) {
    bool c = get_flag(cpu, CCR_C);
    bool z = get_flag(cpu, CCR_Z);
    bool n = get_flag(cpu, CCR_N);
    bool v = get_flag(cpu, CCR_V);
    switch (cond) {
        case 0: return true;              // BRA
        case 1: return false;             // BRN
        case 2: return !c && !z;          // BHI
        case 3: return c || z;            // BLS
        case 4: return !c;               // BCC
        case 5: return c;                // BCS
        case 6: return !z;               // BNE
        case 7: return z;                // BEQ
        case 8: return !v;               // BVC
        case 9: return v;                // BVS
        case 10: return !n;              // BPL
        case 11: return n;               // BMI
        case 12: return !(n ^ v);        // BGE
        case 13: return n ^ v;           // BLT
        case 14: return !(z || (n ^ v)); // BGT
        case 15: return z || (n ^ v);    // BLE
    }
    return false;
}
```

**Group 5:** MULXU/DIVXU (B/W, unsigned), RTS, BSR (d:8 and d:16), RTE, TRAPA, Bcc d:16, JMP (3 modes), JSR (3 modes)

IMPORTANT TRAPA push format differs from normal interrupt:
```c
// TRAPA: push combined (CCR<<24)|PC as single 32-bit value
cpu->er[7] -= 4;
bus_write32(cpu->bus, cpu->er[7], ((uint32_t)cpu->ccr << 24) | cpu->pc);
```

**Group 6:** BSET/BNOT/BCLR/BTST register, OR/XOR/AND.W register, BST/BIST, MOV.B/W @ERs/@ERd/@ERs+/@-ERd/@(d:16,ERs), MOV.B/W @abs:16/24

Compound 6A instructions: when hi nibble of lo is 1 or 3, it's a bit-op-at-absolute-address

**Group 7:** BSET/BNOT/BCLR/BTST immediate, BOR/BXOR/BAND/BLD/BILD, MOV @(d:32,ERs), word/long immediate ops (ADD.W#imm, CMP.W#imm, SUB.W#imm, OR/XOR/AND.W#imm, MOV.W#imm via 0x79xx, same for .L via 0x7Axx), EEPMOV, bit-ops at memory via 7C/7D/7E/7F prefixes

Port ALL instructions from the Java decode5(), decode6(), decode7() methods.

**Step: Build, commit**

```bash
git add src/core/h8s_cpu.c && git commit -m "feat: H8S CPU instruction groups 4-7 (branches, jumps, bit ops, extended)"
```

---

### Task 12: H8S CPU - Instruction Decode Groups 8-F

**Files:**
- Modify: `src/core/h8s_cpu.c`

**Reference:** `H8SCpu.java`

These are all byte-immediate operations with a simple pattern:

```c
// Groups 0x8-0xF: byte-immediate instructions
// Format: high nibble = operation, rd = (op>>8)&0xF, imm = op&0xFF
case 0x8: add_b_imm(cpu, op); break;  // ADD.B #imm, Rd
case 0x9: addx_b_imm(cpu, op); break; // ADDX.B #imm, Rd
case 0xA: cmp_b_imm(cpu, op); break;  // CMP.B #imm, Rd
case 0xB: subx_b_imm(cpu, op); break; // SUBX.B #imm, Rd
case 0xC: or_b_imm(cpu, op); break;   // OR.B #imm, Rd
case 0xD: xor_b_imm(cpu, op); break;  // XOR.B #imm, Rd
case 0xE: and_b_imm(cpu, op); break;  // AND.B #imm, Rd
case 0xF: mov_b_imm(cpu, op); break;  // MOV.B #imm, Rd
```

Each extracts: `int rd = (op >> 8) & 0xF; int imm = op & 0xFF;`

IMPORTANT: ADDX/SUBX Z-flag behavior: Z is only *cleared*, never set. This is for multi-precision arithmetic:
```c
// ADDX: if result is nonzero, clear Z. But never SET Z.
if ((result & 0xFF) != 0) set_flag(cpu, CCR_Z, false);
```

Port from the Java `addB_imm()`, `addxB_imm()`, etc. methods.

**Step: Build, commit**

```bash
git add src/core/h8s_cpu.c && git commit -m "feat: H8S CPU instruction groups 8-F (byte-immediate operations)"
```

---

### Task 13: Address Bus

**Files:**
- Create: `src/core/address_bus.h`
- Create: `src/core/address_bus.c`

**Reference:** `AddressBus.java`

This is the central memory-mapped I/O router. It connects the CPU to all peripherals.

**Step 1: Create `src/core/address_bus.h`**

```c
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
#include "h8s_cpu.h"

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

typedef struct address_bus {
    // Memory regions
    memory_t  boot_rom;       // 32KB, read-only
    memory_t  external_ram;   // 2MB, writable
    memory_t  flash_rom;      // 512KB, read-only
    memory_t  on_chip_ram;    // 10KB, writable

    // Peripherals
    hd66421_t  *lcd;
    timer8_t   *timer8[2];
    timer16_t  *timer16[6];
    rtc_t      *rtc;
    speaker_t  *speaker;
    keyboard_t *keyboard;
    h8s_cpu_t  *cpu;

    // I/O state
    uint8_t tstr;            // Timer start register
    uint8_t ier;             // Interrupt enable register
    uint8_t isr;             // Interrupt status register
    uint8_t adcsr;           // ADC status register
    uint8_t adcr;            // ADC control register

    // DMA registers (XT)
    uint8_t dma_regs[32];    // 0xFFFEE0-0xFFFEFF
    uint8_t dma_ctrl[8];     // 0xFFFF00-0xFFFF07

    // Serial output
    char    sci_output[3][256];
    int     sci_output_pos[3];

    // DMA completion tracking
    int     dma_completion_delay;
    int     dma_completion_vector;
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
char    *bus_drain_serial(address_bus_t *bus, int channel);

#endif // CYBIKO_ADDRESS_BUS_H
```

**Step 2: Implement `src/core/address_bus.c`**

Port from `AddressBus.java`. Key sections:

1. **`decode_region(address)`** - XT-only memory map:
   - `<= 0x03FFFF` → BOOT_ROM
   - `0x100000-0x100001` → LCD
   - `0x200000-0x200003` → USB (stub)
   - `0x400000-0x5FFFFF` → EXT_RAM
   - `0x600000-0x7FFFFF` → FLASH
   - `0xE00000-0xEFFFFF` → KEYBOARD
   - `>= 0xFFDC00` → ON_CHIP
   - else → UNMAPPED

2. **`read8/write8`** - dispatch to correct backing store based on region

3. **`read_on_chip8/write_on_chip8`** - the big I/O register handler:
   - Timer8: 0xFFFFB0-0xFFFFB9 (even=ch0, odd=ch1, register=offset & ~1)
   - Timer16: routed by channel address ranges (ch0: 0xFFFFD0-0xFFFFDF, ch1: 0xFFFFE0-0xFFFFEB, ch2: 0xFFFFF0-0xFFFFFB, ch3: 0xFFFE80-0xFFFE8F, ch4: 0xFFFE90-0xFFFE9B, ch5: 0xFFFEA0-0xFFFEAB)
   - TSTR: 0xFFFFC0 (read/write, controls timer16 enable bits 0-5)
   - IER: 0xFFFF2E, ISR: 0xFFFF2F (write-0-to-clear)
   - SCI0/1/2: 0xFFFF78/80/88 ranges (SSR reads return 0x84)
   - Port reads: 0xFFFF50-0xFFFF5E (power port at 0xFFFF59 returns 0xC0, Port F at 0xFFFF5E handles RTC SDA read)
   - Port writes: 0xFFFF60 (Port 1 DR, bit 3 = speaker), 0xFFFF6E (Port F DR, bit 1 = SCL, bit 6 = SDA for RTC)
   - ADC: 0xFFFF90-0xFFFF99 (XT returns 0xCC/0x00 for data, ADCSR auto-sets ADF)
   - DMA: 0xFFFEE0-0xFFFEFF, 0xFFFF00-0xFFFF07

4. **DMA transfer** (XT): triggered by writes to dma_ctrl[7], edge detection on bits 4/5 (ch0) and 6/7 (ch1)

Port all of this from the Java. The address bus is the most complex file after the CPU.

**Step 3: Build and commit**

```bash
git add src/core/address_bus.h src/core/address_bus.c && git commit -m "feat: address bus with memory-mapped I/O routing for XT"
```

---

### Task 14: CFS Filesystem

**Files:**
- Create: `src/core/cfs.h`
- Create: `src/core/cfs.c`

**Reference:** `CfsImage.java` (emulator version, Xtreme format only)

**Step 1: Create `src/core/cfs.h`**

```c
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

void  cfs_format(cfs_image_t *img);
bool  cfs_add_file(cfs_image_t *img, const char *name, const uint8_t *data, size_t len);
int   cfs_list_files(const cfs_image_t *img, char names[][64], int max_files);

// Low-level page access
uint8_t *cfs_get_block(cfs_image_t *img, int block_index);
uint16_t cfs_compute_crc16(const uint8_t *data, int length);
void     cfs_update_page_crc(cfs_image_t *img, int page_index);

#endif // CYBIKO_CFS_H
```

**Step 2: Implement `src/core/cfs.c`**

From `CfsImage.java` (emulator) and the manager CFS code:

1. **CRC16 (Xtreme):**
```c
uint16_t cfs_compute_crc16(const uint8_t *data, int length) {
    uint32_t val = 0;
    for (int i = 0; i < length; i++) {
        val = (val ^ data[i] ^ i) << 1;
        val = val | ((val >> 16) & 0x0001);
    }
    return (uint16_t)(val & 0xFFFF);
}
```

2. **Format:** Fill boot blocks with 0xFF + CRC=0xFFFF, file blocks with 0xFF + flags=0x7F + computed CRC, padding with 0xFF

3. **Block access:** `page_offset = (CFS_BOOT_BLOCKS + block_index) * CFS_PAGE_SIZE`, block data at `page_offset + 2`

4. **Add file:** Find free blocks (flags & 0x80 == 0), fill header (flags=0x80, file_id, part_id, filename, timestamp, data), update CRC

5. **List files:** Scan blocks for used (0x80) + part_id==0, extract filenames

**Step 3: Build and commit**

```bash
git add src/core/cfs.h src/core/cfs.c && git commit -m "feat: CFS filesystem for Xtreme NVRAM images"
```

---

### Task 15: Emulator Orchestrator

**Files:**
- Create: `src/core/emulator.c`

**Reference:** `CybikoEmulator.java`

This ties everything together into the `cybiko_emu_t` struct and implements the main frame loop.

**Step 1: Implement `src/core/emulator.c`**

```c
#include "emulator.h"
#include "h8s_cpu.h"
#include "address_bus.h"
#include "hd66421.h"
#include "timer8.h"
#include "timer16.h"
#include "rtc.h"
#include "speaker.h"
#include "keyboard.h"
#include "cfs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct cybiko_emu {
    cybiko_hal_t hal;

    // Components
    h8s_cpu_t    cpu;
    address_bus_t bus;
    hd66421_t    lcd;
    timer8_t     timer8[2];
    timer16_t    timer16[6];
    rtc_t        rtc;
    speaker_t    speaker;
    keyboard_t   keyboard;

    bool running;
    uint64_t total_steps;
    uint32_t frame_count;
};
```

Key methods:
- **`cybiko_create(hal)`**: allocate struct, init all components, wire bus to peripherals, wire CPU to bus
- **`cybiko_destroy(emu)`**: free memory regions, free struct
- **`cybiko_load_boot_rom/flash_rom/nvram`**: load data into respective memory regions
- **`cybiko_reset(emu)`**: call h8s_cpu_reset (reads reset vector from address 0)
- **`cybiko_run_frame(emu)`**: the main frame loop from `CybikoEmulator.java run()`:

```c
void cybiko_run_frame(cybiko_emu_t *emu) {
    // Poll keyboard from HAL
    emu->hal.keyboard_poll(emu->hal.ctx, emu->keyboard.columns, CYBIKO_KEYBOARD_COLUMNS);

    // Cache timer running state
    bool t8_0_run = timer8_is_running(&emu->timer8[0]);
    bool t8_1_run = timer8_is_running(&emu->timer8[1]);
    bool t16_run[6];
    for (int i = 0; i < 6; i++) t16_run[i] = timer16_is_running(&emu->timer16[i]);

    // Execute cycles
    for (int cycle = 0; cycle < CYBIKO_CYCLES_PER_FRAME; cycle++) {
        if (t8_0_run) timer8_tick(&emu->timer8[0]);
        if (t8_1_run) timer8_tick(&emu->timer8[1]);
        for (int i = 0; i < 6; i++) {
            if (t16_run[i]) timer16_tick(&emu->timer16[i]);
        }
        h8s_cpu_step(&emu->cpu);
        bus_tick_dma_completion(&emu->bus);
        emu->total_steps++;
    }

    // Render frame
    const uint8_t *pixels = hd66421_render(&emu->lcd);
    emu->hal.render_frame(emu->hal.ctx, pixels, CYBIKO_LCD_WIDTH, CYBIKO_LCD_HEIGHT);

    // Generate audio
    uint8_t audio_buf[8192];
    int samples = speaker_generate_samples(&emu->speaker, CYBIKO_CYCLES_PER_FRAME, audio_buf, sizeof(audio_buf));
    if (samples > 0) {
        emu->hal.audio_output(emu->hal.ctx, audio_buf, samples);
    }

    // Tick RTC
    rtc_tick(&emu->rtc);

    // Drain serial output
    for (int ch = 0; ch < 3; ch++) {
        char *serial = bus_drain_serial(&emu->bus, ch);
        if (serial && serial[0]) {
            // Output each byte via HAL
            for (int i = 0; serial[i]; i++) {
                emu->hal.serial_output(emu->hal.ctx, (uint8_t)serial[i]);
            }
        }
    }

    emu->frame_count++;
}
```

**Step 2: Build and commit**

```bash
git add src/core/emulator.c && git commit -m "feat: emulator orchestrator with main frame loop"
```

---

### Task 16: SDL2 Linux Client

**Files:**
- Create: `src/linux/main.c`

**Reference:** The Java `SwingRenderer` and CLI argument handling

**Step 1: Implement `src/linux/main.c`**

This creates the SDL2 application that:

1. **Parses CLI args:** `./cybiko-emu <boot.bin> <flash.bin> [--nvram file.nvram] [--trace]`
2. **Loads ROM files** from disk using standard C file I/O
3. **Initializes SDL2:** window (480x300 = 160x100 at 3x scale), renderer, audio device, texture
4. **Implements HAL callbacks:**

```c
// render_frame: convert grayscale pixels to SDL texture
static void hal_render_frame(void *ctx, const uint8_t *pixels, int w, int h) {
    sdl_ctx_t *sdl = (sdl_ctx_t *)ctx;
    uint32_t *tex_pixels;
    int pitch;
    SDL_LockTexture(sdl->texture, NULL, (void **)&tex_pixels, &pitch);
    for (int i = 0; i < w * h; i++) {
        uint8_t g = pixels[i];
        tex_pixels[i] = (0xFF << 24) | (g << 16) | (g << 8) | g;
    }
    SDL_UnlockTexture(sdl->texture);
    SDL_RenderCopy(sdl->renderer, sdl->texture, NULL, NULL);
    SDL_RenderPresent(sdl->renderer);
}

// keyboard_poll: copy current key matrix state
static void hal_keyboard_poll(void *ctx, uint16_t *matrix, int num_cols) {
    sdl_ctx_t *sdl = (sdl_ctx_t *)ctx;
    memcpy(matrix, sdl->key_matrix, num_cols * sizeof(uint16_t));
}

// audio_output: queue samples to SDL audio
static void hal_audio_output(void *ctx, const uint8_t *samples, int count) {
    sdl_ctx_t *sdl = (sdl_ctx_t *)ctx;
    SDL_QueueAudio(sdl->audio_dev, samples, count);
}

// serial_output: print to stderr
static void hal_serial_output(void *ctx, uint8_t byte) {
    fputc(byte, stderr);
}
```

5. **Keyboard mapping:** Map SDL scancodes to Cybiko XT 15-column keyboard matrix. The exact mapping needs to be determined from the Java emulator's key handler. Map common keys: A-Z, 0-9, Enter, Space, Tab, Esc, arrows, Backspace, Shift, Ctrl, Alt.

   The Java emulator maps keys to `setKeyState(column, bitmask, pressed)`. Read the Java `KeyAdapter` implementation to get the exact column/bitmask mapping for each key.

6. **Main loop:**
```c
while (running) {
    uint32_t frame_start = SDL_GetTicks();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) running = false;
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            update_key_matrix(&sdl_ctx, &event.key);
        }
    }

    cybiko_run_frame(emu);

    uint32_t frame_time = SDL_GetTicks() - frame_start;
    if (frame_time < 16) SDL_Delay(16 - frame_time);
}
```

7. **NVRAM persistence:** If `--nvram` specified, load at startup and save on exit

**Step 2: Build the full project**

```bash
cd build && cmake .. && make
```

**Step 3: Commit**

```bash
git add src/linux/main.c && git commit -m "feat: SDL2 Linux client with display, keyboard, and audio"
```

---

### Task 17: Integration - Symlink ROMs and First Boot Test

**Files:**
- Create: `roms/` symlinks

**Step 1: Symlink ROM files**

```bash
cd roms
ln -s ../../cybiko-java-emulator/emulator/src/main/resources/cybikoxt/cyrom150.bin boot.bin
ln -s ../../cybiko-java-emulator/emulator/src/main/resources/cybikoxt/flash_v1358.bin flash.bin
```

**Step 2: Build with trace enabled**

```bash
mkdir -p build && cd build
cmake .. -DCYBIKO_TRACE=ON
make -j$(nproc)
```

**Step 3: First boot test**

```bash
./cybiko-emu ../roms/boot.bin ../roms/flash.bin --trace 2>trace.log
```

Expected: The emulator should start executing from the reset vector, and if everything is wired correctly, begin executing CyOS boot code. The trace log will show instruction execution. The SDL window should appear (possibly with garbled or blank display initially).

**Step 4: Debug cycle**

If the boot fails or crashes:
1. Check trace.log for unimplemented instructions (look for "unimplemented" messages)
2. Compare PC addresses with the Java emulator's trace to find divergence points
3. Fix instruction decode bugs one at a time
4. Common first issues: incorrect flag computation, wrong register access pattern, missing instruction, address bus routing errors

**Step 5: Commit working state**

```bash
git add -A && git commit -m "feat: integration test with ROM symlinks and trace support"
```

---

### Task 18: Keyboard Mapping (Post-Boot Polish)

**Files:**
- Modify: `src/linux/main.c`

**Reference:** Java emulator's KeyAdapter for exact column/bitmask mappings

Once CyOS boots and shows the home screen, implement the full keyboard mapping so we can interact with CyOS.

Read the Java emulator's key event handler to get the exact SDL keycode → (column, bitmask) mapping table for the XT's 15-column keyboard.

Create a lookup table in the SDL client:
```c
typedef struct {
    SDL_Scancode scancode;
    int column;
    uint16_t bitmask;
} key_mapping_t;

static const key_mapping_t key_map[] = {
    // Fill from Java emulator's mappings
    { SDL_SCANCODE_RETURN, /* col */, /* mask */ },
    { SDL_SCANCODE_ESCAPE, /* col */, /* mask */ },
    // ... etc
};
```

**Step: Commit**

```bash
git add src/linux/main.c && git commit -m "feat: full keyboard mapping for Cybiko XT"
```

---

## Implementation Order Summary

| Task | Component | Estimated Complexity |
|------|-----------|---------------------|
| 1 | Project scaffolding | Low |
| 2 | Memory module | Low |
| 3 | HD66421 LCD | Medium |
| 4 | Timer8 | Medium |
| 5 | Timer16 | Medium |
| 6 | RTC (PCF8593) | Medium |
| 7 | Speaker | Low |
| 8 | Keyboard | Low |
| 9 | H8S CPU scaffold | High |
| 10 | CPU groups 0-3 | High |
| 11 | CPU groups 4-7 | High |
| 12 | CPU groups 8-F | Medium |
| 13 | Address bus | High |
| 14 | CFS filesystem | Medium |
| 15 | Emulator orchestrator | Medium |
| 16 | SDL2 Linux client | Medium |
| 17 | Integration test | Medium |
| 18 | Keyboard mapping | Low |

## Key Files to Reference During Implementation

All Java sources at `/home/dan/Projects/cybiko/cybiko-java-emulator/emulator/src/main/java/com/github/daberkow/`:

| C Module | Java Reference |
|----------|---------------|
| `h8s_cpu.c` | `H8SCpu.java` (~2547 lines) |
| `address_bus.c` | `AddressBus.java` (~840 lines) |
| `hd66421.c` | `HD66421Lcd.java` |
| `timer8.c` | `H8STimer8.java` |
| `timer16.c` | `H8STimer16.java` |
| `rtc.c` | `PCF8593Rtc.java` |
| `speaker.c` | `SpeakerOutput.java` |
| `emulator.c` | `CybikoEmulator.java` |
| `memory.c` | `Memory.java` |
| `cfs.c` | `CfsImage.java` (emulator) |
