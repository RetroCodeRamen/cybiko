# Handoff: Cybiko Classic V1 LoRa Chat (T-Deck ↔ PC Heltec)

> **Resolved on the host, 23 Oct 2026 (Claude).** The rest of this file is the history before the fix.
>
> - **Root cause:** `timer8.c` raised TMR overflow interrupts only on a 0→1 edge of OVF. TMR1's OVF is set early in boot with OVIE off and is never cleared, so OVI1 never fired. CyOS 1.2.46 runs its own radio scheduler: posting a job (`0x214A6C`) or finishing one calls the tick `0x214760` with a random back-off, the tick loads TCNT1 and enables OVIE (`0x2145D2`), and the overflow interrupt calls the tick again with delay 0, which starts TXI0. Without OVI1 nothing CyOS queued ever went out, so Java (and this tree) faked the tick with a poll/scan driver that guessed Chat from `connObj` bytes. The guesser caused the lost, late, echoed and junk (`1…`, `C…`) frames, and could lock itself (type 0x20 left in the beacon buffer, then every poll skipped).
> - **Fix:** level-sensitive OVI in both `timer8.c` copies (XT `host_bench` hashes unchanged). `sci0_v1.c` lost the driver, bootstrap, nickname/type stamping and RF-service resolve; it keeps the UART, async RX while CyOS's radio is idle, and the CyID. `radio_coproc.c` is back to the committed queue (any size async, poll ≤50, scan any). `radio_lora.c` holds nothing (CyOS paces itself); link message ids are seeded at start-up so a restarted node is not dropped as a repeat.
> - **Keys:** CyOS misses a key held under 10 frames now and then. T-Deck taps are queued, 12 frames down then 3 up; PC `MIN_HOLD` is 12.
> - **Check:** `cybiko-lilygo/tools/v1_chat.sh roms/cyrom112.bin roms/flash_v1246.bin 8000 /tmp/screens` → `PASS: Chat crossed both ways`.
> - **Frames now on the air** are CyOS's own: presence type 0x00 42 bytes; Chat room/messages type 0x20/0x28, 42 or 192 bytes, each message sent 3 times. Nicknames are CyOS's own (About Me; default `g`). `tools/v1_airchat.c` still looks for the old type-0x01 beacons and text at payload[27].
> - **Air test passed** (PC on the Heltec ↔ T-Deck, Chat MAIN, 3 messages each way, 6/6), including both sides sending within 0.1 s.
> - **Radio profile since then:** 915 MHz, SF7, **500 kHz**, **14 dBm** (no bench override in either `platformio.ini`). `sx1262_phy.cpp` listens before talking: 0–30 ms random start, then waits while a preamble or header is latched (one longest packet plus random), giving up after 2 s. The T-Deck stats line and the Heltec status line count these as `waited`/`wait`.


**Date:** 23 Oct 2026  
**Status:** Nearby beacons work. Chat MAIN is **not** reliable. Latest live test: T-Deck→PC sometimes (slow); **PC→T-Deck still fails**. Join-room on T-Deck took ~10 s to appear on PC. Messages 20 s+ or never.  
**Ask of the next agent:** diagnose and fix **bidirectional Chat MAIN over LoRa** with the T-Deck **unplugged** (radio on air, not USB serial). Do not burn another long session on the same local-driver heuristics without a new model of what Chat MAIN actually consumes.

This file is the handoff. `PLAN-V1.md` step 7 is the original radio plan and is stale in places (SCI0 **is** attached now). `cybiko-lilygo/CLAUDE.md` still says V1 radio is not done.

---

## 1. What we are trying to do

Two Cybiko Classic (V1) guests, CyOS **1.2.46**, talking **Chat MAIN** over **915 MHz LoRa**, not USB.

```
PC:  v1_emu  --radio /dev/ttyUSB0 --cyid 4C000001
        USB 921600 SLIP  →  Heltec V3 SX1262 (CyRadio modem)
                              LoRa SF7 / 125 kHz / 915 MHz / 0 dBm
T-Deck: ESP32-S3 firmware  T-Deck-Cybiko-V1
        onboard SX1262  ← air →
        guest CyOS from SD  /cybiko-v1/boot.bin + flash.bin
```

**Success:** type a line on either device, see it in Chat MAIN on the other in a couple of seconds, T-Deck unplugged, keyboard usable.

**Not the goal:** XT (`T-Deck-Cybiko`), Java UDP radio, or rewriting CyOS on the SD card.

