#!/usr/bin/env python3
"""Compile AndroidManifest.xml to Android binary XML (AXML) without aapt/android.jar.

Only what a resource-less app needs: android:* attributes are mapped to their
framework resource IDs from the table below; values are typed as string, int,
hex flags, boolean or @android:style references.

Usage: axml.py AndroidManifest.xml out.bin      (compile)
       axml.py --dump out.bin                    (decode, for checking)
"""
import struct
import sys
import xml.etree.ElementTree as ET

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# android.R.attr IDs
ATTR_IDS = {
    "theme": 0x01010000, "label": 0x01010001, "name": 0x01010003, "hasCode": 0x0101000C,
    "exported": 0x01010010, "screenOrientation": 0x0101001E, "configChanges": 0x0101001F,
    "value": 0x01010024, "minSdkVersion": 0x0101020C, "versionCode": 0x0101021B,
    "versionName": 0x0101021C, "targetSdkVersion": 0x01010270, "glEsVersion": 0x01010281,
    "required": 0x0101028E, "largeHeap": 0x0101035A, "extractNativeLibs": 0x010104EA,
    "requestLegacyExternalStorage": 0x01010603,
}
STYLES = {"Theme.NoTitleBar.Fullscreen": 0x01030007, "Theme.NoTitleBar": 0x01030006}
ORIENTATION = {"unspecified": -1, "landscape": 0, "portrait": 1, "user": 2, "behind": 3, "sensor": 4,
               "nosensor": 5, "sensorLandscape": 6, "sensorPortrait": 7}
CONFIG = {"mcc": 0x1, "mnc": 0x2, "locale": 0x4, "touchscreen": 0x8, "keyboard": 0x10,
          "keyboardHidden": 0x20, "navigation": 0x40, "orientation": 0x80, "screenLayout": 0x100,
          "uiMode": 0x200, "screenSize": 0x400, "smallestScreenSize": 0x800}
BOOLEAN_ATTRS = {"hasCode", "exported", "required", "largeHeap", "extractNativeLibs",
                 "requestLegacyExternalStorage"}
INT_ATTRS = {"minSdkVersion", "targetSdkVersion", "versionCode"}

T_REF, T_STRING, T_DEC, T_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12


def typed(name, text):
    """Return (dataType, data, keep_raw_string)."""
    if name in BOOLEAN_ATTRS:
        return T_BOOL, 0xFFFFFFFF if text == "true" else 0, False
    if name in INT_ATTRS:
        return T_DEC, int(text), False
    if name == "glEsVersion":
        return T_HEX, int(text, 16), False
    if name == "screenOrientation":
        return T_DEC, ORIENTATION[text] & 0xFFFFFFFF, False
    if name == "configChanges":
        v = 0
        for f in text.split("|"):
            v |= CONFIG[f]
        return T_HEX, v, False
    if text.startswith("@android:style/"):
        return T_REF, STYLES[text.split("/", 1)[1]], False
    return T_STRING, None, True


