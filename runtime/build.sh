#!/bin/sh
# Incremental build of the boot test. Needs: DolRecomp built at $DOLRECOMP, the
# original main.dol at $DOL. Generated code goes to $OUT (never commit it).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
DOLRECOMP=${DOLRECOMP:-$HOME/eed/ext_DolRecomp}
DOL=${DOL:-$ROOT/orig/GE9E5D/sys/main.dol}
OUT=${OUT:-$ROOT/build/recomp}
CC=${CC:-gcc}
mkdir -p "$OUT/obj"
python3 "$ROOT/tools/hle_patch.py" "$DOL" "$ROOT/config/GE9E5D/symbols.txt" "$HERE/hle_list.txt" \
    "$OUT/main_hle.dol" "$OUT/hle_table.h"
"$DOLRECOMP/build/dolrecomp" --gamecube --cpu gekko -j"$(nproc)" "$OUT/main_hle.dol" "$OUT/dol" > "$OUT/dolrecomp.log"
G="$OUT/dol/generated"
INC="-I$DOLRECOMP/src -I$G -I$OUT -I$HERE"
# Recompile only chunks whose source changed (content hash).
for c in "$G"/chunks/*.c "$G/generated.c"; do
    n=$(basename "$c" .c); h=$(sha1sum "$c" | cut -c1-16)
    if [ ! -f "$OUT/obj/$n.$h.o" ]; then
        rm -f "$OUT/obj/$n".*.o
        echo "$c $OUT/obj/$n.$h.o"
    fi
done | xargs -r -P "$(nproc)" -n 2 sh -c "$CC -O1 -w -c $INC \"\$0\" -o \"\$1\""
# ---- LU code modules (optional): MODDOLS = folder from `gctool.py linkall` ----
MODOBJS=""
: > "$OUT/modules_table.h"
if [ -n "$MODDOLS" ] && [ -f "$MODDOLS/layout.txt" ]; then
    mkdir -p "$OUT/mods" "$OUT/modobj"
    while read -r name base _t text _s size; do
        MG="$OUT/mods/$name/generated"
        if [ ! -f "$MG/generated.h" ] || [ "$MODDOLS/$name.dol" -nt "$MG/generated.h" ]; then
            "$DOLRECOMP/build/dolrecomp" --gamecube --cpu gekko "$MODDOLS/$name.dol" "$OUT/mods/$name" > "$OUT/mods/$name.log"
        fi
        for c in "$MG"/chunks/*.c; do
            n=${name}_$(basename "$c" .c); h=$(sha1sum "$c" | cut -c1-16)
            if [ ! -f "$OUT/modobj/$n.$h.o" ]; then
                rm -f "$OUT/modobj/$n".*.o
                echo "$c $OUT/modobj/$n.$h.o"
            fi
        done
        # exported lookup for this module's functions
        printf '#include "generated.h"\nDolRecompFunction mod_%s_find(u32 a) { return dolrecomp_find_original(a); }\n' "$name" > "$OUT/mods/$name/find.c"
        echo "MODULE($name, $base, $size)" >> "$OUT/modules_table.h"
    done < "$MODDOLS/layout.txt" > "$OUT/mod_todo.txt"
    xargs -r -P "$(nproc)" -n 2 sh -c "$CC -O1 -w -c -I$DOLRECOMP/src \"\$0\" -o \"\$1\" -I\$(dirname \$(dirname \"\$0\"))" < "$OUT/mod_todo.txt"
    while read -r name _rest; do
        $CC -O1 -w -c -I"$DOLRECOMP/src" -I"$OUT/mods/$name/generated" "$OUT/mods/$name/find.c" -o "$OUT/modobj/find_$name.o"
    done < "$MODDOLS/layout.txt"
    MODOBJS="$OUT/modobj/*.o"
    MODCFLAGS="-DHAVE_MODULES"
fi

# DolRecomp's memory layer + guest address aliasing (see alias.h)
python3 - "$DOLRECOMP/src/cpu/cpu.c" "$OUT/cpu_alias.c" <<'PY'
import sys
s = open(sys.argv[1]).read()
hook = "static u8* resolve_addr(CPUState* cpu, u32 addr, u32* avail) {"
assert hook in s, "resolve_addr signature changed in DolRecomp"
s = '#include "alias.h"\n' + s.replace(hook, hook + "\n    { u8 *fp = page_fast(addr, avail); if (fp) return fp; }\n    addr = alias_translate(addr);\n    { u8 *hr = host_region_resolve(addr, avail); if (hr) return hr; }", 1)
open(sys.argv[2], "w").write(s)
PY
$CC -O2 -w -c $INC -I"$DOLRECOMP/src/cpu" "$OUT/cpu_alias.c" -o "$OUT/cpu.o"
$CC -O1 -Wall -c $INC "$HERE/hle.c" -o "$OUT/hle.o"
$CC -O1 -Wall -c $INC "$HERE/hw.c" -o "$OUT/hw.o"
$CC -O1 -Wall -c $INC "$HERE/disc.c" -o "$OUT/disc.o"
$CC -O1 -Wall -c $INC "$HERE/irq.c" -o "$OUT/irq.o"
$CC -O2 -Wall -c $INC "$HERE/gx_fifo.c" -o "$OUT/gx_fifo.o"
$CC -O2 -Wall -c $INC "$HERE/gx_render.c" -o "$OUT/gx_render.o"
$CC -O2 -Wall -c $INC "$HERE/gx_gles.c" -o "$OUT/gx_gles.o"
$CC -O1 -Wall -c $INC "$HERE/modules.c" -o "$OUT/modules.o"
$CC -O1 -Wall -c $INC "$HERE/input.c" -o "$OUT/input.o"
$CC -O1 -Wall -c $INC "$HERE/card.c" -o "$OUT/card.o"
$CC -O1 -Wall -c $INC "$HERE/core.c" -o "$OUT/core.o"
$CC -O1 -Wall -c $INC "$HERE/boot_test.c" -o "$OUT/boot_test.o"
$CC -o "$OUT/boot_test" "$OUT/boot_test.o" "$OUT/core.o" "$OUT/hle.o" "$OUT/hw.o" "$OUT/disc.o" "$OUT/irq.o" "$OUT/gx_fifo.o" "$OUT/gx_render.o" "$OUT/gx_gles.o" "$OUT/modules.o" "$OUT/input.o" "$OUT/card.o" "$OUT/cpu.o" "$OUT"/obj/*.o $MODOBJS -lm -l:libEGL.so.1 -l:libGLESv2.so.2
echo "built $OUT/boot_test"
