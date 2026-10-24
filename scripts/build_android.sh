#!/data/data/com.termux/files/usr/bin/sh
# Build the Android APK on-device in Termux, from YOUR OWN disc image.
#
#   sh scripts/build_android.sh [path/to/game.ciso]
#
# Steps (each is skipped if its output already exists, so re-running is cheap):
#   1. build DolRecomp (static recompiler)
#   2. extract main.dol + level packs from the disc, verify main.dol
#   3. pre-link LU code modules (gctool.py linkall)
#   4. recompile PowerPC -> C (main.dol with HLE patches, 30 modules)
#   5. compile everything with clang for arm64 (JOBS=4 by default; ~1 hour first time)
#   6. link libeed.so and package + sign the APK -> Download/eed.apk
#
# Nothing derived from the game leaves your phone. Needs:
#   pkg install clang cmake make python openssl-tool git
#   and dtk (decomp-toolkit) in ~/.cargo/bin.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$HOME/eed-build}
JOBS=${JOBS:-4}
OPT=${OPT:--O1}
DL=$HOME/storage/downloads
export PATH="$PATH:$HOME/.cargo/bin"
DISC=${1:-$(ls "$DL"/*.ciso "$DL"/*.iso 2>/dev/null | head -1)}
[ -f "$DISC" ] || { echo "No disc image given or found in Download/"; exit 1; }
mkdir -p "$WORK"
echo "== disc: $DISC"
echo "== work: $WORK (jobs: $JOBS)"

# ---------------------------------------------------------------- 1. DolRecomp
DR=$WORK/DolRecomp
if [ ! -x "$DR/build/dolrecomp" ]; then
    echo "== [1/6] building DolRecomp"
    rm -rf "$DR"
    git clone --recursive --depth 1 https://github.com/ExpansionPak/DolRecomp "$DR"
    cmake -S "$DR" -B "$DR/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$DR/build" -j"$JOBS" --target dolrecomp
fi

# ---------------------------------------------------------------- 2. extract
if [ ! -f "$WORK/main.dol" ]; then
    echo "== [2/6] extracting main.dol and level packs"
    dtk vfs cp "$DISC:sys/main.dol" "$WORK/main.dol"
fi
want=94afc1057b78917c66ca37251ee998d5840dfd12
have=$(sha1sum "$WORK/main.dol" | cut -c1-40)
[ "$have" = "$want" ] || { echo "main.dol SHA-1 $have does not match the USA release ($want)"; exit 1; }
if [ ! -f "$WORK/gcs/.done" ]; then
    mkdir -p "$WORK/gcs"
    dtk vfs ls -r "$DISC:" | grep -oE 'files/eee/[^ ]+\.gc( |$)' | while read -r p; do
        case "$p" in */en/*|*/fr/*) continue;; esac   # localized packs hold no code
        n=$(basename "$p")
        [ -f "$WORK/gcs/$n" ] || dtk vfs cp "$DISC:$p" "$WORK/gcs/$n" >/dev/null
    done
    touch "$WORK/gcs/.done"
fi

# ---------------------------------------------------------------- 3. pre-link modules
if [ ! -f "$WORK/moddols/layout.txt" ]; then
    echo "== [3/6] pre-linking level code modules"
    python3 "$ROOT/tools/gctool.py" linkall "$WORK/gcs" "$ROOT/config/GE9E5D/exports.txt" "$WORK/moddols"
fi

# ---------------------------------------------------------------- 4. recompile to C
echo "== [4/6] recompiling PowerPC to C"
python3 "$ROOT/tools/hle_patch.py" "$WORK/main.dol" "$ROOT/config/GE9E5D/symbols.txt" \
    "$ROOT/runtime/hle_list.txt" "$WORK/main_hle.dol" "$WORK/hle_table.h"
if [ ! -f "$WORK/dol/generated/generated.h" ] || [ "$WORK/main_hle.dol" -nt "$WORK/dol/generated/generated.h" ]; then
    "$DR/build/dolrecomp" --gamecube --cpu gekko -j"$JOBS" "$WORK/main_hle.dol" "$WORK/dol" > "$WORK/dolrecomp.log"
fi
: > "$WORK/modules_table.h"
while read -r name base _t text _s size; do
    MG="$WORK/mods/$name/generated"
    if [ ! -f "$MG/generated.h" ]; then
        "$DR/build/dolrecomp" --gamecube --cpu gekko "$WORK/moddols/$name.dol" "$WORK/mods/$name" > "$WORK/mods_$name.log"
    fi
    printf '#include "generated.h"\nDolRecompFunction mod_%s_find(u32 a) { return dolrecomp_find_original(a); }\n' "$name" > "$WORK/mods/$name/find.c"
    echo "MODULE($name, $base, $size)" >> "$WORK/modules_table.h"
done < "$WORK/moddols/layout.txt"

# ---------------------------------------------------------------- 5. compile
echo "== [5/6] compiling (this is the long part; safe to re-run if interrupted)"
CC="clang --target=aarch64-linux-android26 -fPIC $OPT -w"
mkdir -p "$WORK/obj"
{
    for c in "$WORK"/dol/generated/chunks/*.c "$WORK/dol/generated/generated.c"; do
        o="$WORK/obj/main_$(basename "$c" .c).o"
        [ -f "$o" ] && [ "$o" -nt "$c" ] || echo "$c $o -I$WORK/dol/generated"
    done
    while read -r name _rest; do
        for c in "$WORK/mods/$name"/generated/chunks/*.c "$WORK/mods/$name/find.c"; do
            o="$WORK/obj/${name}_$(basename "$c" .c).o"
            [ -f "$o" ] && [ "$o" -nt "$c" ] || echo "$c $o -I$WORK/mods/$name/generated"
        done
    done < "$WORK/moddols/layout.txt"
} > "$WORK/todo.txt"
total=$(wc -l < "$WORK/todo.txt")
echo "   $total files to compile"
xargs -r -P "$JOBS" -L 1 sh -c "$CC -I$DR/src -c \"\$0\" -o \"\$1.tmp\" \"\$2\" && mv \"\$1.tmp\" \"\$1\" && echo \"   done \$(basename \$0)\"" < "$WORK/todo.txt"

# runtime (memory layer with aliasing + host regions, same patch as runtime/build.sh)
python3 - "$DR/src/cpu/cpu.c" "$WORK/cpu_alias.c" <<'PY'
import sys
s = open(sys.argv[1]).read()
hook = "static u8* resolve_addr(CPUState* cpu, u32 addr, u32* avail) {"
assert hook in s, "resolve_addr signature changed in DolRecomp"
s = '#include "alias.h"\n' + s.replace(hook, hook + "\n    addr = alias_translate(addr);\n    { u8 *hr = host_region_resolve(addr, avail); if (hr) return hr; }", 1)
open(sys.argv[2], "w").write(s)
PY
RT="$ROOT/runtime"
RCC="clang --target=aarch64-linux-android26 -fPIC -O2 -I$DR/src -I$DR/src/cpu -I$WORK/dol/generated -I$WORK -I$RT"
mkdir -p "$WORK/rt"
for f in core hle hw disc irq gx_fifo gx_render modules input card; do $RCC -c "$RT/$f.c" -o "$WORK/rt/$f.o"; done
$RCC -w -c "$WORK/cpu_alias.c" -o "$WORK/rt/cpu.o"
$RCC -c "$RT/android/android_main.c" -o "$WORK/rt/android_main.o"

# embedded data: main.dol + pre-linked modules
{
    echo '.section .rodata'
    echo '.global eed_main_dol'; echo '.balign 32'; echo 'eed_main_dol:'; echo ".incbin \"$WORK/main.dol\""
    echo '.global eed_main_dol_size'; echo '.balign 4'; echo 'eed_main_dol_size:'; echo ".4byte $(stat -c %s "$WORK/main.dol")"
    while read -r name _rest; do
        echo ".global eedmod_$name"; echo '.balign 32'; echo "eedmod_$name:"; echo ".incbin \"$WORK/moddols/$name.dol\""
    done < "$WORK/moddols/layout.txt"
} > "$WORK/embed.S"
{
    echo '#include "platform.h"'
    echo 'typedef struct { const char *name; const u8 *data; u32 size; } EmbeddedModule;'
    while read -r name _rest; do echo "extern const u8 eedmod_$name[];"; done < "$WORK/moddols/layout.txt"
    echo 'const EmbeddedModule eed_modules[] = {'
    while read -r name _rest; do echo "    {\"$name\", eedmod_$name, $(stat -c %s "$WORK/moddols/$name.dol")},"; done < "$WORK/moddols/layout.txt"
    echo '};'
    echo "const int eed_module_count = $(wc -l < "$WORK/moddols/layout.txt");"
} > "$WORK/embed_table.c"
$RCC -c "$WORK/embed.S" -o "$WORK/rt/embed.o"
$RCC -c "$WORK/embed_table.c" -o "$WORK/rt/embed_table.o"

# ---------------------------------------------------------------- 6. link + APK
echo "== [6/6] linking and packaging"
mkdir -p "$WORK/apk/lib/arm64-v8a"
clang --target=aarch64-linux-android26 -shared -o "$WORK/apk/lib/arm64-v8a/libeed.so" \
    "$WORK"/rt/*.o "$WORK"/obj/*.o \
    /system/lib64/libandroid.so /system/lib64/libEGL.so /system/lib64/libGLESv2.so /system/lib64/liblog.so \
    -lm -Wl,-z,max-page-size=16384 -Wl,--gc-sections
echo "   libeed.so: $(du -h "$WORK/apk/lib/arm64-v8a/libeed.so" | cut -f1)"
echo "   dependencies:"
llvm-readelf -d "$WORK/apk/lib/arm64-v8a/libeed.so" 2>/dev/null | grep -E "NEEDED|RPATH|RUNPATH" | sed 's/^/     /' || true

# binary manifest compiled directly (Termux's android.jar is a stub, so aapt can't be used)
python3 "$ROOT/tools/axml.py" "$ROOT/runtime/android/AndroidManifest.xml" "$WORK/AndroidManifest.bin"
python3 "$ROOT/tools/apk_pack.py" - "$WORK/unsigned.apk" "AndroidManifest.xml=$WORK/AndroidManifest.bin" \
    "lib/arm64-v8a/libeed.so=$WORK/apk/lib/arm64-v8a/libeed.so"
# signing key made with openssl (keytool needs a full Java install)
command -v openssl >/dev/null || { echo "openssl missing: run  pkg install openssl-tool"; exit 1; }
if [ ! -f "$WORK/cert.pem" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/key.pem" -out "$WORK/cert.pem" \
        -days 10000 -subj "/CN=eed-port"
fi
# v2 signature written in Python (Termux's apksigner aborts on recent Android)
rm -f "$DL/eed.apk"
python3 "$ROOT/tools/apk_sign.py" "$WORK/unsigned.apk" "$DL/eed.apk" "$WORK/key.pem" "$WORK/cert.pem"
echo
echo "== done: $DL/eed.apk"
echo "   Install it from your Files app, then: Settings > Apps > Ed, Edd n Eddy > allow 'All files access'."
