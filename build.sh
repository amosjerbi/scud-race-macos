#!/bin/bash
# Build the macOS-native host for Scud Race (x86-64 Mach-O; runs natively on Intel Macs, under Rosetta 2 on Apple silicon).
set -euo pipefail
cd "$(dirname "$0")"
SDLINC=${SDLINC:-/opt/homebrew/include/SDL2}
SDLLIB=${SDLLIB:-"$HOME/Library/Application Support/com.isaacmarovitz.Whisky/Libraries/Wine/lib/libSDL2-2.0.0.dylib"}
OUT=build; mkdir -p $OUT $OUT/Frameworks
cp -f "$SDLLIB" $OUT/Frameworks/libSDL2-2.0.0.dylib; chmod u+w $OUT/Frameworks/libSDL2-2.0.0.dylib
install_name_tool -id @rpath/libSDL2-2.0.0.dylib $OUT/Frameworks/libSDL2-2.0.0.dylib 2>/dev/null
codesign --force -s - $OUT/Frameworks/libSDL2-2.0.0.dylib 2>/dev/null
CFLAGS="-arch x86_64 -O2 -g -std=gnu11 -Wall -Wno-unused-function -Wno-incompatible-pointer-types-discards-qualifiers -I$SDLINC -Isrc -fno-strict-aliasing"
for f in loader k32 crt misc sdl_wrap selftest; do clang $CFLAGS -c src/$f.c -o $OUT/$f.o; done
clang -arch x86_64 -O2 -fobjc-arc -Wall -c src/menu.m -o $OUT/menu.o
clang -arch x86_64 -c src/fiber.S -o $OUT/fiber.o
# small pagezero so the fake TEB can live below 4 GB; the PE image sits at its preferred 0x140000000
clang -arch x86_64 -o $OUT/scudrace-host $OUT/*.o $OUT/Frameworks/libSDL2-2.0.0.dylib -framework CoreFoundation -framework Cocoa -Wl,-pagezero_size,0x1000 -Wl,-rpath,@executable_path/Frameworks -Wl,-rpath,@executable_path/../Frameworks
echo built $OUT/scudrace-host
codesign --force -s - $OUT/scudrace-host 2>/dev/null