---

## 2. Hardware / process facts that bit us

| Item | Fact |
|---|---|
| T-Deck MAC | `14:c1:9f:4a:b3:9c` |
| T-Deck CyID on air | `4C4AB39C` = `0x4C000000 \| mac[3..5]` |
| PC CyID | `4C000001` (`--cyid`, not Heltec Wi‑Fi MAC) |
| Nicknames now | `cyb-B39C` (T-Deck), `cyb-0001` (PC). 8-byte field `conn+20`. |
| Same OS image | Yes. USB flash writes **ESP32 firmware only**. Guest CyOS stays on the SD card (`/cybiko-v1/`). PC loads `roms/cyrom112.bin` + `roms/flash_v1246.bin`. Identity is patched in **RAM**, not by rewriting the card. |
| T-Deck buses | Keyboard I2C `0x55`; LoRa+TFT+SD share **SPI**. They are independent electrically. |
| Heltec | `cybiko-heltec-modem`, CP2102 `/dev/ttyUSB0`, SLIP, `'S'` keepalive ~2 s. |
| **Do not open `/dev/ttyACM0`** to `cat` logs | CDC open can **reset** the T-Deck. Flash uses it; casual serial capture is unsafe. |
| Linux `/dev/ttyUSB0` | Multiple processes can open it. Two `v1_emu` both drove CyRadio. Exclusive flock + `TIOCEXCL` + `/proc` holder check is in `radio_serial_phy.c` now. |
| Restarts spawned a second emu | `pkill -f '/.pio/v1_emu'` does **not** match cmdline `.pio/v1_emu`. Use `pgrep -f 'v1_emu'` then `kill`. |

### How to run the PC emu (one instance)

```bash
cd /home/aj/Documents/Development/Cybiko/cybiko-lilygo
# after building to .pio/v1_emu
DISPLAY=:0 SDL_VIDEODRIVER=x11 .pio/v1_emu \
  /home/aj/Documents/Development/Cybiko/roms/cyrom112.bin \
  /home/aj/Documents/Development/Cybiko/roms/flash_v1246.bin \
  --nvram /home/aj/Documents/Development/Cybiko/roms/pc-classic.flash \
  --radio /dev/ttyUSB0 --cyid 4C000001 \
  --shell /home/aj/Documents/Development/Cybiko/img/cybiko.png
```

Rebuild host emu (do **not** overwrite a running `.pio/v1_emu`; write `.new` then swap after kill):

```bash
HERE=/home/aj/Documents/Development/Cybiko/cybiko-lilygo
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=12 -DCYBIKO_MACHINE_V1 -O2 -Wall -Wextra -Wno-format-truncation \
  -I"$CORE" -I"$CORE/core" -I"$RADIO" -I"$HERE/tools" \
  $(pkg-config --cflags sdl2 libpng x11 xext) \
  -o "$HERE/.pio/v1_emu.new" "$HERE/tools/v1_emu.c" "$HERE/tools/host_serial.c" \
  "$CORE/cybiko_core_unity.c" \
  "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" "$RADIO/radio_coproc.c" \
  "$RADIO/radio_link.c" "$RADIO/radio_lora.c" "$RADIO/radio_serial_phy.c" \
  $(pkg-config --libs sdl2 libpng x11 xext) -lm
```

Flash T-Deck: `pio run -t upload --project-dir cybiko-lilygo -e T-Deck-Cybiko-V1` (`upload_port = /dev/ttyACM0`).

Logs: PC emu stderr has `sci0 POLL/SCAN`, `tx type=`, `rx type=`. Heltec OLED is PHY only.

---

## 3. Architecture (read these files first)

| Layer | Path | Role |
|---|---|---|
| V1 SCI0 driver | `cybiko-c-emulator/src/radio/sci0_v1.c` | Starts POLL `0x30`/50 B or SCAN `0xCF`/200 B; pokes CyID, type, nickname |
| Co-processor | `cybiko-c-emulator/src/radio/radio_coproc.c` | AVR stand-in; TX payload; RX queue (cap **4**); POLL vs SCAN indicators `0x32` / `0xC8` |
| LoRa transport | `cybiko-c-emulator/src/radio/radio_lora.c` | Fragment, beacon hold, packet log |
| Heltec PHY | `cybiko-c-emulator/src/radio/radio_serial_phy.c` | SLIP serial; **exclusive open** |
| T-Deck PHY | `cybiko-lilygo/cybiko-emu/radio_sx1262.cpp` | Direct SX1262; CyID from MAC |
| T-Deck glue | `cybiko-lilygo/cybiko-emu/radio_glue.c` | `#include`s coproc + sci0 (same C as PC) |
| T-Deck loop | `cybiko-lilygo/cybiko-emu/main.cpp` | `pump` before and after `cybiko_run_frame`; `dmaWait` if DIO1 |
| PC SDL | `cybiko-lilygo/tools/v1_emu.c` | Heltec attach `--cyid` |
| Java reference | `AddressBus.java` tickSci0 / tickV1Radio (not in this tree; MIT Dan Berkowitz) | Real CyOS 1.2.46 RAM map |

