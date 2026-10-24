#!/usr/bin/env python3
"""Tool for A2M Chimera engine .gc resource packs (Ed, Edd n Eddy: The Mis-Edventures).

Usage:
  gctool.py list    <file.gc>
  gctool.py extract <file.gc> <outdir>     # dump every resource as a raw file
  gctool.py elf     <file.gc> <outdir>     # convert code modules (type 0x26) to PPC ELF objects

Format notes (reverse engineered, see docs/gc_format.md):
  0x000  header (0x40): version 0x03010424, pack id, LU name, game name, ...
  0x040  256 x {u32 count, u32 offset} directory, indexed by resource type
  dir    count x {u32 resource_id, u32 record_offset}
         resource_id = (type << 24) | (pack_id << 12)? | index
  record 0x10 header {0xABABABAB, version, flags, resource_id} + 0x20 name + 0x10 pad
"""
import os
import struct
import sys

TYPE_NAMES = {
    0x13: "lu",
    0x18: "env",
    0x22: "texture",
    0x24: "unk24",
    0x26: "code",
    0x27: "class",
    0x28: "proxy",
    0x29: "objdef",
    0x2A: "object",
    0x2B: "api",
    0x3A: "unk3a",
    0x3B: "palette",
    0x3C: "unk3c",
    0x57: "unk57",
}


def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


class GcPack:
    def __init__(self, data):
        self.d = data
        self.version = u32(data, 0)
        self.lu_name = data[0x10:0x1C].split(b"\0")[0].decode()
        self.game = data[0x1C:0x2C].split(b"\0")[0].decode()
        self.resources = []  # (type, res_id, offset)
        for t in range(256):
            count, off = u32(data, 0x40 + t * 8), u32(data, 0x44 + t * 8)
            for i in range(count):
                rid, roff = u32(data, off + i * 8), u32(data, off + i * 8 + 4)
                self.resources.append((t, rid, roff))
        valid = sorted({r[2] for r in self.resources if 0 < r[2] < len(data)})
        self._ends = {a: b for a, b in zip(valid, valid[1:] + [len(data)])}

    def record(self, off):
        end = self._ends.get(off, len(self.d))
        name = self.d[off + 0x10:off + 0x30].split(b"\0")[0].decode("latin-1")
        return name, self.d[off:end]


class CodeModule:
    """Type 0x26 record: relocatable PowerPC code image."""

    def __init__(self, rec):
        self.rec = rec
        h = 0x40  # module header within record
        f = struct.unpack_from(">10I", rec, h)
        (_, self.image_size, img_off, nsym, sym_off, nimp, imp_off, nrel, rel_off,
         str_off) = f
        self.image = rec[h + img_off:h + img_off + self.image_size]
        self.syms = [u32(rec, h + sym_off + i * 4) for i in range(nsym)]
        self.imports = []
        strbase = h + str_off
        for i in range(nimp):
            v = u32(rec, h + imp_off + i * 4)
            idx, soff = v & 0xFFFF, v >> 16
            name = rec[strbase + soff:].split(b"\0")[0].decode()
            self.imports.append((idx, name))
        self.relocs = []
        for i in range(nrel):
            a, off, add = struct.unpack_from(">3I", rec, h + rel_off + i * 12)
            self.relocs.append((a >> 16, a & 0xFFFF, off, add))
        # Text starts at the earliest of: a bl target, an instruction carrying a
        # HA/LO/REL24 relocation (data never has those), or a stack-frame prologue.
        cands = [self.syms[t] for k, t, _, _ in self.relocs if k == 4 and self.syms[t]]
        cands += [(off & ~3) for k, _, off, _ in self.relocs if k in (2, 3, 4)]
        first = min(cands) if cands else 0
        img = self.image
        prologues = [o for o in range(0, len(img) - 3, 4) if img[o:o + 2] == b"\x94\x21"]
        earlier = [o for o in prologues if o <= first]
        if earlier:
            first = earlier[0]
        self.text_start = first
        self.prologues = [o for o in prologues if o >= first]


