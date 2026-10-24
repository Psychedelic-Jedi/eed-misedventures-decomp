#!/usr/bin/env python3
"""Assemble an APK from an aapt2-linked base (manifest + resources) and native libs,
with the alignment Android requires (stored entries 4-byte aligned, resources.arsc
stored uncompressed). Replaces zipalign, which Termux doesn't ship.

Usage: apk_pack.py <base.apk> <out.apk> <zip_path>=<file> ...
"""
import sys
import zipfile
import zlib


def main():
    base, out, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
    with zipfile.ZipFile(base) as zin, open(out, "wb") as raw:
        zout = zipfile.ZipFile(raw, "w")

        def add(name, data, store):
            info = zipfile.ZipInfo(name, date_time=(2005, 11, 3, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            if store:
                # pad the local header's extra field so the data starts 4-byte aligned
                # (4096 for .so files so they could be mapped directly)
                align = 4096 if name.endswith(".so") else 4
                hdr = raw.tell() + 30 + len(name.encode())
                info.extra = b"\0" * ((align - hdr % align) % align)
            zout.writestr(info, data, compresslevel=6)

        for item in zin.infolist():
            data = zin.read(item.filename)
            add(item.filename, data, store=item.filename == "resources.arsc"
                or item.compress_type == zipfile.ZIP_STORED)
        for spec in extra:
            path_in_zip, src = spec.split("=", 1)
            with open(src, "rb") as f:
                add(path_in_zip, f.read(), store=False)
        zout.close()
    print("wrote", out)


if __name__ == "__main__":
    main()
