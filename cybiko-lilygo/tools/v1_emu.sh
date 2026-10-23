#!/bin/sh
# SDL Classic. Optional Heltec radio.
#   tools/v1_emu.sh <boot.bin> <flash.bin> [--nvram save.flash] [--radio /dev/ttyUSB0] [--cyid HEX]
# Omit --radio to auto-pick the first Heltec CP2102 (10c4:ea60).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
OUT=${OUT:-$HERE/.pio/v1_emu}
mkdir -p "$(dirname "$OUT")"
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=${STRIDE:-12} -DCYBIKO_MACHINE_V1 -O2 -Wall -Wextra \
    -I"$CORE" -I"$CORE/core" -I"$RADIO" -I"$HERE/tools" \
    $(pkg-config --cflags sdl2 libpng x11 xext) \
    -o "$OUT" "$HERE/tools/v1_emu.c" "$HERE/tools/host_serial.c" \
    "$CORE/cybiko_core_unity.c" \
    "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" "$RADIO/radio_coproc.c" \
    "$RADIO/radio_link.c" "$RADIO/radio_lora.c" "$RADIO/radio_serial_phy.c" \
    $(pkg-config --libs sdl2 libpng x11 xext) -lm
"$OUT" "$@"
