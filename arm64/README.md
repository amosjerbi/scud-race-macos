# Native arm64 build (static recompilation), for Apple silicon Macs and Linux arm64 handhelds (RG DS / ROCKNIX)

No emulator, no box64, no Rosetta and no `.exe` at run time: the game's x86-64 code is translated ahead of time into C
and compiled for arm64, and its data image is linked into the binary. `scudrace.exe` is needed **once, at build time**.

## How it works
1. **Ghidra** (headless) disassembles `scudrace.exe`. `lift/ghidra/ResolveJumps.java` recovers the large jump tables of the
   game's recompiled PowerPC dispatchers, then `lift/ghidra/ExportPcode.java` exports every function as p-code.
2. `lift/gen_lift.py` turns the p-code into C (one function per guest function, registers kept in locals, Win64-ABI-aware
   syncing at calls). `build_lift.sh` compiles it (PGO + ThinLTO recommended).
3. `src/` is the host: loader for the embedded data image, the runtime (`rtlift.c`: imports, indirect calls, setjmp,
   threads, fibers), and the Windows API on POSIX + SDL2 (kernel32, msvcrt, user32, winsock, ...).
4. `src/d3d11gl.c` gives the game a **Direct3D 11 device on OpenGL ES 3 (Linux) / OpenGL 4.1 (macOS)**, so its GPU
   renderer is used instead of the software rasterizer. The game's HLSL shaders are hand-ported to GLSL in that file.

## Building
```sh
# 1. p-code (Ghidra project with scudrace.exe imported and analysed)
analyzeHeadless <proj> scud -process scudrace.exe -noanalysis -scriptPath lift/ghidra \
  -postScript ResolveJumps.java -postScript ExportPcode.java /path/pcode.txt
# 2. lifted C + objects
LIFT_CFLAGS="-O2 -flto=thin" ./build_lift.sh /path/pcode.txt build/lift-opt
# 3a. macOS binary
EXE=/path/scudrace.exe LIFT=build/lift-opt LDFLAGS="-flto=thin" ./build.sh        # -> build/scudrace
# 3b. Linux aarch64 binary (Debian arm64 container; needs Docker)
./build_linux.sh                                                                  # -> build/linux/scudrace
```
`PGO=gen ./build_linux.sh` / `PGO=use` builds an instrumented / profile-optimised binary (profile at `build/linux.profdata`).

## RG DS (ROCKNIX)
Copy `build/linux*/scudrace` to `ports/scudrace/scudrace-arm64`, `rgds/ScudRace.sh` to `ports/`, the upstream `ui/` and
`Music/` to `ports/scudrace/game/` and your MAME `scud.zip` to `ports/scudrace/roms/`.

Measured on the RG DS (Mali-G52 MC1, Panfrost, 4x Cortex-A55), attract mode:

| renderer | fps |
|---|---|
| box64 + x86-64 host | ~1.1 |
| arm64 recompilation, software 3D | ~1.7 |
| arm64 recompilation, D3D11 on GLES (this build) | ~19-25 |

Settings (environment): `SCUD_HOST_GL_TEXQ` 0 point / 1 bilinear / 2 trilinear (the game's own filtering),
`SCUD_HOST_GL_SCALE` (0.25-1) 3D picture size relative to the window, `SCUD_HOST_GL=0` software renderer,
`SCUD_HOST_FPS=1` prints frame rate and where the render thread's time goes.

No game binary is distributed here: the arm64 binary contains the game's translated code and data, so build it yourself
from your copy of `scudrace.exe`.
