#!/bin/bash
# Assemble dist/ScudRace-linux/: the host binary plus empty game/ and roms/ folders (you add the game files and your ROM).
set -euo pipefail
cd "$(dirname "$0")"
./build.sh >/dev/null
D=dist/ScudRace-linux; rm -rf $D; mkdir -p $D/game $D/roms
cp build/scudrace-host $D/scudrace
cat > $D/README.txt <<'T'
Scud Race for Linux (native host, no Wine). x86-64, needs libSDL2 (apt install libsdl2-2.0-0).
Game by https://github.com/wanszai/Scud-Race. Source: https://github.com/amosjerbi/scud-race-macos
Add: game/ (scudrace.exe v0.0.14, ui/, Music/, online.ini optional) and roms/scud.zip (your own MAME split "scud" set). Then run ./scudrace
T
tar -C dist -czf dist/ScudRace-linux.tar.gz ScudRace-linux && echo dist/ScudRace-linux.tar.gz
