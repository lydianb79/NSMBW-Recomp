#!/usr/bin/env python3
"""
xref.py - string cross-references for naming functions in a symbol-less binary.

For every function found by recomp.discover(), constant addresses formed with
lis/addi, lis/ori or r2/r13 small-data offsets are resolved; those that point
at printable C strings are reported. SDK functions often reference their own
name in assert / panic / OSReport strings, which makes them easy to identify.

    python3 tools/xref.py build                     # dump all  func -> strings
    python3 tools/xref.py build --grep OSLink DVD   # only matching strings
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import image   # noqa: E402
import recomp  # noqa: E402
from ppc import sext16  # noqa: E402


def cstring(prog, a, maxlen=160):
    out = bytearray()
    while len(out) < maxlen:
        w = prog.read32(a & ~3)
        if w is None:
            return None
        b = (w >> (8 * (3 - (a & 3)))) & 0xFF
        if b == 0:
            break
        if not (32 <= b < 127 or b in (9, 10)):
            return None
        out.append(b)
        a += 1
    if len(out) < 4:
        return None
    return out.decode("ascii")


def function_strings(prog, f):
    regs = {}
    found = []
    for a in range(f.start, f.end, 4):
        i = prog.ins[a]
        n = i.name
        v = None
        if n == "addis" and i.a == 0:
            regs[i.d] = i.imm << 16
            continue
        if n == "addi":
            if i.a in regs:
                v = (regs[i.a] + sext16(i.imm)) & 0xFFFFFFFF
            elif i.a in prog.sda:
                v = (prog.sda[i.a] + sext16(i.imm)) & 0xFFFFFFFF
            if v is not None:
                regs[i.d] = v
        elif n == "ori" and i.d in regs:
            v = regs[i.d] | i.imm
            regs[i.a] = v
        elif n in ("lwz", "stw", "lbz", "lhz", "lfs", "lfd") and i.a in regs:
            pass
        else:
            continue
        if v is not None:
            s = cstring(prog, v)
            if s:
                found.append((a, v, s))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--grep", nargs="*")
    ap.add_argument("--json", help="write {func: [strings]} here")
    args = ap.parse_args()
    segs = image.read_image(os.path.join(args.build, "image.bin"))
    info = json.load(open(os.path.join(args.build, "image.json")))
    prog = recomp.Program(segs, info)
    funcs = recomp.discover(prog, verbose=False)
    pats = [re.compile(g) for g in (args.grep or [])]
    res = {}
    for st in sorted(funcs):
        strs = function_strings(prog, funcs[st])
        if not strs:
            continue
        if pats and not any(p.search(s) for _, _, s in strs for p in pats):
            continue
        res["%08X" % st] = [s for _, _, s in strs]
        print("%08X:" % st)
        for (a, v, s) in strs:
            print("    %08X -> %08X  %r" % (a, v, s[:100]))
    if args.json:
        json.dump(res, open(args.json, "w"), indent=1)


if __name__ == "__main__":
    main()
