# Runtime (work in progress)

Host-side runtime for the statically recompiled game. Currently a Linux boot-test harness;
it will become the Android runtime (SDL + Aurora).

| File | Purpose |
|---|---|
| `boot_test.c` | Loads `main.dol`, sets up boot state (low-memory globals, MSR), runs recompiled code, handles `sc` / FP-unavailable exceptions, drives the time base from the host clock. |
| `hw.c` | Minimal MMIO register model for `0xCC000000` (read-back registers + "always ready" quirks), with deduplicated access logging. |
| `hle.c`, `hle_list.txt` | Native replacements for SDK functions. `tools/hle_patch.py` marks each listed function's first instruction with `mfspr r0, HID0`, which DolRecomp routes to the instruction fallback → `hle_dispatch()`. |
| `disc.c` | Disc reader for DVD emulation: plain ISO/GCM, CISO, or an extracted folder mapped through `sys/fst.bin`. |
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
10. **Next blockers** ⏳
    - The engine jumps into LU code loaded in the heap (`0x80442CA4`) → implement the loader hook
      (copy the pre-linked module from `gctool.py linkall` to its fixed address) and link module C into the runtime.
    - The async file reader copies `en/lvloader.gc` to `0x7E000000` (not RAM): destination pointer comes from
      an allocation/handle that isn't set up yet — investigate after modules run.

Run with a disc: `./build/recomp/boot_test main.dol 20 path/to/game.ciso` (or an extracted folder).
At timeout the harness prints OS threads, GX counters and interrupt state.

All 30 LU modules translate (814k instrs, 0 unknown) and compile (220 files); not yet linked into the runtime.
