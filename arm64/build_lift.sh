#!/bin/sh
# Regenerate the lifted C from the Ghidra p-code export and compile it for the host arch.
# usage: build_lift.sh <pcode.txt> [outdir]   (default outdir: build/lift)
set -e
cd "$(dirname "$0")"
PC="$1"; OUT="${2:-build/lift}"
mkdir -p "$OUT/obj"
export LIFT_CFLAGS="${LIFT_CFLAGS:--O2}"
rm -f "$OUT"/lift_*.c "$OUT"/obj/*.o
python3 -I lift/gen_lift.py "$PC" "$OUT"
cp lift/lift_rt.h "$OUT/"
ls "$OUT"/lift_[0-9]*.c | xargs -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -I{} sh -c 'o="'"$OUT"'/obj/$(basename {} .c).o"; clang $LIFT_CFLAGS -std=gnu11 -fno-strict-aliasing -w -c {} -o "$o"'
clang -O1 -std=gnu11 -w -c "$OUT/lift_table.c" -o "$OUT/obj/lift_table.o"
