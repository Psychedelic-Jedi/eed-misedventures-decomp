# Ed, Edd n Eddy: The Mis-Edventures — Decompilation

Work-in-progress decompilation of *Ed, Edd n Eddy: The Mis-Edventures* (GameCube, USA, `GE9E5D`), with the long-term goal of a native Android port.

This repository contains **no game assets or code**. You need your own legally dumped copy of the game.

## Status

- [x] `main.dol` split with [decomp-toolkit](https://github.com/encounter/decomp-toolkit) (~10,000 functions, 1,161 units)
- [x] Reverse the `.gc` pack format; `tools/gctool.py` converts level code modules to ELF
- [ ] Identify and name engine (Chimera) subsystems
- [ ] Decompile engine code
- [x] Static recompilation (DolRecomp) of main.dol and all 30 level code modules, with an HLE runtime (`runtime/`)
- [x] Android APK built entirely on the phone in Termux (`scripts/build_android.sh`); boots into gameplay
- [x] GPU renderer: GX raster pipeline on OpenGL ES (`runtime/gx_gles.c`): TEV as generated shaders, texture cache, EFB at 2x on Android
- [ ] EFB copy-to-texture effects, lines/points, audio (AX), Bink video

## Setup

1. Dump your disc (CleanRip) to ISO/CISO/RVZ.
2. Put the disc image (any supported format) in `orig/GE9E5D/`.
3. `python configure.py && ninja`

Expected `sys/main.dol` SHA-1: `94afc1057b78917c66ca37251ee998d5840dfd12`

See [docs/findings.md](docs/findings.md) for research notes.

Based on [dtk-template](https://github.com/encounter/dtk-template).
