#!/usr/bin/env python3
"""
image.py - DOL + REL loader and static linker.

Builds one flat guest-memory image from main.dol and the REL modules:
  * DOL text/data sections are placed where the DOL header says.
  * Every REL module gets a fixed, non-overlapping load address in a
    reserved "module arena" (default 0x81800000, directly above MEM1 so every
    REL24 branch between modules and the DOL stays within +-32 MiB),
    its sections are laid out with their required alignment, its BSS is
    reserved, and all relocations (including imports from other modules
    and from the DOL) are applied.
  * The result is written as  <out>/image.bin  (segment list) and
    <out>/image.json  (sections, modules, entry points).

At run time the HLE replacement for OSLink/OSLinkFixed simply maps the
module the game asks for onto these pre-linked addresses, so recompiled
code and the game's own pointers always agree.

Usage:
    python3 tools/image.py link game/sys/main.dol game/files/rels/*.rel -o build/
"""
import argparse
import json
import os
import struct
import sys

MODULE_ARENA = 0x81800000

R_PPC_NONE, R_PPC_ADDR32, R_PPC_ADDR24, R_PPC_ADDR16, R_PPC_ADDR16_LO, \
    R_PPC_ADDR16_HI, R_PPC_ADDR16_HA, R_PPC_ADDR14, R_PPC_ADDR14_T, \
    R_PPC_ADDR14_NT, R_PPC_REL24, R_PPC_REL14, R_PPC_REL14_T, R_PPC_REL14_NT = range(14)
R_DOLPHIN_NOP, R_DOLPHIN_SECTION, R_DOLPHIN_END, R_DOLPHIN_MRKREF = 201, 202, 203, 204


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


class Segment:
    def __init__(self, name, addr, data, exec_, bss=0, module=0):
        self.name, self.addr, self.data, self.exec, self.bss, self.module = name, addr, bytearray(data), exec_, bss, module

    @property
    def end(self):
        return self.addr + len(self.data) + self.bss

    def contains(self, a):
        return self.addr <= a < self.addr + len(self.data)


# ---------------------------------------------------------------------------
def load_dol(path):
    d = open(path, "rb").read()
    segs = []
    for i in range(18):
        off, addr, size = be32(d, i * 4), be32(d, 0x48 + i * 4), be32(d, 0x90 + i * 4)
        if not size:
            continue
        is_text = i < 7
        name = (".text%d" % i) if is_text else (".data%d" % (i - 7))
        segs.append(Segment(name, addr, d[off:off + size], is_text))
    bss_addr, bss_size, entry = be32(d, 0xD8), be32(d, 0xDC), be32(d, 0xE0)
    segs.append(Segment(".bss", bss_addr, b"", False, bss_size))
    return segs, entry, bss_addr, bss_size