### Guest radio object (CyOS 1.2.46)

```
V1_RADIO_OBJ_PTR  0x21F216
V1_CYID_RAM       0x21F9DC     patched once from radio_id
V1_RF_OBJECT      0x223186     task state forced to 0x01
conn+4            sender CyID
conn+8            0xC0|channel     = LoRa payload[0]
conn+9            frame type       = payload[1]   0x01 Nearby, 0x20 Chat
conn+16           0x2A if empty    = payload[8]
conn+20           8-byte nickname  = payload[12]
conn+27           chat text        = payload[19]   NOT a length at +26
```

`cybiko_patch_cyid()` writes XT flash offset `0x7F818`. **V1 skips that** (`#ifndef CYBIKO_MACHINE_V1` in T-Deck `main.cpp`). V1 CyID is inside compressed flash; RAM poke is the real patch.

### What Chat MAIN actually needs (learned the hard way)

- **Nearby** is short **type `0x01`**, ~42 bytes, POLL window 50 bytes, async inject indicator **`0x32`**.
- **Chat** is **type `0x20`**, ~192 bytes. Chat MAIN **ignores async `0xC8`**. The 192-byte frame must come back as the **RX of a SCAN (`0xCF`)**, indicator `0xC8`, 200-byte window.
- CyOS retries a failed Chat send on the order of **~45 s**. That looked like “it finally arrived.”
- LoRa is **half-duplex**. If the T-Deck is transmitting a 192-byte leftover, it will miss the PC’s Chat.

Packet log snip: type `0x01` starts at payload[12] (name); type `0x20` starts at payload[19] (text).

---

## 4. Current driver policy (as of this handoff)

### `sci0_frame` (`sci0_v1.c`)

- POLL every **180 frames (~3 s)** when idle (`CYBIKO_V1_POLL_FRAMES`). Java used 15; that filled LoRa and starved keys.
- `chat_pending` if printable `conn+27..29` changed, or type rose to `0x20`.
- **Echo skip:** do not SCAN-as-send if queued RX `data[19..21]` matches `conn+27..29`, **or** if those bytes match `last_rx_chat` for ~180 frames (CyOS copies inbound Chat into `conn+27` after the queue is empty; that was rebroadcast as “we sent it”).
- `want_scan = chat_pending \|\| inbound_large` (`has_large` = queued type `0x20` with len>19).
- SCAN in **2 frames** when `want_scan`; else POLL at 180.
- If idle type `0x20` and not `want_scan`: **skip POLL** (do not stamp `0x01` over Chat, do not truncate to 50 bytes).
- On SCAN-for-RX (`inbound_large`, not `sending_chat`): poke type **`0x01`** so leftover Chat text is not rebroadcast. Nickname `cyb-%04X` always stamped on that path.
- On `sending_chat`: poke type **`0x20`**, SCAN 200.

### `radio_coproc.c`

- `try_async`: **only** frames `len<=50` **and** `data[1] != 0x20`. Chat waits for SCAN.
- `has_large` / SCAN prefer type `0x20`.
- POLL skips type `0x20` even if short.
- Type `0x01` with `len>50` is **clamped to 50** in the RX queue.
- Last inbound Chat `data[19..21]` stored in `last_rx_chat` for echo skip.

### `radio_lora.c`

- Hold **short** unchanged type `0x01` (same channel+name) for **300 pumps** (~5 s at 1 pump/frame on PC; T-Deck pumps **twice** per loop so ~2.5 s).
- **Never air** type `0x01` with `len>50` (SCAN-for-RX leftover).
- **Never air** type `0x20` with `len<=50` (truncated Chat from a 50-byte POLL).
- RX also holds duplicate short `0x01` from the same sender+name (same 300 pumps). **This is a leading suspect for the ~10 s Chat-room join delay.**