def compile_manifest(src):
    tree = ET.parse(src)
    root = tree.getroot()
    # collect attribute names (android ones first, in resource-map order) and other strings
    android_attrs, other = [], []

    def walk(e):
        for k in e.attrib:
            ns, _, local = k[1:].partition("}") if k.startswith("{") else ("", "", k)
            if ns == ANDROID_NS:
                if local not in ATTR_IDS:
                    raise SystemExit(f"unknown android attribute {local}")
                if local not in android_attrs:
                    android_attrs.append(local)
        for c in e:
            walk(c)
    walk(root)
    android_attrs.sort(key=lambda a: ATTR_IDS[a])
    strings = list(android_attrs)

    def sidx(s):
        if s not in strings:
            strings.append(s)
        return strings.index(s)

    sidx("android")
    sidx(ANDROID_NS)
    body = b""

    def chunk(t, hsize, payload):
        return struct.pack("<HHI", t, hsize, 8 + len(payload)) + payload

    body += chunk(0x0100, 16, struct.pack("<IiII", 1, -1, sidx("android"), sidx(ANDROID_NS)))

    def element(e):
        nonlocal body
        attrs = []
        for k, v in e.attrib.items():
            if k.startswith("{"):
                ns, _, local = k[1:].partition("}")
            else:
                ns, local = "", k
            is_android = ns == ANDROID_NS
            dtype, data, raw = typed(local, v) if is_android else (T_STRING, None, True)
            rid = ATTR_IDS.get(local, 0) if is_android else 0
            if dtype == T_STRING:
                data = sidx(v)
            attrs.append((rid, sidx(ANDROID_NS) if is_android else 0xFFFFFFFF, sidx(local),
                          sidx(v) if raw else 0xFFFFFFFF, dtype, data))
        attrs.sort(key=lambda a: a[0])           # framework expects resource-id order
        ext = struct.pack("<iIHHHHHH", -1, sidx(e.tag), 0x14, 0x14, len(attrs), 0, 0, 0)
        for _rid, ns, name, raw, dtype, data in attrs:
            ext += struct.pack("<IIIHBBI", ns, name, raw, 8, 0, dtype, data & 0xFFFFFFFF)
        body += chunk(0x0102, 16, struct.pack("<Ii", 1, -1) + ext)
        for c in e:
            element(c)
        body += chunk(0x0103, 16, struct.pack("<IiiI", 1, -1, -1, sidx(e.tag)))

    element(root)
    body += chunk(0x0101, 16, struct.pack("<IiII", 1, -1, sidx("android"), sidx(ANDROID_NS)))

    # string pool (UTF-16)
    data, offsets = b"", []
    for s in strings:
        offsets.append(len(data))
        enc = s.encode("utf-16-le")
        data += struct.pack("<H", len(s)) + enc + b"\0\0"
    data += b"\0" * ((-len(data)) % 4)
    hdr = 28
    pool = struct.pack("<IIIII", len(strings), 0, 0, hdr + 4 * len(strings), 0)
    pool += b"".join(struct.pack("<I", o) for o in offsets) + data
    pool_chunk = struct.pack("<HHI", 0x0001, hdr, 8 + len(pool)) + pool
    resmap = b"".join(struct.pack("<I", ATTR_IDS[a]) for a in android_attrs)
    res_chunk = struct.pack("<HHI", 0x0180, 8, 8 + len(resmap)) + resmap
    payload = pool_chunk + res_chunk + body
    return struct.pack("<HHI", 0x0003, 8, 8 + len(payload)) + payload


def dump(path):
    d = open(path, "rb").read()
    t, hs, size = struct.unpack_from("<HHI", d, 0)
    assert t == 3 and size == len(d), "bad header"
    off = 8
    strings = []
    depth = 0
    while off < len(d):
        t, hs, sz = struct.unpack_from("<HHI", d, off)
        if t == 0x0001:
            n, _, flags, sstart, _ = struct.unpack_from("<IIIII", d, off + 8)
            for i in range(n):
                o = struct.unpack_from("<I", d, off + 28 + 4 * i)[0] + off + sstart
                ln = struct.unpack_from("<H", d, o)[0]
                strings.append(d[o + 2:o + 2 + 2 * ln].decode("utf-16-le"))
        elif t == 0x0180:
            ids = struct.unpack_from(f"<{(sz - 8) // 4}I", d, off + 8)
            print("resmap:", ", ".join(f"{strings[i]}={v:#010x}" for i, v in enumerate(ids)))
        elif t == 0x0102:
            name = strings[struct.unpack_from("<I", d, off + 20)[0]]
            cnt = struct.unpack_from("<H", d, off + 28)[0]
            parts = []
            for i in range(cnt):
                ns, nm, raw, _s, _r, dt, val = struct.unpack_from("<IIIHBBI", d, off + 36 + 20 * i)
                v = strings[val] if dt == T_STRING else hex(val)
                parts.append(f"{'android:' if ns != 0xFFFFFFFF else ''}{strings[nm]}={v}")
            print("  " * depth + f"<{name} " + " ".join(parts) + ">")
            depth += 1
        elif t == 0x0103:
            depth -= 1
        off += sz


if __name__ == "__main__":
    if sys.argv[1] == "--dump":
        dump(sys.argv[2])
    else:
        open(sys.argv[2], "wb").write(compile_manifest(sys.argv[1]))
        print("wrote", sys.argv[2])
