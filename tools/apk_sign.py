#!/usr/bin/env python3
"""Sign an APK with APK Signature Scheme v2 (enough for minSdk >= 24).

Pure Python plus the `openssl` command for the RSA signature, so it works in
Termux where apksigner (which needs the Android framework) aborts.

Usage: apk_sign.py <unsigned.apk> <signed.apk> <key.pem> <cert.pem>
"""
import hashlib
import struct
import subprocess
import sys
import tempfile
import os

ALG_RSA_PKCS1_SHA256 = 0x0103
V2_BLOCK_ID = 0x7109871A
CHUNK = 1 << 20


def lp(b):  # uint32 length-prefixed
    return struct.pack("<I", len(b)) + b


def find_eocd(d):
    i = d.rfind(b"PK\x05\x06")
    if i < 0 or len(d) - i < 22:
        raise SystemExit("not a zip (no end of central directory)")
    cd_size, cd_off = struct.unpack_from("<II", d, i + 12)
    return i, cd_off, cd_size


def digest_sections(sections):
    chunks = []
    for s in sections:
        for o in range(0, len(s), CHUNK):
            c = s[o:o + CHUNK]
            chunks.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(c)) + c).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(chunks)) + b"".join(chunks)).digest()


def openssl(*args, data=None):
    return subprocess.run(["openssl", *args], input=data, check=True, capture_output=True).stdout


def main():
    src, dst, key, cert = sys.argv[1:5]
    d = open(src, "rb").read()
    eocd, cd_off, cd_size = find_eocd(d)
    if cd_off + cd_size != eocd:
        raise SystemExit("unexpected zip layout (data between central directory and EOCD)")
    if d[cd_off - 16:cd_off] == b"APK Sig Block 42":
        raise SystemExit("input is already signed")
    entries, cdir, eocd_b = d[:cd_off], d[cd_off:eocd], d[eocd:]

    digest = digest_sections([entries, cdir, eocd_b])
    cert_der = openssl("x509", "-in", cert, "-outform", "DER")
    pub_der = openssl("pkey", "-pubin", "-outform", "DER",
                      data=openssl("x509", "-in", cert, "-noout", "-pubkey"))

    signed_data = (lp(lp(struct.pack("<I", ALG_RSA_PKCS1_SHA256) + lp(digest)))  # digests
                   + lp(lp(cert_der))                                              # certificates
                   + lp(b""))                                                       # attributes
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(signed_data)
        tmp = f.name
    try:
        sig = openssl("dgst", "-sha256", "-sign", key, tmp)
    finally:
        os.unlink(tmp)
    signer = (lp(signed_data)
              + lp(lp(struct.pack("<I", ALG_RSA_PKCS1_SHA256) + lp(sig)))
              + lp(pub_der))
    v2_value = lp(lp(signer))

    pair = struct.pack("<I", V2_BLOCK_ID) + v2_value
    pairs = struct.pack("<Q", len(pair)) + pair
    block_size = len(pairs) + 8 + 16          # everything after the leading size field
    block = struct.pack("<Q", block_size) + pairs + struct.pack("<Q", block_size) + b"APK Sig Block 42"

    new_eocd = bytearray(eocd_b)
    struct.pack_into("<I", new_eocd, 16, cd_off + len(block))
    with open(dst, "wb") as f:
        f.write(entries + block + cdir + bytes(new_eocd))
    print("signed", dst)


if __name__ == "__main__":
    main()
