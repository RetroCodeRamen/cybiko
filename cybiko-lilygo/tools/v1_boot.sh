#!/bin/sh
# Build and run tools/v1_boot.c on this tree's vendored core.
#   tools/v1_boot.sh <boot.bin> <flash.bin> [frames] [screen.pgm]
#   MACHINE=XT STRIDE=20 tools/v1_boot.sh ...
# STRIDE defaults to 1, which is the Java emulator's timing.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
OUT=${OUT:-$HERE/.pio/v1_boot}
mkdir -p "$(dirname "$OUT")"
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=${STRIDE:-1} -DCYBIKO_MACHINE_${MACHINE:-V1} -O2 -Wall -Wextra \
    -I"$CORE" -I"$CORE/core" -I"$RADIO" \
    -o "$OUT" "$HERE/tools/v1_boot.c" "$CORE/cybiko_core_unity.c" \
    "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" "$RADIO/radio_coproc.c" "$RADIO/radio_link.c"
"$OUT" "$1" "$2" "${3:-900}" ${4:+"$4"}
