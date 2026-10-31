#!/bin/bash
# Build the Linux aarch64 binary (for the RG DS / ROCKNIX) inside a Debian arm64 container.
# Uses the lifted C already generated in $LIFTSRC (default build/lift-opt) and the image blob build/image.bin.
set -euo pipefail
cd "$(dirname "$0")"
LIFTSRC=${LIFTSRC:-build/lift-opt}
CPU=${CPU:-cortex-a55}
# PGO=gen: instrumented build (build/linux-gen); PGO=use: optimised with build/linux.profdata
PGO=${PGO:-}
docker image inspect scud-arm64-build:gles >/dev/null 2>&1 || docker build -t scud-arm64-build:gles - <<'D'
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends clang lld llvm libclang-rt-14-dev python3 libsdl2-dev libgles-dev make file && rm -rf /var/lib/apt/lists/*
D
docker run --rm -v "$PWD":/w -w /w -e LIFTSRC="$LIFTSRC" -e CPU="$CPU" -e PGO="$PGO" scud-arm64-build:gles bash -c '
set -e
O=build/linux; PF=""
if [ "$PGO" = gen ]; then O=build/linux-gen; PF="-fprofile-instr-generate"; fi
if [ "$PGO" = use ]; then O=build/linux-pgo; PF="-fprofile-instr-use=/w/build/linux.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"; fi
mkdir -p $O/lift $O/host
ls $LIFTSRC/lift_[0-9]*.c | xargs -P $(nproc) -I{} sh -c "o=$O/lift/\$(basename {} .c).o; [ \"\$o\" -nt {} ] || clang -O2 -mcpu=$CPU -flto=thin $PF -std=gnu11 -fno-strict-aliasing -w -c {} -o \$o"
clang -O1 -std=gnu11 -w -c $LIFTSRC/lift_table.c -o $O/lift/lift_table.o
CF="-O2 -g -mcpu=$CPU -D_GNU_SOURCE -std=gnu11 -Wall -Wno-unused-function -Wno-incompatible-pointer-types-discards-qualifiers -Wno-deprecated-declarations $(sdl2-config --cflags) -Isrc -I$LIFTSRC -fno-strict-aliasing"
for f in host rtlift fibers overrides k32 crt misc sdl_wrap d3d11gl; do clang $CF -c src/$f.c -o $O/host/$f.o & done; wait
clang -c src/image.S -DIMAGE_BIN="\"/w/build/image.bin\"" -o $O/host/image.o
clang -fuse-ld=lld -flto=thin -O2 -mcpu=$CPU $PF -o $O/scudrace $O/host/*.o $O/lift/*.o $(sdl2-config --libs) -lGLESv2 -lm -lpthread -ldl -Wl,-z,stack-size=8388608
file $O/scudrace; ls -la $O/scudrace
'