### Keyboard (T-Deck)

- I2C drain task on core 1, queue 32.
- Synthetic hold **8 frames**.
- IRQ2 (vector 18) on press **edge**, plus **3 retries** while the key is still down (`emulator.c`). Radio SCAN used to eat the only edge frame; CyOS never scanned again → 2–3 presses per character.

### Exclusive CyRadio

`cybiko_serial_phy_open`: refuse if another PID already has the char device; `flock` + `TIOCEXCL`. Second `v1_emu` prints `CyRadio … already in use by pid N` and continues **without** radio.

---

## 5. What we tried, in order (and why)

Do not re-apply a reverted item without a new reason.

1. **Skip poll/RX while typing (`input_busy`)**  
   Theory: shared SPI. **Wrong.** Keyboard is I2C. This **blocked Chat** until CyOS’s ~45 s retry. **Removed.**

2. **Always SCAN when `D2` clear**  
   Flooded 192-byte type `0x01`. Chat only showed on retry. **Reverted** to SCAN only for chat / inbound large.

3. **Treat any-length `0x01` as a holdable beacon (first time)**  
   Held SCAN Chat when the driver still stamped `0x01` over sends. **Reverted**, then **reintroduced only for `len>50` type `0x01`** after Chat sends as `0x20`.

4. **Expand `lora_is_beacon` to any type-`0x01` length, then clamp**  
   192-byte `0x01` from SCAN-for-RX filled the 4-slot RX queue; SCAN delivered the leftover instead of type `0x20`. Now: hold those on TX, clamp on RX, `has_large` is type `0x20` only.

5. **`try_async` for 192-byte Chat**  
   PHY `rx type=20` arrived, Chat stayed blank. Chat MAIN wants SCAN RX, not idle `0xC8`. **Async is small beacons only.**

6. **Chat detect used `conn+26` as length**  
   Logs showed `"Hi"` at **`conn+27`**. Fixed.

7. **Idle type `0x20` skipped all TX**  
   Heltec RX stuck. Now skip only the **50-byte POLL**; SCAN still for inbound.

8. **Same flash.bin / same nickname `"Cybiko"`**  
   Radio CyIDs already differed. Chat/Nearby still keyed off the name. Nicknames are now `cyb-` + last 4 hex of radio id. Owner asked for that format.

9. **Echo: T-Deck `rx type=20 from 4C4AB39C` of the PC’s own `h1` / `abies…oms`**  
   Driver treated inbound `conn+27` as a local send. Queue peek skip **plus** `last_rx_chat` TTL. **Not proven on air after the last flash.**

10. **Two PC emulators on one Heltec**  
    Linux allows two opens. Packets went to the wrong Chat window. Exclusive attach added. Kill with `pgrep -f 'v1_emu'`, not `pkill '/.pio/v1_emu'`.

11. **IRQ2 retries + KEY_HOLD 8**  
    Keys still occasionally need extra presses; less than before.

12. **Rate-limit beacons (5 s hold)**  
    Needed so two radios share SF7. Likely **too coarse for Chat MAIN presence** (join ~10 s).

Unit tests: `make -C cybiko-c-emulator build/test_radio build/test_lora` — passing after the last policy change. **Air tests are not passing.**

---

## 6. Live air evidence (PC emu stderr)

Pattern that kept repeating:

```
rx type=20 len=192 from 4C4AB39C <text>
sci0 SCAN type=01 size=200 chat=0 in=1     # PC receiving: SCAN as 0x01
tx type=01 len=192 cyb-0001                # until last flash, this went on the air
sci0 SCAN type=20 size=200 chat=1 …        # PC sending
tx type=20 len=192 <pc text>
rx type=20 len=192 from 4C4AB39C <same pc text>   # T-Deck echo
```

Also seen: `tx type=20 len=42` (truncated Chat) then a later 192-byte SCAN of the same snip.

Join-looking payload on type `0x20`: `.....abies...oms....` (likely Chat MAIN room name, not user text).

T-Deck user text that **did** show on PC (slow): e.g. `ggg…`.

PC user text (`1`, `yu`, `1u`) **logged as TX** on the PC; **not** shown on the T-Deck.

Heltec `modem status: busy timeout` after emu restart is leftover TX state; usually clears.

---

## 7. What still looks wrong (hypotheses, ranked)

