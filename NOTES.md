# Scud Race for macOS (native host, no Wine)

`ScudRace.app` is a macOS Mach-O that maps the game's own `scudrace.exe` into memory and runs its code directly.
Every Windows API the game imports (379 of them) is implemented here on top of macOS libc, pthreads and SDL2.

* x86-64 build: native on Intel Macs, translated by Rosetta 2 on Apple silicon (a true arm64 build needs the game's source).
* Self-contained: `game/` holds the game program and data (`scudrace.exe`, `ui/`, `Music/`, `online.ini`; the game also writes `scudrace.ini`/`scudrace.nv` there). Nothing outside this folder is needed. `SCUDRACE_HOME` overrides the game folder.
* ROMs: put the MAME split `scud.zip` in `roms/` (the host serves it to the game, which only looks for `scud.zip` next to the exe).
* ROMs: the game needs the *split* `scud` set as `scud.zip` (see the game's README).
* The game's D3D11/D3D9 paths are reported as unavailable, so it uses its own SDL renderer path (Metal under SDL).
  CRT-filter effects and the GPU 3D path that live in the D3D code are therefore not available.
* The Windows auto-updater is disabled (WinHTTP calls fail on purpose).

* Menu bar: a native *Video* menu (resolution 1x–8x, fullscreen/windowed, smooth/sharp, 4:3/wide, scanlines) edits `scudrace.ini` and restarts the game to apply (`src/menu.m`).

Build: `native-mac/make_app.sh`   Self-test of the shim layer: `ScudRace.app/Contents/MacOS/ScudRace --selftest`
Diagnostics: `SCUD_HOST_TRACE=1` (loader log), `=2` (log every Windows API call), `SCUD_HOST_NOGUI=1` (no alert dialogs).

## Performance notes
- The game statically links a libm that uses x87 (`fyl2x`, `fpatan`, `fprem`, `frndint`). Rosetta 2 emulates x87 very slowly (`log2f` alone was ~58% of the
  race frame time: ~7 fps). The loader overwrites those entry points with SSE/libm versions (`patch_x87` in `loader.c`); `SCUD_HOST_X87=1` disables the patch. Race: ~22 fps.
- What remains is the game's CPU texture sampler (`r3d_texel`, `r3d_soft_draw`), dominated by indirect jumps/returns, which Rosetta translates slowly.
- Diagnostics: `SCUD_HOST_SAMPLE=<file>` (Mach thread sampler, no signals; post-process rips against the disassembly), `SCUD_HOST_TRACE=3` (per-import call counts every 5 s).
