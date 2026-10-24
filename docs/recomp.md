# Static recompilation plan (Android port)

Goal: run the game natively on Android (ARM64) by statically translating its PowerPC code to C,
then replacing the GameCube hardware/SDK layer with native implementations.

## Pipeline

```
main.dol ───────────────────────────────► DolRecomp ─► C (engine, ~584k instrs)
files/eee/*/*.gc ─ gctool.py linkall ─► *.dol ─► DolRecomp ─► C (level code, ~814k instrs)
                       │
                       └─ config/GE9E5D/exports.txt (engine API addresses)
C + runtime (memory, OS/DVD/PAD/AX HLE, GX→Aurora, SDL) ─► ARM64 build ─► APK
```

- CPU translator: [DolRecomp](https://github.com/ExpansionPak/DolRecomp) (GPL-3.0), `--gamecube --cpu gekko`.
  Generated C requires its `src/cpu/cpu.h` runtime interface (`CPUState`, `mem_read*/mem_write*`, dispatch).
- The generated C is derived from the game and **must not be committed**; it is regenerated locally from the user's own disc.

## Level code layout

LU code modules are normally loaded into the heap at a dynamic address. For static recompilation each one is
pre-linked at a fixed address (`gctool.py linkall`), name-sorted, starting at `0x81800000`:
just above the 24 MB of MEM1 and within `bl` range (±32 MB) of `main.dol`, which modules call directly
(`memcpy`, `memset`). All 30 modules total ~7.4 MB, so they fit below `0x82000000` without overlapping.

The runtime therefore needs:
1. Guest RAM covering `0x80000000–0x82000000` (32 MB instead of 24 MB).
2. A hook on the engine's LU code loader so that, instead of relocating a module in place, it copies the
   pre-linked image to its fixed address. Loader chain in main.dol:
   - `fn_800BEDB0` → `fn_800FFA80`: resolves imports by name (`fn_800FF044` lookup in registry `lbl_802A9C84`).
   - `fn_800C1224`: streaming loader; calls `fn_800FF904(module, first, count)` in batches of 500 (0x1F4)
     relocations per step, checking a time budget between batches. `module` = code record + 0x40;
     `+0x08` image base, `+0x10` resolved symbol addresses, `+0x1C/+0x20` reloc count/table.
   - `fn_800FF904`: applies kinds 1–5, then `DCFlushRange`.
   Planned hook: replace `fn_800FF904` natively — on the first batch, copy the pre-linked image to its
   fixed address (looked up by module name in `layout.txt`) and point `+0x08`/`+0x10` there; later batches no-op.
3. Imports resolved by name through `fn_800FF51C` (export registry); addresses are static, see `exports.txt`.

## Status

- [x] main.dol → C: 584,472 instructions, 0 unknown. All chunks compile with GCC.
- [x] Level module → C: `mainmenu` linked at `0x81810000`, recompiled (28,385 instrs, 0 unknown), compiles.
- [x] Locate the LU loader in main.dol and design the hook (see above).
- [ ] Self-modifying-code sites flagged by DolRecomp (`generated_smc.txt`): review (likely cache/patch code in SDK/TRK).
- [ ] Runtime: memory, dispatch table for indirect calls (engine ↔ modules), OS HLE (heap, threads, alarms, time).
- [ ] DVD HLE reading from the disc image (dtk/nod formats incl. CISO).
- [ ] GX via Aurora; VI/framebuffer; Bink video.
- [ ] Audio (AX/DSP) and PAD → SDL input.
- [ ] Havok: runs as translated code (no replacement needed initially).
- [ ] Android packaging (NDK, SDL Android project).

## Open question: where builds happen
The translated C is copyrighted-derived output, so it can't be built in a public CI. Options:
build in a private workspace from the user's own files, or build on-device in Termux (clang is available).
