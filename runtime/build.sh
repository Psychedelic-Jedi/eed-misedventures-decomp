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
$CC -O1 -w -c $INC "$DOLRECOMP/src/cpu/cpu.c" -o "$OUT/cpu.o"
$CC -O1 -Wall -c $INC "$HERE/hle.c" -o "$OUT/hle.o"
$CC -O1 -Wall -c $INC "$HERE/hw.c" -o "$OUT/hw.o"
$CC -O1 -Wall -c $INC "$HERE/disc.c" -o "$OUT/disc.o"
$CC -O1 -Wall -c $INC "$HERE/boot_test.c" -o "$OUT/boot_test.o"
$CC -o "$OUT/boot_test" "$OUT/boot_test.o" "$OUT/hle.o" "$OUT/hw.o" "$OUT/disc.o" "$OUT/cpu.o" "$OUT"/obj/*.o -lm
echo "built $OUT/boot_test"