1. **T-Deck is deaf while transmitting.** Even with 192-byte `0x01` held, Chat SCAN of a local send is 192-byte type `0x20` on the air. If both sides SCAN at once, one loses. Need RX-during / backoff / don’t SCAN-for-RX by transmitting 200 bytes of dummy `0x01` through CyOS RS encode (guest still burns a frame).

2. **Chat MAIN membership.** The app may ignore type `0x20` from a CyID that is not in the room list. Room list may come from a type `0x01` **flag** we **hold** because we only compare nickname. Join delay ~10 s ≈ TX hold + RX hold. PC never “in the room” on the T-Deck → PC messages dropped. **Check Java / disassembly for the in-chat bit in the beacon (payload bytes 2–11).**

3. **SCAN-for-RX still pokes type `0x01` into `connObj`.** CyOS might bind Chat’s RX to `conn+9` rather than the UART RX buffer. Then inbound Chat is presented as Nearby.

4. **Echo skip too wide or too narrow.** 3-byte signature at `payload[19]` collides (`C……` leftover from `"Cybiko"`). TTL 3 s may block a real send of the same first letters.

5. **RX hold of short `0x01`.** PC holds T-Deck beacons 5 s; T-Deck pumps 2× so its TX hold is ~2.5 s. Presence for Chat MAIN is stale.

6. **Keyboard vs radio still coupled in time.** A SCAN RS-encodes 200 bytes on the guest; IRQ2 retries help but a slow frame still misses keys.

7. **Wrong Chat window / stale emu.** Always confirm **one** `v1_emu` and `Radio on /dev/ttyUSB0` in stderr.

---

## 8. Suggested next work (do not start from heuristic #1 again)

1. **Instrument Chat MAIN, not only PHY.** Dump `conn+9`, `conn+20`, `conn+27`, and the 200-byte SCAN RX buffer after each SCAN on both sides (PC stderr is enough; T-Deck Serial.printf already has `cybiko_radio_log`). Prove whether CyOS **sees** type `0x20` inbound.

2. **Find the “in Chat MAIN” beacon field** from Java `AddressBus` / CyOS 1.2.46. Bust TX/RX hold when that field changes, even if the nickname is unchanged. Consider dropping **RX** hold entirely (POLL is already 3 s).

3. **SCAN RX without a 200-byte TX.** If CyOS always transmits `conn` on `0xCF`, keep holding type `0x01` `len>50`. Verify the T-Deck stays in SX1262 RX during that held TX.

4. **Host two_boot V1 Chat** (`cybiko-lilygo/tools/v1_two.c` / c-emulator `two_boot`) on `radio_sim` **without LoRa**. If Chat works on the sim bus and fails on air, the bug is hold/half-duplex/PHY. If it fails on sim, the bug is sci0/coproc vs Chat MAIN.

5. **Stop using 3-byte text as echo identity.** Use sender CyID + a longer payload hash, or “do not SCAN-as-send for N frames after a large RX.”

6. **Do not `cat /dev/ttyACM0`.** If you need T-Deck logs, use the existing `tools/capture_serial.py` knowing it may reset, or log to the TFT chrome line.

---

## 9. Keyboard / T-Deck notes (secondary)

- `$` then `x` = Esc (no Esc key). Trackball click = Enter; hold 600 ms = Esc.
- Keys dropping: IRQ2 edge-only was the main bug; retries added. If it returns, log whether `sci0 SCAN` frames coincide with the miss.
- Do not bring back `input_busy`.

---

## 10. Uncommitted / docs debt

Radio work is **uncommitted**. No `v1.1026.3b` bump (`v1_emu.c` still `1.1026.2b`). `PLAN.md` / `PLAN-V1.md` / `ARCHITECTURE.md` / `CLAUDE.md` were **not** updated (todo 5). This handoff is the radio status of record until those are edited.

Owner OS image on the T-Deck SD card was **never** modified this session.

---

## 11. Prompt stub for a new session

> Read `/home/aj/Documents/Development/Cybiko/HANDOFF-V1-RADIO.md` first. Classic V1 Chat MAIN over LoRa (T-Deck SX1262 ↔ PC Heltec) still does not deliver reliably (PC→T-Deck especially). Do not reintroduce input_busy or always-SCAN. Prefer proving what Chat MAIN reads on SCAN RX vs guessing another sci0 heuristic. T-Deck unplugged; one v1_emu on /dev/ttyUSB0; do not open ttyACM0 just to log.
