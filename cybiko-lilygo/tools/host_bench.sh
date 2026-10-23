#!/bin/sh
# Build and run the host bench against this tree's vendored core, at the
# firmware's EMU_TIMER_STRIDE. Does not touch a T-Deck.
#
#   tools/host_bench.sh <boot.bin> <flash.bin> [frames]
#   BENCH_RADIO=1 tools/host_bench.sh ...    # with a co-processor attached
#   MACHINE=V1 tools/host_bench.sh ...       # Cybiko Classic build (default XT)
#
# Compare the "ram"/"lcd" hash lines before and after a core change: same
# ROMs and frame count must give the same hashes.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
OUT=${OUT:-$HERE/.pio/host_bench}
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=20 -DCYBIKO_MACHINE_${MACHINE:-XT} -O2 -w \
    -I"$CORE" -I"$CORE/core" -I"$RADIO" \
    -o "$OUT" "$HERE/tools/host_bench.c" "$CORE/cybiko_core_unity.c" \
    "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" "$RADIO/radio_coproc.c" "$RADIO/radio_link.c" "$RADIO/radio_sim.c"
"$OUT" "$1" "$2" "${3:-3600}"
