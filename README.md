# Scud Race: native macOS, Linux and arm64 hosts

Hosts that run the PC port of **Scud Race** without Wine or any Windows DLLs. Each one maps the game's own `scudrace.exe` into memory and runs its code directly,
implementing the Windows imports the game needs (kernel32, msvcrt, user32, winsock, SDL2, ...) on top of the host OS and SDL2.

| Folder | Platform | Status |
|---|---|---|
| [`macos/`](macos) | macOS (x86-64; Rosetta 2 on Apple silicon) | tested: boots, attract mode ~58 fps, race ~22 fps |
| [`linux/`](linux) | Linux x86-64 | tested only in an emulated container (self-test 35/35, boots, attract ~57 fps); not tried on real hardware |
| [`arm64/`](arm64) | macOS arm64, Linux arm64 (RG DS / ROCKNIX) | static recompilation, no .exe at run time; D3D11 on GL/GLES; RG DS ~19-25 fps attract, Mac 60 |

## Credits
The game itself, **Scud Race PC** (a port of Sega's Model 3 arcade game made from a decompilation), is the work of its authors:
**https://github.com/wanszai/Scud-Race**. This repository contains only the hosts; it does **not** include the game, its data or any ROMs.
Scud Race is a Sega game; you must supply the ROM set yourself.

## What you need (both platforms)
1. From the upstream project's releases: `scudrace.exe` (v0.0.14), `ui/`, `Music/` (and `online.ini` for online play) in a `game/` folder.
2. Your own MAME standard split `scud` set as `roms/scud.zip`.
3. See `macos/README.md`, `linux/README.md` and `arm64/README.md` for building and running.
