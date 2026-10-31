#!/bin/bash
# Build the arm64 (statically recompiled) Scud Race: translated game code + host runtime + embedded data image.
# Needs, at build time only: the game's scudrace.exe (for the data image) and the lifted C (build_lift.sh).
#   EXE=/path/to/scudrace.exe ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
EXE=${EXE:-../native-mac/game/scudrace.exe}
LIFT=${LIFT:-build/lift}
OUT=build; mkdir -p $OUT/host
[ -f $OUT/image.bin ] && [ $OUT/image.bin -nt "$EXE" ] || python3 -I gen/mkimage.py "$EXE" $OUT/image.bin
SDLCF=$(sdl2-config --cflags); SDLLD=$(sdl2-config --libs)
CFLAGS="-O2 -g -std=gnu11 -Wall -Wno-unused-function -Wno-incompatible-pointer-types-discards-qualifiers -Wno-deprecated-declarations $SDLCF -Isrc -fno-strict-aliasing"
for f in host rtlift fibers overrides k32 crt misc sdl_wrap d3d11gl; do clang $CFLAGS -c src/$f.c -o $OUT/host/$f.o & done; wait
clang -c src/image.S -DIMAGE_BIN="\"$PWD/$OUT/image.bin\"" -o $OUT/host/image.o
LDX=""
if [ "$(uname)" = Darwin ]; then
  clang -O2 -fobjc-arc -Wall -c src/menu.m -o $OUT/host/menu.o
  LDX="-framework CoreFoundation -framework Cocoa -framework OpenGL -Wl,-stack_size,0x20000000"
else
  LDX="-lGLESv2 -lm -lpthread -ldl"
fi
clang ${LDFLAGS:-} -o $OUT/scudrace $OUT/host/*.o $LIFT/obj/*.o $SDLLD $LDX
[ "$(uname)" = Darwin ] && codesign --force -s - $OUT/scudrace 2>/dev/null || true
echo built $OUT/scudrace
