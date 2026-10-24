# Research notes

## Disc
- Game ID `GE9E5D`, developer Artificial Mind & Movement (A2M), publisher Midway, 2005.
- `files/engine.ini` configures the engine: `StartingGameName = EEE`, `StartingLuName = LVLOADER`, `MaxFPS = 30`, languages EN/FR.
- `files/eee/<lu>/` — one folder per "LU" (load unit): `lvloader`, `mainmenu`, `global`, `gbl_api`, levels `w01m1_1` … `w02m2_4`, loading screens `ls_*`, hubs.
  - `<lu>.gc` — resource pack. **Contains compiled PowerPC code** (C, original paths like `z:\eee\production\local\loader\levelloader\objects\loadingscreen\source\loadingscreen.c`) plus assets.
  - `<lu>.gc.gca` — Havok packfile (collision meshes, rigid bodies, MOPP code).
  - `en/`, `fr/` — localized `.gc` / `.sbk`.
  - `.sbk` sound banks, `.str` dialogue streams, `.as` large per-scam streams (TBD).
- `files/MOVIES/*.bik` — Bink video.

## main.dol (engine, "Chimera")
SHA-1 `94afc1057b78917c66ca37251ee998d5840dfd12`, entry `0x8000E1DC`.

| Range | Contents |
|---|---|
| `0x80003100`, `0x80003C20` | Unusual low-memory data sections (embedded `engine` / `Build` resource headers) |
| `0x8000FD40`–`0x80121FC0` | Engine + game API (~1.1 MB) |
| `0x80121FC0`–~`0x8016B000` | MetroTRK, Dolphin SDK (builds Apr/May 2004, `0x2301`), MSL |
| ~`0x8016B000`–`0x8024A9A0` | Havok physics (~900 KB) |

- Third-party: Havok, Bink, libpng, zlib, GameSpy.
- Shared engine lineage with A2M's *Teen Titans* (`CLIENT.Ph.a2m-teentitans` string).
- ~600 engine API functions callable from LU code (e.g. `GameObjTranslate`, `SendMessageTo`); each stores its own name in a global for error reporting (see `fn_8009A464`).
- No symbol map shipped.

## Split notes
- `_rom_copy_info` begins at `0x8000E4E0` with two entries for the low sections; named `_rom_copy_info_low` to keep dtk happy.
- The compiler download (files.decomp.dev) is needed for matching builds; not required for porting.

## Code size survey (all 30 LU code modules)

```
total instrs 813855  after removing duplicates 587045  unique fns 4397
functions in >= 12 of 12 levels: 35 fns, 2801 instrs
functions in >= 6 of 12 levels: 91 fns, 10733 instrs
functions in >= 2 of 12 levels: 290 fns, 49600 instrs
gbl_api     179947 instrs,  173473 unique to this module
global      172420 instrs,  164678 unique to this module
pphub1       41833 instrs,   15877 unique to this module
w01m1_3      41600 instrs,   14649 unique to this module
w01m1_1      40748 instrs,   18156 unique to this module
w02m2_1      37421 instrs,   13573 unique to this module
w01m1_3b     37415 instrs,   14620 unique to this module
w02m2_2      37161 instrs,   16321 unique to this module
w02m2_3b     35261 instrs,   12973 unique to this module
w02m2_3a     33337 instrs,   12615 unique to this module
w01m1_2      30590 instrs,    8269 unique to this module
w01m1_4      29719 instrs,   16257 unique to this module
w01m1_2b     28722 instrs,   12921 unique to this module
mainmenu     28385 instrs,   27942 unique to this module
w02m2_4      27379 instrs,   17661 unique to this module
lvloader      3367 instrs,    3233 unique to this module
bnk_plr       1997 instrs,    1785 unique to this module
intro          806 instrs,     756 unique to this module
ls_w2m2        479 instrs,     129 unique to this module
ls_w1m3        479 instrs,     129 unique to this module
ls_intro       479 instrs,     129 unique to this module
ls_w1m3b       479 instrs,     129 unique to this module
ls_w1m4        479 instrs,     129 unique to this module
ls_w2m3a       479 instrs,     129 unique to this module
ls_w2m3b       479 instrs,     129 unique to this module
ls_w1m2        479 instrs,     129 unique to this module
ls_hub1        479 instrs,     129 unique to this module
ls_w2m4        479 instrs,     129 unique to this module
ls_w2m1        479 instrs,     129 unique to this module
ls_w1m1        478 instrs,     128 unique to this module
```

main.dol engine code (0x8000FD40-0x80121FC0) is ~280k instructions. Combined unique code to decompile (excluding SDK/Havok/libs): ~870k instructions.
Exact duplicates were detected by hashing function bytes with relocated fields zeroed; near-duplicates (same source, different struct offsets) are not counted, so real sharing is higher.