class Rel:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path).split(".")[0]
        self.raw = d = open(path, "rb").read()
        (self.id, _, _, self.nsec, self.sec_off, self.name_off, self.name_size,
         self.version, self.bss_size, self.rel_off, self.imp_off, self.imp_size) = struct.unpack_from(">12I", d, 0)
        (self.prolog_sec, self.epilog_sec, self.unres_sec, self.bss_sec) = d[0x30:0x34]
        self.prolog, self.epilog, self.unresolved = struct.unpack_from(">III", d, 0x34)
        self.align = be32(d, 0x40) if self.version >= 2 else 32
        self.bss_align = be32(d, 0x44) if self.version >= 2 else 32
        self.fix_size = be32(d, 0x48) if self.version >= 3 else 0
        self.sections = []
        for i in range(self.nsec):
            off, size = struct.unpack_from(">II", d, self.sec_off + i * 8)
            self.sections.append({"exec": bool(off & 1), "off": off & ~3, "size": size})
        self.imports = [struct.unpack_from(">II", d, self.imp_off + i * 8) for i in range(self.imp_size // 8)]
        self.addr = [0] * self.nsec

    def relocations(self, imp_off):
        d = self.raw
        p = imp_off
        sec = 0
        off = 0
        while True:
            delta, typ, s, addend = struct.unpack_from(">HBBI", d, p)
            p += 8
            if typ == R_DOLPHIN_END:
                return
            if typ == R_DOLPHIN_SECTION:
                sec, off = s, 0
                continue
            off += delta
            if typ == R_DOLPHIN_NOP:
                continue
            yield sec, off, typ, s, addend


def layout(rels, base):
    """Assign fixed addresses to every REL section (text/data first, then bss)."""
    a = base
    for r in rels:
        r.base = a
        for i, s in enumerate(r.sections):
            if s["off"] == 0 and s["size"] == 0:
                continue
            if s["off"] == 0:          # BSS section
                continue
            al = 32 if s["exec"] else 8
            a = (a + al - 1) & ~(al - 1)
            r.addr[i] = a
            a += s["size"]
        for i, s in enumerate(r.sections):
            if s["off"] == 0 and s["size"]:
                al = max(8, r.bss_align)
                a = (a + al - 1) & ~(al - 1)
                r.addr[i] = a
                a += s["size"]
        a = (a + 0xFFF) & ~0xFFF
        r.end = a
    return a


class Memory:
    def __init__(self, segs):
        self.segs = segs

    def seg(self, a):
        for s in self.segs:
            if s.contains(a):
                return s
        raise KeyError("address 0x%08x not in any loaded segment" % a)

    def r32(self, a):
        s = self.seg(a)
        return be32(s.data, a - s.addr)

    def w32(self, a, v):
        s = self.seg(a)
        struct.pack_into(">I", s.data, a - s.addr, v & 0xFFFFFFFF)

    def w16(self, a, v):
        s = self.seg(a)
        struct.pack_into(">H", s.data, a - s.addr, v & 0xFFFF)


def apply_reloc(mem, where, typ, value, stats):
    if typ == R_PPC_ADDR32:
        mem.w32(where, value)
    elif typ == R_PPC_ADDR24:
        ins = mem.r32(where)
        mem.w32(where, (ins & 0xFC000003) | (value & 0x03FFFFFC))
    elif typ == R_PPC_ADDR16:
        mem.w16(where, value)
    elif typ == R_PPC_ADDR16_LO:
        mem.w16(where, value & 0xFFFF)
    elif typ == R_PPC_ADDR16_HI:
        mem.w16(where, value >> 16)
    elif typ == R_PPC_ADDR16_HA:
        mem.w16(where, ((value >> 16) + (1 if value & 0x8000 else 0)) & 0xFFFF)
    elif typ in (R_PPC_ADDR14, R_PPC_ADDR14_T, R_PPC_ADDR14_NT):
        ins = mem.r32(where)
        mem.w32(where, (ins & 0xFFFF0003) | (value & 0xFFFC))
    elif typ == R_PPC_REL24:
        d = (value - where) & 0xFFFFFFFF
        sd = d - (1 << 32) if d & 0x80000000 else d
        if not -0x2000000 <= sd < 0x2000000:
            stats["rel24_overflow"] = stats.get("rel24_overflow", 0) + 1
        ins = mem.r32(where)
        mem.w32(where, (ins & 0xFC000003) | (d & 0x03FFFFFC))
    elif typ in (R_PPC_REL14, R_PPC_REL14_T, R_PPC_REL14_NT):
        d = (value - where) & 0xFFFFFFFF
        ins = mem.r32(where)
        mem.w32(where, (ins & 0xFFFF0003) | (d & 0xFFFC))
    else:
        raise NotImplementedError("relocation type %d" % typ)
    stats[typ] = stats.get(typ, 0) + 1


def link(dol_path, rel_paths, base=MODULE_ARENA):
    segs, entry, bss_addr, bss_size = load_dol(dol_path)
    rels = [Rel(p) for p in rel_paths]
    # profile module first, like the game loads it
    rels.sort(key=lambda r: (0 if "profile" in r.name else 1, r.id))
    layout(rels, base)
    by_id = {r.id: r for r in rels}
    for r in rels:
        for i, s in enumerate(r.sections):
            if s["size"] == 0 or r.addr[i] == 0:
                continue
            nm = "%s.s%d" % (r.name, i)
            if s["off"]:
                segs.append(Segment(nm, r.addr[i], r.raw[s["off"]:s["off"] + s["size"]], s["exec"], 0, r.id))
            else:
                segs.append(Segment(nm, r.addr[i], b"", False, s["size"], r.id))
    mem = Memory(segs)
    stats = {}
    missing = set()
    for r in rels:
        for (mid, off) in r.imports:
            for (sec, o, typ, tsec, addend) in r.relocations(off):
                where = r.addr[sec] + o
                if mid == 0:
                    value = addend
                elif mid in by_id:
                    value = by_id[mid].addr[tsec] + addend
                else:
                    missing.add(mid)
                    continue
                apply_reloc(mem, where, typ, value, stats)
    info = {
        "entry": entry,
        "dol_bss": [bss_addr, bss_size],
        "module_arena": [base, max(r.end for r in rels) if rels else base],
        "segments": [{"name": s.name, "addr": s.addr, "size": len(s.data), "bss": s.bss,
                      "exec": s.exec, "module": s.module} for s in segs],
        "modules": [{
            "name": r.name, "id": r.id, "version": r.version,
            "sections": [{"addr": r.addr[i], "size": s["size"], "exec": s["exec"], "bss": s["off"] == 0}
                         for i, s in enumerate(r.sections)],
            "prolog": r.addr[r.prolog_sec] + r.prolog if r.prolog_sec else 0,
            "epilog": r.addr[r.epilog_sec] + r.epilog if r.epilog_sec else 0,
            "unresolved": r.addr[r.unres_sec] + r.unresolved if r.unres_sec else 0,
            "bss_size": r.bss_size, "fix_size": r.fix_size,
        } for r in rels],
        "reloc_stats": {str(k): v for k, v in stats.items()},
        "unresolved_imports": sorted(missing),
    }
    return segs, info


def write_image(segs, path):
    """image.bin: 'NSIM' u32 count, then per segment: addr, size, bss, flags, data."""
    with open(path, "wb") as f:
        f.write(b"NSIM" + struct.pack(">I", len(segs)))
        for s in segs:
            f.write(struct.pack(">IIII", s.addr, len(s.data), s.bss, (1 if s.exec else 0) | (s.module << 8)))
            f.write(s.data)


def read_image(path):
    d = open(path, "rb").read()
    assert d[:4] == b"NSIM"
    n = be32(d, 4)
    p = 8
    segs = []
    for _ in range(n):
        addr, size, bss, flags = struct.unpack_from(">IIII", d, p)
        p += 16
        segs.append(Segment("seg_%08x" % addr, addr, d[p:p + size], bool(flags & 1), bss, flags >> 8))
        p += size
    return segs


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("link")
    a.add_argument("dol")
    a.add_argument("rels", nargs="*")
    a.add_argument("-o", "--out", default="build")
    a.add_argument("--base", type=lambda x: int(x, 0), default=MODULE_ARENA)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    segs, info = link(args.dol, args.rels, args.base)
    write_image(segs, os.path.join(args.out, "image.bin"))
    with open(os.path.join(args.out, "image.json"), "w") as f:
        json.dump(info, f, indent=1)
    print("entry 0x%08x" % info["entry"])
    for s in info["segments"]:
        print("  %-22s %08x %8x%s%s" % (s["name"] if s["module"] == 0 else "mod%d %s" % (s["module"], s["name"]),
                                       s["addr"], s["size"] or s["bss"], " X" if s["exec"] else "", " bss" if s["bss"] else ""))
    for m in info["modules"]:
        print("module %-14s id %2d prolog %08x epilog %08x" % (m["name"], m["id"], m["prolog"], m["epilog"]))
    print("relocations:", info["reloc_stats"], "unresolved imports:", info["unresolved_imports"])
    if "rel24_overflow" in info["reloc_stats"]:
        sys.exit("error: REL24 branch out of range - choose a module arena closer to the DOL")


if __name__ == "__main__":
    main()
