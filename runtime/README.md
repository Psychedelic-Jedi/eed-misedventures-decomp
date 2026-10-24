# Runtime (work in progress)

Host-side runtime for the statically recompiled game. Currently a Linux boot-test harness;
it will become the Android runtime (SDL + Aurora).

| File | Purpose |
|---|---|
| `boot_test.c` | Loads `main.dol`, sets up boot state (low-memory globals, MSR), runs recompiled code, handles `sc` / FP-unavailable exceptions, drives the time base from the host clock. |
| `hw.c` | Minimal MMIO register model for `0xCC000000` (read-back registers + "always ready" quirks), with deduplicated access logging. |
| `hle.c`, `hle_list.txt` | Native replacements for SDK functions. `tools/hle_patch.py` marks each listed function's first instruction with `mfspr r0, HID0`, which DolRecomp routes to the instruction fallback → `hle_dispatch()`. |
| `disc.c` | Disc reader for DVD emulation: plain ISO/GCM, CISO, or an extracted folder mapped through `sys/fst.bin`. |
| `gx_fifo.c`, `gx_render.c`, `gx.h` | GX command processor (BP/CP/XF state, display lists, PE interrupts) and software renderer. |
| `input.c` | Platform controller state → `PADRead`; `EED_INPUT="300-310:A,900-910:START"` for scripted tests. |
| `card.c` | Memory card: CARD API (probe/mount/open/create/read/write/delete/status/free blocks) on host files. |
| `build.sh` | Patch → DolRecomp → incremental compile (only changed chunks) → link `build/recomp/boot_test`. |

## Build & run (Linux, from repo root)
```
DOLRECOMP=/path/to/DolRecomp sh runtime/build.sh
./build/recomp/boot_test orig/GE9E5D/sys/main.dol 20   # 20 s timeout
```
The first build compiles ~150 MB of generated C (~20 min on 2 cores); later builds only recompile changed chunks.

## Boot progress
1. `__start` → `__init_registers`/`__init_data` → `OSInit`: caches, PI/MI/EXI/SI/VI init, thread init. ✅
2. `__OSInitAudioSystem` (HLE no-op). ✅
3. DVD (DI) init, AI init (sample counter modelled). ✅
4. `DSPInit` → `__DSP_boot_task` (HLE no-op). ✅
5. AX init: `DSPAddTask` HLE marks the ucode task running. ✅
6. `vprintf` HLE: OSReport/OSPanic output goes to stdout; game prints SDK banners and `Engine: 3.1.1060`. ✅
7. DVD emulation: DI command execution + PI interrupts; FST/disc ID/bi2 set up like the apploader. ✅
   Reads `engine.ini`, `icon.rgb`, `lvloader.gc`, `en/lvloader.gc` (128 KB chunks), `lvloader.gc.gca`.
8. Interrupts: external (PI), decrementer (OS alarms), VI retrace at 60 Hz, PE finish/token.
   `OSDisable/Enable/RestoreInterrupts` are HLE'd so pending interrupts fire as soon as MSR[EE] is set. ✅
9. GX command stream parsed (`gx_fifo.c`): BP/CP/XF/draw sizing from VCD/VAT; `GXDrawDone` completes. ✅
10. LU code modules run recompiled: `LU_FixupModuleHeader` HLE installs the pre-linked image (from
    `gctool.py linkall`, path in `$EED_MODULES`) and aliases the heap copy to it (`alias.h`), so pointers derived
    from the pack (class descriptors) and from the header reach the same memory and recompiled code;
    `LU_RelocateBatch` becomes a no-op. Cross-module calls go through `ctx->host_call` (`modules.c`). ✅
11. Dolphin SDK **VM library** (ARAM-backed virtual memory, 14 MB at `0x7E000000`): `VMInit`/`VMAlloc` HLE'd,
    the range is host memory (no page faults). ✅
12. **`lvloader` (loading screen / language select) runs its frame loop at 60 fps**: ~1,500 frames,
    ~8,000 draws in 25 s. ✅
13. **First rendered frame** (software reference renderer `gx_render.c`): vertex decode incl. indexed arrays and
    display lists, XF transform/projection/viewport, all GX texture formats (incl. CMPR, palettes via TMEM),
    TEV combiner, alpha test, blending, depth, scissor, EFB->XFB copies. Frames: `EED_FRAMES=dir` (PPM). ✅
14. Controller input: `PADRead` HLE fed by `input.c` (scripted via `EED_INPUT` for headless tests; SDL later).
    Pressing A on the language select makes the game load the next loading screen (`ls_intro`). ✅
15. ARAM (16 MB) + ARAM DMA with DSP interrupts; sound banks load. ✅
16. **Boot flow reaches the main menu**: Licensed by Nintendo → language select → autosave notice →
    memory card prompts → Bink/Havok logos → legal screen → **title screen (PRESS START)** → **main menu**
    (New Game / Load Game / Credits). Modules loaded: lvloader, ls_intro, gbl_api, intro, ls_hub1, mainmenu. ✅
    Test input: `EED_INPUT="300-310:A,700-710:A,950-1100:DOWN,1300-1310:A,1500-1600:UP,1750-1760:A,2400-2410:START"`
17. Memory card: CARD library replaced by host files (`card.c`, folder `$EED_MEMCARD`). The game finds a
    59-block card, offers to create save data, and New Game proceeds: global, pphub1 and bnk_plr modules load,
    then the first scam's movie and the World 1 Mission 1 loading screen are requested. ✅
18. Next ⏳
    - Lighting (XF light units) for correct colours.
    - GLES backend for Android reusing the decode/TEV front half.
    - PAD/SI input (language select waits for a button).
    - Null reads in `fn_800DCB38` (near Bink code).

Run: `EED_MODULES=path/to/linked_dols ./build/recomp/boot_test main.dol 20 path/to/game.ciso`
Build with modules: `MODDOLS=path/to/linked_dols DOLRECOMP=... sh runtime/build.sh`
