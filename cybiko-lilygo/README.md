# Cybiko Xtreme Emulator for LilyGo T-Deck

![CyOS running on a LilyGo T-Deck](tdeck-small.jpg)

> Original upstream note: 13–20 fps on the ESP32-S3. That figure is obsolete. On the attached original T-Deck this tree holds 60 fps (frame-limited) through CyOS 1.5.08 boot; see Performance.

A Cybiko Xtreme (XT) handheld computer emulator for the original LilyGo T-Deck (ESP32-S3, ST7789, keyboard at I2C 0x55). T-Deck Pro is e-paper with a different pinout and is not the target. The pin definitions in `tdeck_pins.h` match the original T-Deck. This tree boots CyOS with display, audio, keyboard, filesystem, and the SX1262 as the radio transport under the emulated AVR co-processor. A second env, `T-Deck-Cybiko-V1`, builds the Classic (see `../PLAN-V1.md`).

The Cybiko XT was a wireless handheld computer from the early 2000s built around the Hitachi H8S/2323 CPU with a 160x100 grayscale LCD. This project emulates that hardware in C and runs the original CyOS firmware on the T-Deck's ESP32-S3, using its keyboard, display, trackball, and speaker.

## Hardware

- **LilyGo T-Deck** (original, ESP32-S3, 16MB Flash, 8MB PSRAM)
- ST7789 320x240 TFT display (Cybiko's 160x100 output is 2x upscaled to 320x200)
- Built-in QWERTY keyboard and trackball
- I2S speaker for audio
- MicroSD card slot for ROM files

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- A LilyGo T-Deck (original, not Pro)
- A MicroSD card
- Cybiko XT ROM files: `boot.bin` and `flash.bin`

## Setup

### 1. Prepare the SD card

Create a `/cybiko/` folder on the SD card and copy in your ROM files:

```
SD card root/
  cybiko/
    boot.bin
    flash.bin
```

ROMs are not included in this repo. You need the Cybiko XT v1.5.08 firmware dumps:

| SD card name | Original filename | Size | SHA-256 |
|---|---|---|---|
| `boot.bin` | `cyrom150.bin` | 32 KB | `b22f1f30cbf4000d060bacd676ef728e2d3bd7d3eaccbae53fb37cee72604ec1` |
| `flash.bin` | `cyos_v1508.bin` | 512 KB | `2d68c951749102eb310daf3d652e6818ca7e121b9ebd93c5340fe813e8fa11d3` |

Insert the SD card into the T-Deck.

### 2. Build and flash

```bash
git clone <this-repo-url>
cd cybiko-lilygo

# Build
pio run

# Build and upload to T-Deck
pio run -t upload

# Open serial monitor to see boot logs and FPS
pio run -t monitor
```

PlatformIO will automatically fetch dependencies (TFT_eSPI, etc.) on first build. No additional setup is needed.

## Usage

Once flashed, the T-Deck boots directly into CyOS. The serial monitor (115200 baud) prints fps, guest instructions per second, and frame-limiter headroom every 5 seconds (see Performance).

**Keyboard (XT):** The T-Deck's QWERTY keyboard maps to the Cybiko's keyboard matrix. Letters, Enter, Space, Backspace, Tab, and Escape work directly. Numbers are entered via Fn+letter combinations (same as the original Cybiko). The V1 env maps digits directly.

**Trackball:** Maps to the Cybiko's directional pad. Center click is Enter (`col 4`, `0x0008`), not Select. Select does not open an icon.

**NVRAM:** XT emulator state is auto-saved to `/cybiko/save.nvram` via `save.tmp` every 5 minutes (only when data has changed). SD SPI is 20 MHz.

**Radio:** After the SX1262 starts, the firmware attaches the guest co-processor and pumps the LoRa transport once per frame when display DMA is idle. Default: 915 MHz, SF7, 125 kHz, 0 dBm, `CYBIKO_RADIO_TX=1`. CyOS still sends nothing until Communications is on in `settings.dat`. The PC side is `../cybiko-heltec-modem/` plus `cybiko-emu --radio`.

**Classic (V1):** `pio run -e T-Deck-Cybiko-V1` with `/cybiko-v1/boot.bin` and `flash.bin` on the card. First on-device UI is still open; see `../PLAN-V1.md`.

## Project Structure

```
cybiko-lilygo/
  cybiko-emu/
    main.cpp          # T-Deck firmware (display, keyboard, audio, SD, radio pump)
    radio_sx1262.*    # SX1262 transport wrapper
    tdeck_pins.h      # Hardware pin definitions (original T-Deck)
  tools/
    host_bench.sh     # Host build + RAM/LCD hash check of the vendored core
    v1_boot.sh        # V1 host STATUS line vs Java
    capture_serial.py # Timestamped serial log for a fixed time
  lib/
    cybiko_core/      # Emulator core (vendored from cybiko-c-emulator)
  boards/
    T-Deck.json       # Custom board definition
  platformio.ini      # T-Deck-Cybiko (XT) and T-Deck-Cybiko-V1
```

The emulator core (`lib/cybiko_core/`) is platform-independent C and communicates with the T-Deck firmware through a HAL (Hardware Abstraction Layer) with callbacks for display rendering, audio output, keyboard polling, and serial output. The same core also builds as a standalone Linux/SDL2 emulator from the [cybiko-c-emulator](https://github.com/daberkow/cybiko-c-emulator) repo.

## Performance

Measured on an original T-Deck (ESP32-S3, 240 MHz) on 22 Oct 2026, booting CyOS 1.5.08 from the card. The serial log prints one line every 5 s:

```
60.0 fps (100%)  922 kIPS  232 cyc/insn  sleep 0%  core 4461 ms  render 22 ms  idle 526 ms
```

`kIPS` is guest instructions per second. `cyc/insn` is ESP32 cycles per guest instruction, outside the display callback. `sleep` is the share of guest cycles spent in the SLEEP fast-forward. `idle` is time the frame limiter waited, which is the headroom.

| Phase | Before | After |
|---|---|---|
| CyOS loader, LZSS unpack | 61 fps | 60 fps (capped) |
| "Loading CyOS" (a checksum loop in guest RAM) | 24 fps, 370 kIPS, ~630 cyc/insn | 60 fps (capped), 922 kIPS, ~232 cyc/insn, ~10% idle |
| Power-on to home screen | several minutes | ~70 s |
| CyOS idle at the home screen | — | 60 fps, ~20% of one core |
| Display callback | ~360 ms per 5 s | ~3 ms per 5 s |

### What changed

1. **Timer16 skips ticks that cannot match** (`timer16_advance`). gprof on the host showed ~60% of the time in `timer16_event`: about 5 calls per guest instruction. CyOS leaves channel 3 at φ/4, clear on TGRA, TGRA=1, with TGFA already set, so every tick is a compare match with no effect. The batch now jumps to the next tick that can hit TGRA, TGRB, or overflow. If a whole clear-on-TGRA period sets no new TSR flag and moves no output pin, the remaining whole periods are dropped. Interrupts, flags, and speaker edges are unchanged.
2. **60 Hz frame limiter** (`pace_frame` in `main.cpp`). Once idle CyOS reached ~260 fps, the RTC, key hold, and audio ran 4x fast. Each frame is a fixed slice of guest time, so the loop now waits for the 60 Hz deadline. A frame that runs late is not made up in a burst.
3. **Unchanged frames are not drawn.** The 160×100 frame is compared with the last one sent; CyOS rarely redraws.
4. **The 128 KB RGB565 buffer is in internal DMA SRAM** when a block is free (it is: ~270 KB largest block at boot), so the upscale does not stream through the 32 KB PSRAM data cache. It falls back to PSRAM.
5. **SCI2 skips ticks that only count down** (`cybiko_sci2_advance`, shared with the host core). With a co-processor attached, the old loop ran one full SCI2 tick per guest cycle, 307200 a frame. On the host bench that made the core 6x slower (0.71 s → 4.5 s for 3600 frames). It is 0.77 s now. On the T-Deck the co-processor is attached after the SX1262 starts (`CYBIKO_RADIO_TX=1` in `platformio.ini`) and the 60 fps cap still holds.

Earlier upstream work stays: `IRAM_ATTR` and `-O3` on the interpreter, eight CPU steps per timer batch, boot ROM and on-chip RAM in internal SRAM.

### How the results were checked

- `tools/host_bench.sh <boot.bin> <flash.bin> [frames]` builds this tree's vendored core on the host at stride 20 and prints a hash of guest RAM and the LCD every 250 frames. Before and after each core change, 6000 frames gave identical hashes, with and without `BENCH_RADIO=1`.
- `cybiko-c-emulator`: `make test` and `make harness` pass. `two_boot --frames 7000` exits 0 and puts `pingdeck` in B's RAM, with output identical to the pre-change build apart from timing.
- The c-emulator `test_timer16`, `test_timer8`, `test_h8s_cpu`, `test_hd66421`, `test_memory`, and `test_keyboard` pass against this tree's core. `test_speaker` does not build against it; the two speaker structs have diverged.
- `tools/capture_serial.py <seconds> <log>` records the serial lines after `pio run -t upload`.
- Build with `PLATFORMIO_BUILD_FLAGS=-DCYBIKO_PROFILE` to add a guest-PC histogram to each report. The radio co-processor is always attached when the SX1262 starts; `CYBIKO_RADIO_TX=1` (set in `platformio.ini`) lets it transmit.

### What is still slow

- `EMU_TIMER_STRIDE=20` is a timing approximation: each guest instruction advances the timers 20 cycles, so a frame carries ~15360 instructions. The guest gets ~0.9 MIPS, well short of a real XT. CPU-heavy apps can still run slower than on hardware. Halving the stride needs about 2x interpreter speed for the same 60 fps.
- ~230 ESP32 cycles per guest instruction, against ~60 for the same core on a desktop. The guest's RAM and flash image are in PSRAM behind a 32 KB data cache with 32-byte lines (the Arduino SDK default). A 64 KB data cache needs a custom ESP-IDF sdkconfig.
- Further interpreter work, in rough order of payoff: lazy flags, a fetch window instead of four range compares per fetch, and keeping `pc`/`ccr` in locals in `h8s_cpu_run`.

## Acknowledgments

The emulator core was developed with reference to several existing Cybiko emulation projects:

- [MAME](https://github.com/mamedev/mame) — Cybiko XT machine driver and H8S CPU core (BSD-3-Clause)

## License

MIT — see [LICENSE](LICENSE) for details.

