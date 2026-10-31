# Scud Race for macOS (native host)

A macOS host that runs the PC port of **Scud Race** without Wine, Whisky or any Windows DLLs. It maps the game's own `scudrace.exe` into memory and
runs its code directly, implementing the Windows imports the game needs (kernel32, msvcrt, user32, winsock, SDL2, ...) on top of macOS and SDL2.
It is an x86-64 Mach-O, so it runs natively on Intel Macs and under Rosetta 2 on Apple silicon.

## Credits
The game itself, **Scud Race PC** (a port of Sega's Model 3 arcade game made from a decompilation), is the work of its authors:
**https://github.com/wanszai/Scud-Race**. This repository contains only the macOS host; it does **not** include the game, its data or any ROMs.
Scud Race is a Sega game; the ROM set is not included and must be supplied by you.

## What you need
1. The game files from the upstream project's releases: `scudrace.exe`, `ui/`, `Music/` (and `online.ini` if you want online play). Put them in `game/`.
2. Your own MAME standard split `scud` ROM set as `roms/scud.zip`.
3. Xcode command line tools and an x86-64 SDL2 dylib (`SDLLIB=/path/to/libSDL2-2.0.0.dylib`, headers in `SDLINC`).

```
native-mac layout:   game/scudrace.exe  game/ui/  game/Music/   roms/scud.zip
./make_app.sh        # builds ScudRace.app next to game/ and roms/
```

The game's address layout (`scudrace.exe` v0.0.14) is patched in a few places (`gs:` TEB reads, x87 libm entry points), so the host targets that exact build.

See [NOTES.md](NOTES.md) for how it works, debug environment variables and performance notes.
