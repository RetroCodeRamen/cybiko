# Cybiko on the LilyGO T-Deck

Native Cybiko Xtreme emulator for the original LilyGO T-Deck, with LoRa as a transport under the emulated Cybiko radio. The guest is original CyOS. This is not a Cybiko-style rewrite.

The design and the reasons for it are in [ARCHITECTURE.md](ARCHITECTURE.md). The work order is in [PLAN.md](PLAN.md).

## Current status

Checked on this machine on 22 Oct 2026.

- `make -C cybiko-c-emulator test` passed. That includes the CPU and peripherals, the radio codec, the SCI2 beacon crossing two guest buffers, the CyID patch, and a 400-byte `.app` written into the filesystem and read back.
- XT ROMs are on this machine as `roms/cyrom150.bin` (32 KB) and `roms/cyos_v1508.bin` (512 KB). They are not part of the tree.
- `two_boot` (7000 frames, exit 0) walks both images through the first-boot wizard (nickname `deck`), pokes `settings.dat` so Communications is on, resets, and opens Chat → General. A types `pingdeck`. After B sends the stock `Hi, everybody!` line (so B's TX cycle pulls the queued frames), B's screen shows `deck> pingdeck` and B's external RAM holds the string. CyIDs are `0x10000001` and `0x10000002`. This is the desktop sim, not a two-board radio test.
- A T-Deck is attached on `/dev/ttyACM0` as Espressif USB JTAG/serial `303A:1001`, MAC `14:c1:9f:4a:b3:9c`. With the FAT card in place it boots CyOS 1.5.08 and reaches the home screen about 70 s after power-on. The emulator now holds 60 fps (frame-limited) through the whole boot: ~922 k guest instructions/s with ~10% headroom on the Loading screen, which was 24 fps before. Idle CyOS uses ~20% of one core. The main fix was Timer16, which was stepping a φ/4 channel tick by tick. Details, method, and before/after numbers are in [cybiko-lilygo/README.md](cybiko-lilygo/README.md#performance). Trackball click is Enter. The SX1262 co-processor is attached; `CYBIKO_RADIO_TX=1` at 0 dBm. A Heltec ping of 34/34 arrived at −21 dBm. CyOS still has not been seen to transmit a frame over the air.
- Saves were broken: at the 800 kHz SD clock every write stopped after 12 KB, and opening `save.nvram` for writing had already emptied it, so the card held a truncated or 0-byte save. The SD clock is now 20 MHz and saves go to `save.tmp` first, with a byte check before the rename. On the T-Deck a full 2 MB save took 2.3 s, and the next boot loaded all 2,097,152 bytes. `flash.bin` now loads in 0.5 s instead of 6 s; the emulator starts 3.5 s after reset, and CyOS reaches idle about 65 s after reset.
- SCI2 with a co-processor attached used to cost one full tick per guest cycle (6x slower core on the host). `cybiko_sci2_advance` skips the countdown-only ticks with identical results. A T-Deck measurement build with the co-processor attached (TX refused) holds 60 fps. `make test`, `make harness`, and `two_boot --frames 7000` still pass, with `two_boot` output unchanged.

## What's next

1. Two-way CyOS radio with the Heltec as the PC's modem. Raw pings already arrive on the T-Deck (34/34). Wireless must be on in `settings.dat` on both sides, then a CyOS frame has to leave the T-Deck and show up on the PC.

2. Confirm a file in `/cybiko/apps/` is listed by CyOS after boot. The injector is in the firmware; it has not been watched on the card.

3. Settings screen outside CyOS (frequency, power, CyID), then unmodified chat between two T-Decks.

4. V1 Classic on the T-Deck: firmware env `T-Deck-Cybiko-V1` exists; first on-device UI is still open. Details in [PLAN-V1.md](PLAN-V1.md).

## What is in this directory

| Path | Role |
|---|---|
| `cybiko-java-emulator-main` | Behavioral reference. Radio co-processor, UDP, and SDR bridge. MIT, Dan Berkowitz |
| `cybiko-c-emulator` | H8S core, host tests, radio codec, `two_boot`, SDL `--radio` |
| `cybiko-lilygo` | T-Deck firmware. XT default, V1 env. Display, keyboard, audio, SD, LoRa pump |
| `cybiko-heltec-modem` | Heltec WiFi LoRa 32 V3. Same SX1262 PHY. USB SLIP to the PC emulator |

## Card layout

| Path | Role |
|---|---|
| `/cybiko/boot.bin` | XT boot ROM, `cyrom150.bin`, 32 KB |
| `/cybiko/flash.bin` | CyOS 1.5.08, `cyos_v1508.bin`, 512 KB |
| `/cybiko/save.nvram` | Optional guest RAM image. Written back about every 5 minutes |
| `/cybiko/apps/*.app` | Copied into that save before boot when the name is not already stored |
| `/cybiko-v1/boot.bin` | Classic boot ROM (`cyrom112.bin`) for the `T-Deck-Cybiko-V1` env |
| `/cybiko-v1/flash.bin` | CyOS 1.2.46 (`flash_v1246.bin`) |
| `/cybiko-v1/save.flash` | AT45DB041 image. Written back when the chip has been programmed |

ROMs and `.app` files are not in this tree.

## Host checks

```bash
make -C cybiko-c-emulator test
make -C cybiko-c-emulator harness
cybiko-c-emulator/build/two_boot roms/cyrom150.bin roms/cyos_v1508.bin --frames 7000
```

Upload to the attached T-Deck:

```bash
pio run -t upload --project-dir cybiko-lilygo --upload-port /dev/ttyACM0
```

A passing host test is not a hardware result. A LoRa ping is not CyOS chat.
