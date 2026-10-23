#!/bin/sh
# One Classic on a Heltec modem. Host only, 60 fps.
#   tools/v1_radio.sh <boot.bin> <flash.bin> [--radio /dev/ttyUSB0] [--cyid HEX] [--seconds N]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
OUT=${OUT:-$HERE/.pio/v1_radio}
mkdir -p "$(dirname "$OUT")"
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=${STRIDE:-12} -DCYBIKO_MACHINE_V1 -O2 -Wall -Wextra \
    -I"$CORE" -I"$CORE/core" -I"$RADIO" \
    -o "$OUT" "$HERE/tools/v1_radio.c" "$CORE/cybiko_core_unity.c" \
    "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" "$RADIO/radio_coproc.c" \
    "$RADIO/radio_link.c" "$RADIO/radio_lora.c" "$RADIO/radio_serial_phy.c"
"$OUT" "$@"