R_MAP = {1: 1, 2: 4, 3: 6, 4: 10, 5: 1}
# gc kind -> ELF: 1 ADDR32 (pointer tables), 2 ADDR16_LO, 3 ADDR16_HA, 4 REL24 (bl),
# 5 ADDR32 (switch jump tables; word pre-filled with a text-relative offset)


def write_elf(mod, path):
    ts = mod.text_start
    data_b, text_b = mod.image[:ts], mod.image[ts:]
    import_idx = {i: n for i, n in mod.imports}

    strtab = bytearray(b"\0")

    def s(name):
        o = len(strtab)
        strtab.extend(name.encode() + b"\0")
        return o

    # sections: 0 null, 1 .text, 2 .data, 3 .rela.text, 4 .rela.data, 5 .symtab, 6 .strtab, 7 .shstrtab
    syms = [(0, 0, 0, 0, 0, 0)]  # name, value, size, info, other, shndx
    syms.append((0, 0, 0, 3, 0, 1))  # section .text
    syms.append((0, 0, 0, 3, 0, 2))  # section .data
    nlocal_base = len(syms)
    symmap = {}
    defined = {v for i, v in enumerate(mod.syms) if i >= 7 and i not in import_idx and v}
    func_set = {mod.syms[t] for k, t, _, _ in mod.relocs if k == 4} | set(mod.prologues)
    # pointer-table (kind 1) targets inside text are callbacks
    func_set |= {mod.syms[t] for k, t, _, _ in mod.relocs if k == 1 and t != 1 and mod.syms[t] >= ts}
    func_set.add(ts)
    defined = sorted(defined | func_set)
    for v in defined:
        if v >= ts:
            nm = f"fn_{v:X}" if v in func_set else f"lbl_{v:X}"
            syms.append((s(nm), v - ts, 0, (1 << 4) | (2 if v in func_set else 0), 0, 1))
        else:
            syms.append((s(f"lbl_{v:X}"), v, 0, (1 << 4) | 1, 0, 2))
        symmap[v] = len(syms) - 1
    imp_sym = {}
    for idx, name in mod.imports:
        syms.append((s(name), 0, 0, (1 << 4), 0, 0))
        imp_sym[idx] = len(syms) - 1

    rela = {1: bytearray(), 2: bytearray()}
    img = bytearray(mod.image)
    for kind, tgt, off, add in mod.relocs:
        sec = 1 if off >= ts else 2
        roff = off - ts if sec == 1 else off
        if tgt in imp_sym:
            si, a = imp_sym[tgt], add
        else:
            v = add if tgt == 1 else mod.syms[tgt] + add
            base = mod.syms[tgt] if tgt != 1 else None
            if base in symmap:
                si, a = symmap[base], add
            elif v >= ts:
                si, a = 1, v - ts
            else:
                si, a = 2, v
        rela[sec] += struct.pack(">IIi", roff, (si << 8) | R_MAP[kind], a)
        # clear the field so the object has no baked-in addresses
        if kind == 4:
            w = u32(img, off) & ~0x03FFFFFC
            struct.pack_into(">I", img, off, w)
        elif kind in (2, 3):
            struct.pack_into(">H", img, off, 0)
        elif kind in (1, 5):
            struct.pack_into(">I", img, off, 0)
    data_b, text_b = bytes(img[:ts]), bytes(img[ts:])

    # globals must come after locals
    locals_ = [x for x in syms if (x[3] >> 4) == 0]
    globs = [x for x in syms if (x[3] >> 4) != 0]
    order = locals_ + globs
    remap = {syms.index(x): order.index(x) for x in syms}
    for sec in rela:
        b = rela[sec]
        for i in range(0, len(b), 12):
            o, info, a = struct.unpack_from(">IIi", b, i)
            struct.pack_into(">IIi", b, i, o, (remap[info >> 8] << 8) | (info & 0xFF), a)
    symtab = b"".join(struct.pack(">IIIBBH", *x) for x in order)

    shstr = bytearray(b"\0")
    names = {}
    for n in [".text", ".data", ".rela.text", ".rela.data", ".symtab", ".strtab", ".shstrtab"]:
        names[n] = len(shstr)
        shstr += n.encode() + b"\0"

    blobs = [text_b, data_b, bytes(rela[1]), bytes(rela[2]), symtab, bytes(strtab), bytes(shstr)]
    off = 0x34
    offs = []
    body = bytearray()
    for b in blobs:
        pad = (-(off + len(body))) % 16
        body += b"\0" * pad
        offs.append(off + len(body))
        body += b
    shoff = off + len(body)
    shoff += (-shoff) % 4
    body += b"\0" * (shoff - off - len(body))

    sh = [struct.pack(">10I", *([0] * 10))]
    sh.append(struct.pack(">10I", names[".text"], 1, 6, 0, offs[0], len(text_b), 0, 0, 4, 0))
    sh.append(struct.pack(">10I", names[".data"], 1, 3, 0, offs[1], len(data_b), 0, 0, 4, 0))
    sh.append(struct.pack(">10I", names[".rela.text"], 4, 0, 0, offs[2], len(blobs[2]), 5, 1, 4, 12))
    sh.append(struct.pack(">10I", names[".rela.data"], 4, 0, 0, offs[3], len(blobs[3]), 5, 2, 4, 12))
    sh.append(struct.pack(">10I", names[".symtab"], 2, 0, 0, offs[4], len(symtab), 6, len(locals_), 4, 16))
    sh.append(struct.pack(">10I", names[".strtab"], 3, 0, 0, offs[5], len(strtab), 0, 0, 1, 0))
    sh.append(struct.pack(">10I", names[".shstrtab"], 3, 0, 0, offs[6], len(shstr), 0, 0, 1, 0))

    ehdr = b"\x7fELF\x01\x02\x01" + b"\0" * 9 + struct.pack(
        ">HHIIIIIHHHHHH", 1, 20, 1, 0, 0, shoff, 0x80000000, 0x34, 0, 0, 40, len(sh), 7)
    with open(path, "wb") as fp:
        fp.write(ehdr + bytes(body) + b"".join(sh))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd, path = sys.argv[1], sys.argv[2]
    pack = GcPack(open(path, "rb").read())
    print(f"{path}: LU '{pack.lu_name}' game '{pack.game}' version {pack.version:#010x}, "
          f"{len(pack.resources)} resources")
    if cmd == "list":
        for t, rid, off in pack.resources:
            name, rec = pack.record(off) if 0 < off < len(pack.d) else ("-", b"")
            print(f"  {rid:08x} {TYPE_NAMES.get(t, hex(t)):8} @ {off:#08x} size {len(rec):#8x} {name}")
            if t == 0x26 and rec:
                m = CodeModule(rec)
                print(f"      code image {m.image_size:#x} (text from {m.text_start:#x}), "
                      f"{len(m.syms)} syms, {len(m.relocs)} relocs, imports {[n for _, n in m.imports]}")
        return
    out = sys.argv[3]
    os.makedirs(out, exist_ok=True)
    for t, rid, off in pack.resources:
        if not (0 < off < len(pack.d)):
            continue
        name, rec = pack.record(off)
        safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in name) or "unnamed"
        if cmd == "extract":
            fn = os.path.join(out, f"{rid:08x}_{TYPE_NAMES.get(t, hex(t))}_{safe}.bin")
            open(fn, "wb").write(rec)
        elif cmd == "elf" and t == 0x26:
            fn = os.path.join(out, f"{safe}.o")
            try:
                write_elf(CodeModule(rec), fn)
                print("  wrote", fn)
            except Exception as e:  # keep going so one bad module doesn't stop the batch
                import traceback
                print(f"  ERROR converting {name} ({rid:08x}): {e!r}")
                traceback.print_exc()


if __name__ == "__main__":
    main()
