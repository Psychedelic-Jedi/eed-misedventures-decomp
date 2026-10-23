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
