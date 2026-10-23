#!/bin/sh
# Two Cybiko Classics in Chat (MAIN) on the in-process radio. Host only.
#   tools/v1_chat.sh <boot.bin> <flash.bin> [frames] [outdir for screens]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
OUT=${OUT:-$HERE/.pio/v1_chat}
mkdir -p "$(dirname "$OUT")"
[ -n "$4" ] && mkdir -p "$4"
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=${STRIDE:-12} -DCYBIKO_MACHINE_V1 -O2 -Wall -Wextra \
    -I"$CORE" -I"$CORE/core" -I"$RADIO" \
    -o "$OUT" "$HERE/tools/v1_chat.c" "$CORE/cybiko_core_unity.c" \
    "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" "$RADIO/radio_coproc.c" "$RADIO/radio_link.c" "$RADIO/radio_sim.c"
"$OUT" "$1" "$2" "${3:-8000}" ${4:+"$4"}
