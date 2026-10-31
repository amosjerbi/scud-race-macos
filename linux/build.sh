#!/bin/bash
# Build the Linux-native host for Scud Race (x86-64 ELF). Needs clang (or gcc), libsdl2-dev. Output: build/scudrace-host
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-clang}
SDLCFLAGS=$(sdl2-config --cflags); SDLLIBS=$(sdl2-config --libs)
OUT=build; mkdir -p $OUT
CFLAGS="-O2 -g -std=gnu11 -Wall -Wno-unused-function -Wno-incompatible-pointer-types-discards-qualifiers -Isrc $SDLCFLAGS -fno-strict-aliasing -fno-pie -D_GNU_SOURCE"
for f in loader k32 crt misc sdl_wrap selftest; do $CC $CFLAGS -c src/$f.c -o $OUT/$f.o; done
$CC -c src/fiber.S -o $OUT/fiber.o
# non-PIE: the PE image sits at its preferred 0x140000000 and the fake TEB at 0x10000000, both far from the executable
$CC -no-pie -o $OUT/scudrace-host $OUT/*.o $SDLLIBS -ldl -lpthread -lm
echo built $OUT/scudrace-host
