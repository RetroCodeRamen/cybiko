# Architecture

This workspace builds a native Cybiko emulator for the original LilyGO T-Deck (ESP32-S3, ST7789, SX1262). The guest is unmodified CyOS. LoRa is a transport under the emulated AVR radio co-processor, not a new Cybiko application.

The work order is in [PLAN.md](PLAN.md) (Xtreme) and [PLAN-V1.md](PLAN-V1.md) (Classic). The latest measured numbers are in [README.md](README.md) and [cybiko-lilygo/README.md](cybiko-lilygo/README.md#performance).

## Decision

Use Dan Berkowitz's C emulator as the guest core, and the existing `cybiko-lilygo` firmware as the T-Deck host. Extend them. Do not replace the H8S interpreter with a new CPU core. Speed work is batching, placement, and the existing decoder, checked against RAM/LCD hashes.

The Java emulator remains the behavioral reference for the radio co-processor, SCI/DTC sequencing, and the bytes CyOS actually exchanges. It is not the runtime. The T-Deck cannot host a JVM, and the finished device must boot CyOS with no PC attached.

The first wireless implementation is a small LoRa link that carries the same guest payload the Java emulator already sends over UDP. Reticulum is not linked in. The transport vtable is the seam where a later gateway, or an on-device stack, can replace the link without another CyOS change.

## Trees

| Path | Role |
|---|---|
| `cybiko-java-emulator-main` | MIT reference. Radio co-processor, UDP, SDR, V1/XT/V2 behavior |
| `cybiko-c-emulator` | Host H8S core, unit tests, radio codec, `radio_sim`, SX1262 PHY, Heltec serial PHY, `two_boot`, SDL `--radio` |
| `cybiko-lilygo` | T-Deck firmware. Vendored core (XT default, V1 env), display, keyboard, audio, SD, LoRa pump |
| `cybiko-heltec-modem` | Heltec WiFi LoRa 32 V3. Same SX1262 PHY. SLIP to the PC emulator |

The two C cores have diverged. The Linux tree is what `make test` covers. The T-Deck tree adds PSRAM placement, sleep fast-forward, `EMU_TIMER_STRIDE`, Timer16/SCI2 batching, and a compile-time machine switch (`machine.h`). Radio sources live once under `cybiko-c-emulator/src/radio/` and are compiled into firmware from there.

## Target hardware

Original T-Deck, not T-Deck Pro and not a requirement on T-Deck Plus GPS.

| Function | GPIO |
|---|---|
| ESP32-S3FN16R8, 240 MHz, 16 MB flash, 8 MB OPI PSRAM | — |
| Peripheral power enable | 10, driven high |
| Shared SPI SCK / MISO / MOSI | 40 / 38 / 41 |
| ST7789 CS / DC / backlight | 12 / 11 / 42, no RST pin |
| SX1262 CS / RST / DIO1 / BUSY | 9 / 17 / 45 / 13 |
| microSD CS | 39 |
| Keyboard I2C SDA / SCL, address 0x55, INT | 18 / 8 / 46 |
| Trackball up / down / left / right / click | 3 / 15 / 1 / 2 / 0 |
| Speaker I2S WS / BCK / DOUT | 5 / 7 / 6 |

T-Deck Pro is a different machine: 320×240 e-paper, SPI on 36/33/47, SX1262 on CS 3, and a TCA8418 keyboard at I2C 0x34. This firmware must not be built against that pinout.

The SX1262 shares SPI with the display and the card. Every radio transaction leaves TFT CS and SD CS high. DIO2 is the RF switch and DIO3 is a 1.8 V TCXO; `sx1262_phy.cpp` programs both.

Display path: HD66421 160×100 grayscale, nearest-neighbor 2× to 320×200, centered on the 320×240 panel with 20 px bars. Unchanged frames are not redrawn. The RGB565 buffer is in internal DMA SRAM when a block is free.

Trackball click is CyOS Enter (`col 4`, `0x0008`), not Select. Select does not open an icon or leave the first-run pages.

## Guest radio boundary

CyOS never sees LoRa, UDP, or Reticulum. The boundary is the emulated AVR co-processor.

```
CyOS
  → SCI2 TX / DTC (XT) or SCI0 (V1, not on the air yet)
  → emulated AVR co-processor
  → cybiko_radio_transport_t        guest payload + channel
  → link codec                      fragment if the PHY needs it
  → radio_phy (SX1262, serial modem, or radio_sim)
  → peer transport
  → peer co-processor prepare_rx    8-byte AVR header restored
  → SCI RX / DTC
  → peer CyOS
```

`cybiko_radio_transport_t` matches the Java `RadioTransport` contract:

- `send_packet(payload, channel)` is non-blocking. It copies bytes and returns.
- A listener later receives `(payload, channel, sender CyID)`.
- `pump()` is how the host moves packets. The CPU frame loop must not sit inside an SPI transfer or a LoRa airtime wait.
- `set_channel()` records the channel CyOS selected. The transport does not drop on channel. CyOS hops between 2 and 4 while packets may still be in flight.

On transmit, the co-processor strips the 8-byte RF2915 preamble/CyID header. XT frames also drop the trailing status byte (51→42, 201→192). V1 frames have no trailing byte. Anything shorter than or equal to 9 bytes is an AVR command, not a packet, and is not transmitted.

On receive, `prepare_rx` rebuilds the buffer CyOS expects:

| Offset | Bytes |
|---|---|
| 0–3 | destination `FF FF FF FF` (broadcast) |
| 4–7 | sender CyID, big-endian, from the transport, not invented |
| 8+ | guest payload |
| pad | `FF` out to 50 or 200 bytes |

A null read is filled with `00`, not `FF`. An all-`FF` buffer passes CyOS's broadcast check and leaks connection objects.

Local `03` bytes are UART acknowledgements from the emulated AVR. They are not a claim that a peer received the packet. The link layer does not emit its own ACK.

XT `complete_tx_dtc` returns `32` for a small queued frame or an empty queue, and `C8` for a large frame. Poll (`30`) will not dequeue a payload longer than 50 bytes; that frame waits for scan (`CF`).

The Java co-processor waits up to 15 ms inside `complete_tx_dtc`. That wait is not ported. A 192-byte chat payload plus the 16-byte link header is about 330 ms on the air at SF7/125 kHz/4/5; a 42-byte beacon is about 110 ms. Frames that arrive later are delivered by `try_async` when the guest radio is idle.

The T-Deck XT image attaches the co-processor after the SX1262 starts. `CYBIKO_RADIO_TX=1` (set in `platformio.ini`) lets it transmit. Without that flag the board receives only. CyOS still sends nothing until Communications is on in `settings.dat`.

## Link layer and PHY

SX1262 explicit-header mode carries at most 255 bytes. Current CyOS payloads are 42 or 192 bytes. With a 16-byte link header they fit in one PHY packet (58 and 208 bytes). Fragmentation still exists so a smaller MTU does not need a CyOS change.

```
0     magic 0xC7
1     version 1
2     flags (reserved, 0)
3–4   message id, big-endian
5     fragment index
6     fragment count (1–8)
7     channel
8–11  sender CyID, big-endian
12–13 total guest payload length
14–15 CRC-16/CCITT-FALSE over the header (CRC field zero) and this chunk
16+   chunk
```

No transport ACK and no transport retry. CyOS is the protocol that retransmits. PHY duplicates are dropped by message id; CyOS's own 3× retransmit gets a new id each time.

`sx1262_phy.cpp` is shared by the T-Deck and the Heltec modem: DIO2 RF switch, 1.8 V TCXO with recalibration, image calibration, PA, OCP, TX-clamp and IQ errata, private sync word `0x1424`, continuous RX. TX starts and returns; TxDone/RxDone are read in `poll()` when DIO1 is high.

`radio_lora.c` is the `cybiko_radio_transport_t` over any `radio_phy`. One fragment on the air at a time. Sends are rejected while transmit is gated off.

There is no dedicated radio task. The T-Deck pumps the radio once per frame from the Arduino loop, and only when display DMA is idle. Right after `tft.initDMA()`, the Arduino SPI bus reads back 0 until the first DMA transfer; the emulator draws a frame before the first radio pump.

Default profile:

| Parameter | Value | Notes |
|---|---|---|
| Frequency | 915.0 MHz | US915 starting point. Not legal everywhere |
| Bandwidth | 125 kHz | Chat fits in one packet at SF7 |
| Spreading factor | 7 | Keeps airtime under a second |
| Coding rate | 4/5 | |
| Power | 0 dBm on the T-Deck and Heltec desk builds; 14 dBm is the header default | Desk range until the settings UI exists |
| Sync word | 0x1424 | Private LoRa, not LoRaWAN |
| Preamble | 8 symbols | |

The T-Deck CyID is `0x4C` plus the low 24 bits of its MAC, patched into flash the way the Java emulator patches `0x7F818`. The PC `--radio` path defaults to `0x4C000001`.

## Why not Reticulum as the first transport

Reticulum is a real network stack, and a gateway later is attractive. It is the wrong thing to put under CyOS on this chip right now.

- The guest model is broadcast datagrams with the CyID inside the frame. Reticulum data delivery is path-routed and acknowledged.
- A 192-byte guest payload plus a Reticulum header does not reliably fit in a 255-byte LoRa PHY packet.
- Path tables would spend RAM and CPU the interpreter already needs.
- The Python Reticulum stack is not MIT. Nothing is linked here.

`cybiko_radio_transport_t` does not mention LoRa. A later Reticulum wrapper can sit on the same vtable after a license review, without changing CyOS.

## Runtime shape on the ESP32-S3

```
Core 1 (Arduino loop)
  keyboard I2C, trackball
  cybiko_run_frame
  display DMA (skip if the 160×100 frame is unchanged)
  audio write (blocks until the 800-sample slot is free)
  radio pump when DMA is idle

Guest core
  H8S interpreter (IRAM, -O3, jump tables restored for this TU)
  timers (Timer16 skips ticks that cannot match)
  SCI2 (skips countdown-only ticks when a co-processor is attached)
  LCD, speaker, CFS
```

`EMU_TIMER_STRIDE=20` on XT (12 on the V1 env). Each guest instruction advances the timers that many cycles, so an XT frame is about 15360 instructions. 60 fps is about 0.9 guest MIPS, not a real XT. The 60 Hz limiter (`pace_frame`) keeps RTC, key hold, and audio from running fast once the core is ahead of real time.

Memory: emulator state, on-chip guest RAM, and boot ROM in internal SRAM; 2 MB (XT) guest RAM and the flash image in PSRAM. The 128 KB RGB565 buffer prefers internal DMA SRAM.

Saves: XT writes `/cybiko/save.nvram` via `save.tmp` at 20 MHz SPI. A full 2 MB save was 2.3 s on the attached board. V1 writes the AT45 chip image to `/cybiko-v1/save.flash`.

## Machines

Compile-time switch in `cybiko-lilygo/lib/cybiko_core/src/core/machine.h`. The XT is the default. V1 is `-DCYBIKO_MACHINE_V1` / PlatformIO env `T-Deck-Cybiko-V1`.

| | XT (shipping path) | V1 Classic |
|---|---|---|
| CPU clock | H8S/2323 @ 18.432 MHz | H8S/2241 @ 11.0592 MHz |
| RAM | 2 MB @ 0x400000 | 512 KB @ 0x200000 |
| Storage | memory-mapped flash + CFS in RAM | AT45DB041 on SCI1 |
| Radio UART | SCI2 + DTC | SCI0 (not on the air yet) |
| Card folder | `/cybiko/` | `/cybiko-v1/` |
| Stride on device | 20 | 12 |
| Hardware | boots CyOS 1.5.08 to home (~70 s) | firmware built; first on-device UI not yet observed |

The H8S takes no interrupt immediately after LDC/ANDC/ORC/XORC to CCR (`irq_hold`). CyOS's scheduler relies on that. Without it, boots freeze on the logo at some strides.

## Process and licenses

- Guest core, firmware HAL, and the Java reference: MIT, Copyright 2026 Dan Berkowitz. That notice stays in the copies.
- MAME was a hardware reference for the upstream authors. This project does not vendor MAME sources.
- ROMs and `.app` files are not in the tree.

## What this tree runs today

See [README.md](README.md) for the latest run. In short: host tests and `two_boot` pass; the attached T-Deck boots XT CyOS at a held 60 fps; PC→T-Deck LoRa pings arrive; CyOS chat over LoRa is still open; V1 is a second firmware env, not yet a hardware UI result.
