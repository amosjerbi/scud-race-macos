# Scud Race for Linux (native host)

Same idea as the macOS host, in `../native-mac`: an x86-64 ELF that maps the game's own `scudrace.exe` into memory and runs its code directly, with the Windows imports
(kernel32, msvcrt, user32, winsock, SDL2, ...) implemented on glibc, pthreads and SDL2. No Wine. Output goes through SDL's OpenGL renderer (software fallback).

## Layout
```
scudrace            (the host; build/scudrace-host when built from source)
game/               scudrace.exe (v0.0.14), ui/, Music/, online.ini (optional) -- from the upstream release
roms/scud.zip       your own MAME standard split "scud" set
```

## Build
`sudo apt install clang libsdl2-dev` then `./build.sh` (or `./package.sh` for `dist/ScudRace-linux.tar.gz`). Self-test: `build/scudrace-host --selftest`.

## Status
- Built and tested only inside an emulated x86-64 Debian 12 container (qemu, Xvfb, Mesa llvmpipe, dummy audio): the self-test passes 35/35 and the game boots and runs attract mode (~57 fps there).
- **Not tested on real Linux hardware**: real audio (ALSA/PulseAudio), controllers, fullscreen, online play, a full race.
- No menu bar like the macOS build; edit `game/scudrace.ini` (e.g. `scale=4`, `smooth=1`) or use the game's own OPTIONS menu.
- `SCUD_HOST_SSE_LIBM=1` swaps the game's x87 libm functions for SSE ones (only matters under Rosetta/emulation).

Debug variables are the same as the macOS host (`SCUD_HOST_TRACE`, `SCUD_HOST_FPS`, `SCUDRACE_HOME`, ...); the Mach-thread sampler (`SCUD_HOST_SAMPLE`) is macOS-only.
Game by https://github.com/wanszai/Scud-Race.
