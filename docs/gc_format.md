# `.gc` resource pack format (Chimera engine)

Reverse engineered from `files/eee/lvloader/lvloader.gc`. All values big-endian.
Tool: `tools/gctool.py` (`list`, `extract`, `elf`).

## File header (0x40 bytes)
| Off | Type | Meaning |
|---|---|---|
| 0x00 | u32 | Version `0x03010424` |
| 0x0C | u16 | Pack id (`0x0062` for lvloader) |
| 0x10 | char[12] | LU name (`lvloader`) |
| 0x1C | char[16] | Game name (`eee`) |
| 0x2C | u32 | Build timestamp? (`0x4346B8AA` = Oct 2005) |
| 0x38 | char[8] | `Build` |

## Type directory (0x40 .. 0x840)
256 entries of `{u32 count, u32 offset}`, indexed by resource type.
Each points to `count` entries of `{u32 resource_id, u32 record_offset}`.
`resource_id = type << 24 | 0x06 << 16 | 0x20 << 8 | index` (middle bits likely pack-related).
Offsets of `0` / negative values mean "not present in this pack".

## Records
Every record starts with:
```
u32 0xABABABAB   magic (debug-fill pattern)
u32 version      0x03010424 (palette: 0x02070064)
u32 flags
u32 resource_id
char name[0x20]
u8  pad[0x10]    0xEF fill
```
Texture records (type 0x22) have no `ABABABAB` magic.

| Type | Name | Notes |
|---|---|---|
| 0x13 | LU | load-unit descriptor |
| 0x18 | Env | environment (`dummy`) |
| 0x22 | Texture | `dialog_bg`, 0x10100 bytes |
| 0x26 | **Code module** | relocatable PPC code, see below |
| 0x27 | Class | per-class descriptor (`api_master_lu_mgr`, `loadingscreen`), self-relative pointers into code |
| 0x28 | Proxy | `proxy:loadingscreen`, `EnvProxy:dummy` → refs 0x29 |
| 0x29 | Object def | |
| 0x2A | Object instance | `LoadingScreen`, `env_dummy` |
| 0x2B | API | `api_master_lu_mgr` → refs 0x27 |
| 0x3B | Palette | |

## Code module (type 0x26)
Module header at record+0x40, offsets relative to it:
```
u32 unk (0)
u32 image_size
u32 image_offset      (0x40)
u32 num_symbols
u32 symbols_offset    u32[num_symbols]: image-relative offset of each symbol
u32 num_imports
u32 imports_offset    u32[num_imports]: (string_offset << 16) | symbol_index
u32 num_relocs
u32 relocs_offset     {u16 kind, u16 symbol, u32 image_offset, u32 addend}[num_relocs]
u32 strings_offset    NUL-terminated import names
```
- Image = data (+ strings) first, then text. Text starts at the lowest branch target.
- Symbols 0–6 are 0 (section placeholders); symbol 1 means "image base + addend".
- Reloc kinds: `2` = ADDR16_LO, `3` = ADDR16_HA, `4` = REL24 (`bl`), (`1` assumed ADDR32).
- Imports are resolved by name against engine globals: lvloader imports `gApiInput`, `gApiHud`.
  Engine services are reached through API structs of function pointers (e.g. `lbl_8` → `+0x58` is an error/assert reporter taking message, file, line).
- LU code style (r9–r11 scratch, `subfic/adde` idioms, no SDA) differs from main.dol (Metrowerks): LU modules are possibly built with a different compiler (GCC/ProDG?). Needs confirmation.

### Relocation kinds (confirmed on mainmenu.gc)
| Kind | ELF equivalent | Where | Notes |
|---|---|---|---|
| 1 | `R_PPC_ADDR32` | data | function-pointer tables (callbacks); word is 0 |
| 2 | `R_PPC_ADDR16_LO` | text | |
| 3 | `R_PPC_ADDR16_HA` | text | always on `lis` |
| 4 | `R_PPC_REL24` | text | `bl` |
| 5 | `R_PPC_ADDR32` | data | switch jump tables; word pre-filled with a text-relative offset |

`mainmenu` code: image 0x1F244 bytes, text from 0x36C0, ~190 functions, imports `memcpy`, `memset`.
