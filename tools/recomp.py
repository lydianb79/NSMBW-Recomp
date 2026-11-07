#!/usr/bin/env python3
"""
recomp.py - static recompiler: Gekko/Broadway PowerPC machine code -> C.

Input : build/image.bin + build/image.json   (from image.py link)
        config/functions.json (optional: names, extra entry points, HLE list)
Output: build/gen/ppc_func_*.c, ppc_funcs.h, ppc_func_table.c, functions.json

Every guest function becomes

    PPC_FUNC_IMPL(f_80012345) { ... }

taking (PPCContext *ctx, uint8_t *base). Direct `bl` become C calls; `b`
inside a function becomes `goto`; branches leaving the function become
guaranteed tail calls (clang musttail); `bctr` jump tables become C
`switch` statements; `bctrl`/`blrl` go through the runtime dispatch table.
Each function is emitted as a weak symbol, so a runtime HLE override with
the same name replaces it at link time (the original stays reachable as
__imp_f_XXXXXXXX).

Function discovery (no symbols are available for retail NSMBW):
  seeds = entry point, module prolog/epilog/unresolved, every `bl` target,
          32-bit words in data sections that point at plausible code starts
          (vtables, callbacks, ctor tables), user-listed entries.
  A function extends to the next seed. Branches that leave a function and
  hit a non-seed create a new seed; repeat to a fixed point.
"""
import argparse
import json
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import image  # noqa: E402
import ppc    # noqa: E402
from ppc import sext16, mask32  # noqa: E402


# ---------------------------------------------------------------------------
# analysis
# ---------------------------------------------------------------------------
class Program:
    def __init__(self, segs, info):
        self.segs = segs
        self.info = info
        self.exec = [s for s in segs if s.exec]
        self.data = [s for s in segs if not s.exec and len(s.data)]
        self.ins = {}
        for s in self.exec:
            for o in range(0, len(s.data) - 3, 4):
                a = s.addr + o
                self.ins[a] = ppc.decode(struct.unpack_from(">I", s.data, o)[0], a)
        self.sda = {}
        self.sda = find_sda_bases(self)

    def seg_of(self, a):
        for s in self.exec:
            if s.addr <= a < s.addr + len(s.data):
                return s
        return None

    def read32(self, a):
        for s in self.segs:
            if s.addr <= a < s.addr + len(s.data) - 3:
                return struct.unpack_from(">I", s.data, a - s.addr)[0]
        return None

    def is_code(self, a):
        return a in self.ins

    def plausible_start(self, a):
        if a & 3 or a not in self.ins:
            return False
        i = self.ins[a]
        if i.name is None:
            return False
        s = self.seg_of(a)
        if a == s.addr:
            return True
        p = self.ins.get(a - 4)
        return p is None or p.w == 0 or ppc.ends_block(p)


def resolve_const(prog, reg, at, limit=512, depth=0):
    """Best-effort backward constant propagation for `reg` just before `at`."""
    if reg == 0:
        return None
    if reg in (2, 13) and reg in prog.sda:
        return prog.sda[reg]
    if depth > 6:
        return None
    a = at - 4
    seg = prog.seg_of(at)
    stop = max(seg.addr, at - 4 * limit) if seg else at
    while a >= stop:
        i = prog.ins.get(a)
        if i is None:
            return None
        n = i.name
        if n == "b" and i.lk and reg not in range(14, 32):
            return None              # volatile register clobbered by a call
        writes = None
        if n in ("addi", "addis", "lwz", "lbz", "lhz", "lha", "mulli", "subfic", "lwzx"):
            writes = i.d
        elif n in ("ori", "oris", "rlwinm", "or", "and", "xori", "andi.", "srawi", "extsh", "extsb"):
            writes = i.a
        elif n in ("lwzu", "lbzu", "lhzu") and i.a == reg:
            return None
        if writes == reg:
            if n == "addi":
                base = 0 if i.a == 0 else resolve_const(prog, i.a, a, limit, depth + 1)
                return None if base is None else (base + sext16(i.imm)) & 0xFFFFFFFF
            if n == "addis":
                base = 0 if i.a == 0 else resolve_const(prog, i.a, a, limit, depth + 1)
                return None if base is None else (base + (i.imm << 16)) & 0xFFFFFFFF
            if n == "ori":
                base = resolve_const(prog, i.d, a, limit, depth + 1) if i.d != reg else None
                if base is None and i.d == reg:
                    base = resolve_const(prog, reg, a, limit, depth + 1)
                return None if base is None else base | i.imm
            if n == "or" and i.d == i.b:
                return resolve_const(prog, i.d, a, limit, depth + 1)
            return None
        a -= 4
    return None


def find_jump_table(prog, pc, start, end):
    """Recognise the MWCC switch idiom ending in `mtctr rX; bctr`.

        cmplwi  rI, N ; bgt default
        lis     rT, tbl@ha ; addi rT, rT, tbl@l     (may be far above)
        slwi    rO, rI, 2  ; lwzx rX, rT, rO ; mtctr rX ; bctr
    """
    ctr_reg = None
    lwzx_at = None
    a = pc - 4
    while a >= max(start, pc - 4 * 24):
        i = prog.ins[a]
        if ctr_reg is None:
            if i.name == "mtspr" and i.spr == 9:
                ctr_reg = i.d
        elif i.name == "lwzx" and i.d == ctr_reg:
            lwzx_at = a
            break
        a -= 4
    if lwzx_at is None:
        return None
    lw = prog.ins[lwzx_at]
    tbl = None
    for reg in (lw.a, lw.b):
        v = resolve_const(prog, reg, lwzx_at)
        if v is not None and prog.read32(v) is not None:
            tbl = v
            break
    if tbl is None:
        return None
    limit = None
    a = lwzx_at - 4
    while a >= max(start, lwzx_at - 4 * 24):
        i = prog.ins[a]
        if i.name == "cmpli":
            limit = i.imm + 1
            break
        a -= 4
    targets = []
    n = 0
    while n < (limit if limit is not None else 1024):
        v = prog.read32(tbl + n * 4)
        if v is None or not (start <= v < end) or v & 3:
            if limit is not None:
                return None
            break
        targets.append(v)
        n += 1
    if not targets:
        return None
    return tbl, targets


def find_sda_bases(prog):
    """r2/r13 are loaded once in __init_registers with lis/ori (or addi)."""
    sda = {}
    for a, i in prog.ins.items():
        if i.name == "addis" and i.a == 0 and i.d in (2, 13):
            j = prog.ins.get(a + 4)
            if j and j.d == i.d and j.name == "ori" and j.a == i.d:
                pass
            if j and j.name == "ori" and j.d == i.d and j.a == i.d:
                v = (i.imm << 16) | j.imm
            elif j and j.name == "addi" and j.d == i.d and j.a == i.d:
                v = ((i.imm << 16) + sext16(j.imm)) & 0xFFFFFFFF
            else:
                continue
            sda.setdefault(i.d, v)
    return sda


class Func:
    __slots__ = ("start", "end", "labels", "tables", "calls")

    def __init__(self, start, end):
        self.start, self.end = start, end
        self.labels = set()
        self.tables = {}
        self.calls = set()


def discover(prog, extra_entries=(), verbose=True):
    seeds = set(extra_entries)
    seeds.add(prog.info["entry"])
    for m in prog.info["modules"]:
        for k in ("prolog", "epilog", "unresolved"):
            if m[k]:
                seeds.add(m[k])
    for s in prog.exec:
        seeds.add(s.addr)
    for a, i in prog.ins.items():
        if i.name == "b" and i.lk and prog.is_code(i.target) and i.target != a + 4:
            seeds.add(i.target)
    seeds = {a for a in seeds if prog.is_code(a)}

    def ranges(seedset):
        out = []
        for s in prog.exec:
            lo, hi = s.addr, s.addr + len(s.data)
            ss = sorted(a for a in seedset if lo <= a < hi)
            for k, a in enumerate(ss):
                out.append((a, ss[k + 1] if k + 1 < len(ss) else hi))
        return out

    # jump-table pass first so table words are not mistaken for function pointers
    table_words = set()
    table_targets = set()
    for (st, en) in ranges(seeds):
        for a in range(st, en, 4):
            i = prog.ins[a]
            if i.name == "bcctr" and not i.lk:
                jt = find_jump_table(prog, a, st, en)
                if jt:
                    tbl, t = jt
                    table_words.update(range(tbl, tbl + 4 * len(t), 4))
                    table_targets.update(t)
    ptr_seeds = 0
    for s in prog.data:
        d = s.data
        for o in range(0, len(d) - 3, 4):
            a = s.addr + o
            if a in table_words:
                continue
            v = struct.unpack_from(">I", d, o)[0]
            if v in prog.ins and v not in seeds and prog.plausible_start(v):
                seeds.add(v)
                ptr_seeds += 1

    # code directly after an unconditional blr is (almost) always a function start
    blr_seeds = 0
    for a, i in prog.ins.items():
        after_pad = ppc.ends_block(i) and (a + 4) in prog.ins and prog.ins[a + 4].w == 0
        if (i.name == "bclr" and not i.lk and ppc.is_unconditional(i)) or after_pad:
            t = a + 4
            while t in prog.ins and prog.ins[t].w == 0:
                t += 4
            if t in prog.ins and t not in seeds and t not in table_targets and prog.ins[t].name:
                seeds.add(t)
                blr_seeds += 1

    # function addresses materialised in code: lis rX,hi ; addi/ori rY,rX,lo
    const_seeds = 0
    hi = {}
    for a in sorted(prog.ins):
        i = prog.ins[a]
        n = i.name
        if n == "addis" and i.a == 0:
            hi[i.d] = (a, i.imm << 16)
            continue
        v = None
        if n == "addi" and i.a in hi and a - hi[i.a][0] <= 64:
            v = (hi[i.a][1] + sext16(i.imm)) & 0xFFFFFFFF
        elif n == "ori" and i.d in hi and a - hi[i.d][0] <= 64:
            v = hi[i.d][1] | i.imm
        if ppc.ends_block(i):
            hi = {}
        elif n == "b" and i.lk:
            hi = {r: v for r, v in hi.items() if r >= 14}   # callee-saved survive calls
        if v is not None and v in prog.ins and v not in seeds and prog.plausible_start(v):
            seeds.add(v)
            const_seeds += 1
        if n in ("addi", "ori") and v is None and i.d in hi:
            del hi[i.d]

    it = 0
    while True:
        it += 1
        added = set()
        for (st, en) in ranges(seeds):
            for a in range(st, en, 4):
                i = prog.ins[a]
                if i.name in ("b", "bc") and not i.lk:
                    t = i.target
                    if not (st <= t < en) and prog.is_code(t) and t not in seeds:
                        added.add(t)
        if not added:
            break
        seeds |= added
    funcs = {}
    for (st, en) in ranges(seeds):
        f = Func(st, en)
        for a in range(st, en, 4):
            i = prog.ins[a]
            if i.name in ("b", "bc") and not i.lk and st <= i.target < en:
                f.labels.add(i.target)
            elif i.name == "bcctr" and not i.lk:
                jt = find_jump_table(prog, a, st, en)
                if jt:
                    f.tables[a] = jt
                    f.labels.update(jt[1])
        funcs[st] = f
    if verbose:
        print("functions: %d (data-pointer seeds %d, code-constant seeds %d, after-blr seeds %d, split passes %d, jump tables %d)" % (
            len(funcs), ptr_seeds, const_seeds, blr_seeds, it, sum(len(f.tables) for f in funcs.values())))
    return funcs


# ---------------------------------------------------------------------------
# C emission
# ---------------------------------------------------------------------------
R = "ctx->r[%d]"
F0 = "ctx->f[%d].d"       # ps0 as double
FU = "ctx->f[%d].u"       # ps0 raw bits
P1 = "ctx->ps1[%d]"


def r(n):
    return R % n


def f0(n):
    return F0 % n


def p1(n):
    return P1 % n


def ra0(n):
    return "0" if n == 0 else r(n)


def h(v):
    return "0x%X" % (v & 0xFFFFFFFF)


def cr0(expr):
    return "PPC_CR0(ctx, (int32_t)(%s));" % expr


class Emitter:
    def __init__(self, prog, funcs, names):
        self.prog = prog
        self.funcs = funcs
        self.names = names
        self.unimpl = defaultdict(int)

    def fname(self, a):
        return "f_%08X" % a

    def call(self, target, ret):
        if target in self.funcs:
            return "ctx->lr = %s; %s(ctx, base);" % (h(ret), self.fname(target))
        return "ctx->lr = %s; ppc_call(ctx, base, %s);" % (h(ret), h(target))

    def tail(self, f, target, pc=None):
        if f.start <= target < f.end:
            if pc is not None and target <= pc:
                return "PPC_CHECK_IRQ(); goto L_%08X;" % target   # loop back-edge
            return "goto L_%08X;" % target
        if target in self.funcs:
            return "PPC_TAILCALL(%s(ctx, base));" % self.fname(target)
        return "{ ppc_call(ctx, base, %s); return; }" % h(target)

    @staticmethod
    def bo_cond(i, ctr_already=False):
        """Return (pre, cond) for a BO/BI pair."""
        bo, bi = i.bo, i.bi
        pre = ""
        parts = []
        if not bo & 4:
            pre = "ctx->ctr--; "
            parts.append("ctx->ctr == 0" if bo & 2 else "ctx->ctr != 0")
        if not bo & 0x10:
            parts.append(("ctx->crb[%d]" % bi) if bo & 8 else ("!ctx->crb[%d]" % bi))
        return pre, " && ".join(parts) if parts else "1"

    # -----------------------------------------------------------------
    def emit_function(self, f):
        out = []
        nm = self.names.get(f.start)
        out.append("\n/* %s%08X..%08X */" % ((nm + " ") if nm else "", f.start, f.end))
        out.append("PPC_FUNC_IMPL(%s) {" % self.fname(f.start))
        out.append("\tuint32_t ea; (void)ea;")
        out.append("L_%08X:;" % f.start)
        # drop zero-word alignment padding that follows the final branch
        stop = f.end
        while stop - 4 > f.start and self.prog.ins[stop - 4].w == 0 and (stop - 4) not in f.labels:
            stop -= 4
        if stop == f.end or not ppc.ends_block(self.prog.ins[stop - 4]):
            stop = f.end
        a = f.start
        last = None
        while a < stop:
            i = self.prog.ins[a]
            if a in f.labels and a != f.start:
                out.append("L_%08X:;" % a)
            code = self.emit(f, a, i)
            out.append("\t" + code if not code.startswith("\t") else code)
            last = i
            a += 4
        if last is None or not ppc.ends_block(last):
            # falls through into the next function
            out.append("\t" + self.tail(f, f.end) if f.end in self.funcs else "\tppc_fallthrough(ctx, %s);" % h(f.end))
        out.append("}")
        return "\n".join(out)

    def emit(self, f, pc, i):  # noqa: C901 - big instruction switch
        n = i.name
        d, a, b, c = i.d, i.a, i.b, i.c
        simm = sext16(i.imm)
        uimm = i.imm
        rc = i.rc
        if n is None:
            if i.w == 0:
                return "ppc_illegal(ctx, %s, 0);" % h(pc)
            self.unimpl[".word"] += 1
            return "ppc_illegal(ctx, %s, %s);" % (h(pc), h(i.w))

        # ---------------- integer arithmetic ----------------
        if n == "addi":
            if a == 0:
                return "%s = %s;" % (r(d), h(simm))
            return "%s = %s + %s;" % (r(d), r(a), h(simm))
        if n == "addis":
            if a == 0:
                return "%s = %s;" % (r(d), h(uimm << 16))
            return "%s = %s + %s;" % (r(d), r(a), h(uimm << 16))
        if n in ("addic", "addic."):
            s = "{ uint64_t t = (uint64_t)%s + %s; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (
                r(a), h(simm) + "u", r(d))
            return s + (" " + cr0(r(d)) if n == "addic." else "")
        if n == "subfic":
            return "{ uint64_t t = (uint64_t)(uint32_t)~%s + %su + 1; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (
                r(a), h(simm), r(d))
        if n == "mulli":
            return "%s = (uint32_t)((int32_t)%s * %d);" % (r(d), r(a), simm)

        rec = (" " + cr0(r(d))) if rc else ""
        if n in ("add", "subf", "neg", "mullw", "mulhw", "mulhwu", "divw", "divwu",
                 "addc", "adde", "addze", "addme", "subfc", "subfe", "subfze", "subfme"):
            e = {
                "add": "%s = %s + %s;" % (r(d), r(a), r(b)),
                "subf": "%s = %s - %s;" % (r(d), r(b), r(a)),
                "neg": "%s = (uint32_t)-(int32_t)%s;" % (r(d), r(a)),
                "mullw": "%s = (uint32_t)((int32_t)%s * (int32_t)%s);" % (r(d), r(a), r(b)),
                "mulhw": "%s = (uint32_t)(((int64_t)(int32_t)%s * (int64_t)(int32_t)%s) >> 32);" % (r(d), r(a), r(b)),
                "mulhwu": "%s = (uint32_t)(((uint64_t)%s * (uint64_t)%s) >> 32);" % (r(d), r(a), r(b)),
                "divw": "%s = ppc_divw(%s, %s);" % (r(d), r(a), r(b)),
                "divwu": "%s = ppc_divwu(%s, %s);" % (r(d), r(a), r(b)),
                "addc": "{ uint64_t t = (uint64_t)%s + %s; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(b), r(d)),
                "adde": "{ uint64_t t = (uint64_t)%s + %s + ctx->xer_ca; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(b), r(d)),
                "addze": "{ uint64_t t = (uint64_t)%s + ctx->xer_ca; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(d)),
                "addme": "{ uint64_t t = (uint64_t)%s + ctx->xer_ca + 0xFFFFFFFFu; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(d)),
                "subfc": "{ uint64_t t = (uint64_t)(uint32_t)~%s + %s + 1; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(b), r(d)),
                "subfe": "{ uint64_t t = (uint64_t)(uint32_t)~%s + %s + ctx->xer_ca; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(b), r(d)),
                "subfze": "{ uint64_t t = (uint64_t)(uint32_t)~%s + ctx->xer_ca; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(d)),
                "subfme": "{ uint64_t t = (uint64_t)(uint32_t)~%s + ctx->xer_ca + 0xFFFFFFFFu; ctx->xer_ca = (uint32_t)(t >> 32); %s = (uint32_t)t; }" % (r(a), r(d)),
            }[n]
            if i.oe:
                # XER[OV]/[SO] tracking is not modelled (MWCC never relies on it)
                self.unimpl["oe:" + n] += 1
            return e + rec

        # ---------------- logical / shifts ----------------
        if n in ("ori", "oris", "xori", "xoris"):
            if n == "ori" and a == d == 0 and uimm == 0:
                return "/* nop */;"
            op = "|" if n[0] == "o" else "^"
            v = uimm << 16 if n.endswith("s") else uimm
            return "%s = %s %s %s;" % (r(a), r(d), op, h(v))
        if n in ("andi.", "andis."):
            v = uimm << 16 if n == "andis." else uimm
            return "%s = %s & %s; %s" % (r(a), r(d), h(v), cr0(r(a)))
        recA = (" " + cr0(r(a))) if rc else ""
        if n in ("and", "or", "xor", "nand", "nor", "eqv", "andc", "orc"):
            if n == "or" and d == b:
                return "%s = %s;%s" % (r(a), r(d), recA)
            e = {"and": "%s & %s", "or": "%s | %s", "xor": "%s ^ %s", "nand": "~(%s & %s)",
                 "nor": "~(%s | %s)", "eqv": "~(%s ^ %s)", "andc": "%s & ~%s", "orc": "%s | ~%s"}[n]
            return "%s = %s;%s" % (r(a), e % (r(d), r(b)), recA)
        if n == "extsb":
            return "%s = (uint32_t)(int32_t)(int8_t)%s;%s" % (r(a), r(d), recA)
        if n == "extsh":
            return "%s = (uint32_t)(int32_t)(int16_t)%s;%s" % (r(a), r(d), recA)
        if n == "cntlzw":
            return "%s = ppc_cntlzw(%s);%s" % (r(a), r(d), recA)
        if n == "slw":
            return "{ uint32_t s = %s & 0x3F; %s = s > 31 ? 0 : %s << s; }%s" % (r(b), r(a), r(d), recA)
        if n == "srw":
            return "{ uint32_t s = %s & 0x3F; %s = s > 31 ? 0 : %s >> s; }%s" % (r(b), r(a), r(d), recA)
        if n == "sraw":
            return "%s = ppc_sraw(ctx, %s, %s);%s" % (r(a), r(d), r(b), recA)
        if n == "srawi":
            sh = b
            if sh == 0:
                return "%s = %s; ctx->xer_ca = 0;%s" % (r(a), r(d), recA)
            return "{ int32_t v = (int32_t)%s; ctx->xer_ca = (v < 0) && (v & %s); %s = (uint32_t)(v >> %d); }%s" % (
                r(d), h((1 << sh) - 1), r(a), sh, recA)
        if n == "rlwinm":
            m = mask32(i.mb, i.me)
            sh = i.sh
            if sh == 0:
                e = "%s & %s" % (r(d), h(m))
            elif m == (0xFFFFFFFF << sh) & 0xFFFFFFFF:
                e = "%s << %d" % (r(d), sh)
            elif m == 0xFFFFFFFF >> (32 - sh):
                e = "%s >> %d" % (r(d), 32 - sh)
            else:
                e = "ppc_rotl(%s, %d) & %s" % (r(d), sh, h(m))
            return "%s = %s;%s" % (r(a), e, recA)
        if n == "rlwimi":
            m = mask32(i.mb, i.me)
            return "%s = (ppc_rotl(%s, %d) & %s) | (%s & %s);%s" % (r(a), r(d), i.sh, h(m), r(a), h(~m & 0xFFFFFFFF), recA)
        if n == "rlwnm":
            m = mask32(i.mb, i.me)
            return "%s = ppc_rotl(%s, %s & 31) & %s;%s" % (r(a), r(d), r(b), h(m), recA)

        # ---------------- compare ----------------
        if n in ("cmpi", "cmpli", "cmp", "cmpl"):
            crf = d >> 2
            if n == "cmpi":
                return "PPC_CMPS(ctx, %d, (int32_t)%s, %d);" % (crf, r(a), simm)
            if n == "cmpli":
                return "PPC_CMPU(ctx, %d, %s, %su);" % (crf, r(a), h(uimm))
            if n == "cmp":
                return "PPC_CMPS(ctx, %d, (int32_t)%s, (int32_t)%s);" % (crf, r(a), r(b))
            return "PPC_CMPU(ctx, %d, %s, %s);" % (crf, r(a), r(b))

        # ---------------- loads / stores ----------------
        mem_d = {"lwz": ("ld32", 0), "lbz": ("ld8", 0), "lhz": ("ld16", 0), "lha": ("ld16s", 0),
                 "stw": ("st32", 1), "stb": ("st8", 1), "sth": ("st16", 1)}
        base_n = n.rstrip("ux") if n not in ("lhbrx", "lwbrx", "sthbrx", "stwbrx", "stfiwx") else n
        if base_n in mem_d and (n == base_n or n[len(base_n):] in ("u", "x", "ux")):
            fn, st = mem_d[base_n]
            suf = n[len(base_n):]
            upd = "u" in suf
            if "x" in suf:
                ea = "%s + %s" % (r(a) if upd else ra0(a), r(b))
            else:
                ea = "%s + %s" % (r(a) if upd else ra0(a), h(simm))
            s = "ea = %s; " % ea
            if st:
                s += "%s(base, ea, %s);" % (fn, r(d))
            else:
                s += "%s = %s(base, ea);" % (r(d), fn)
            if upd:
                s += " %s = ea;" % r(a)
            return s
        if n in ("lhbrx", "lwbrx", "sthbrx", "stwbrx"):
            ea = "ea = %s + %s; " % (ra0(a), r(b))
            return ea + {"lhbrx": "%s = ld16le(base, ea);" % r(d), "lwbrx": "%s = ld32le(base, ea);" % r(d),
                         "sthbrx": "st16le(base, ea, %s);" % r(d), "stwbrx": "st32le(base, ea, %s);" % r(d)}[n]
        if n == "lmw":
            return "ea = %s + %s; for (int k = %d; k < 32; k++, ea += 4) ctx->r[k] = ld32(base, ea);" % (ra0(a), h(simm), d)
        if n == "stmw":
            return "ea = %s + %s; for (int k = %d; k < 32; k++, ea += 4) st32(base, ea, ctx->r[k]);" % (ra0(a), h(simm), d)
        if n in ("lswi", "stswi", "lswx", "stswx"):
            self.unimpl[n] += 1
            return "ppc_%s(ctx, base, %d, %d, %d);" % (n, d, a, b)
        if n in ("lwarx", "stwcx."):
            if n == "lwarx":
                return "ea = %s + %s; %s = ld32(base, ea); ctx->reserve = 1;" % (ra0(a), r(b), r(d))
            return "ea = %s + %s; st32(base, ea, %s); ctx->crb[0] = ctx->crb[1] = 0; ctx->crb[2] = 1; ctx->crb[3] = ctx->xer_so;" % (ra0(a), r(b), r(d))

        fmem = {"lfs": 0, "lfd": 0, "stfs": 1, "stfd": 1}
        fb = n.rstrip("ux")
        if fb in fmem and n[len(fb):] in ("", "u", "x", "ux"):
            suf = n[len(fb):]
            upd = "u" in suf
            if "x" in suf:
                ea = "%s + %s" % (r(a) if upd else ra0(a), r(b))
            else:
                ea = "%s + %s" % (r(a) if upd else ra0(a), h(simm))
            s = "ea = %s; " % ea
            s += {"lfs": "%s = %s = (double)ld32f(base, ea);" % (f0(d), p1(d)),
                  "lfd": "%s = ld64(base, ea);" % (FU % d),
                  "stfs": "st32f(base, ea, (float)%s);" % f0(d),
                  "stfd": "st64(base, ea, %s);" % (FU % d)}[fb]
            if upd:
                s += " %s = ea;" % r(a)
            return s
        if n == "stfiwx":
            return "ea = %s + %s; st32(base, ea, (uint32_t)%s);" % (ra0(a), r(b), FU % d)

        # ---------------- paired-single load/store ----------------
        if n in ("psq_l", "psq_lu", "psq_st", "psq_stu", "psq_lx", "psq_lux", "psq_stx", "psq_stux"):
            upd = "u" in n[4:]
            if n.endswith("x"):
                ea = "%s + %s" % (r(a) if upd else ra0(a), r(b))
            else:
                ea = "%s + %s" % (r(a) if upd else ra0(a), h(i.imm))
            s = "ea = %s; " % ea
            if "_l" in n:
                s += "ppc_psq_load(ctx, base, ea, %d, %d, %d);" % (d, i.w_, i.i_)
            else:
                s += "ppc_psq_store(ctx, base, ea, %d, %d, %d);" % (d, i.w_, i.i_)
            if upd:
                s += " %s = ea;" % r(a)
            return s

        # ---------------- floating point ----------------
        crf1 = " PPC_CR1(ctx);" if rc else ""
        if n in ("fadd", "fsub", "fmul", "fdiv", "fadds", "fsubs", "fmuls", "fdivs"):
            op = {"add": "+", "sub": "-", "mul": "*", "div": "/"}[n[1:4]]
            rhs = c if n.startswith("fmul") else b
            e = "%s %s %s" % (f0(a), op, f0(rhs))
            if n.endswith("s"):
                return "{ double t = (double)(float)(%s); %s = %s = t; }%s" % (e, f0(d), p1(d), crf1)
            return "%s = %s;%s" % (f0(d), e, crf1)
        if n in ("fmadd", "fmsub", "fnmadd", "fnmsub", "fmadds", "fmsubs", "fnmadds", "fnmsubs"):
            neg = n.startswith("fn")
            sub = "sub" in n
            e = "fma(%s, %s, %s%s)" % (f0(a), f0(c), "-" if sub else "", f0(b))
            if neg:
                e = "-" + e
            if n.endswith("s"):
                return "{ double t = (double)(float)(%s); %s = %s = t; }%s" % (e, f0(d), p1(d), crf1)
            return "%s = %s;%s" % (f0(d), e, crf1)
        if n == "fsel":
            return "%s = (%s >= 0.0) ? %s : %s;%s" % (f0(d), f0(a), f0(c), f0(b), crf1)
        if n in ("fres", "frsqrte", "fsqrt", "fsqrts"):
            e = {"fres": "1.0 / %s", "frsqrte": "1.0 / sqrt(%s)", "fsqrt": "sqrt(%s)", "fsqrts": "sqrt(%s)"}[n] % f0(b)
            if n in ("fres", "fsqrts"):
                return "{ double t = (double)(float)(%s); %s = %s = t; }%s" % (e, f0(d), p1(d), crf1)
            return "%s = %s;%s" % (f0(d), e, crf1)
        if n == "frsp":
            return "{ double t = (double)(float)%s; %s = %s = t; }%s" % (f0(b), f0(d), p1(d), crf1)
        if n == "fmr":
            return "%s = %s;%s" % (FU % d, FU % b, crf1)
        if n == "fneg":
            return "%s = %s ^ 0x8000000000000000ull;%s" % (FU % d, FU % b, crf1)
        if n == "fabs":
            return "%s = %s & 0x7FFFFFFFFFFFFFFFull;%s" % (FU % d, FU % b, crf1)
        if n == "fnabs":
            return "%s = %s | 0x8000000000000000ull;%s" % (FU % d, FU % b, crf1)
        if n in ("fctiw", "fctiwz"):
            return "%s = ppc_%s(ctx, %s);%s" % (FU % d, n, f0(b), crf1)
        if n in ("fcmpu", "fcmpo"):
            return "PPC_FCMP(ctx, %d, %s, %s);" % (d >> 2, f0(a), f0(b))
        if n == "mffs":
            return "%s = 0xFFF8000000000000ull | ctx->fpscr;%s" % (FU % d, crf1)
        if n == "mtfsf":
            fm = (i.w >> 17) & 0xFF
            return "ppc_mtfsf(ctx, %s, (uint32_t)%s);%s" % (h(fm), FU % b, crf1)
        if n == "mtfsb0":
            return "ctx->fpscr &= ~%su;%s" % (h(0x80000000 >> d), crf1)
        if n == "mtfsb1":
            return "ctx->fpscr |= %su;%s" % (h(0x80000000 >> d), crf1)
        if n == "mtfsfi":
            imm = (i.w >> 12) & 0xF
            sh = 28 - 4 * (d >> 2)
            return "ctx->fpscr = (ctx->fpscr & ~%su) | %su; ppc_fpscr_changed(ctx);%s" % (h(0xF << sh), h(imm << sh), crf1)
        if n == "mcrfs":
            return "ppc_mcrfs(ctx, %d, %d);" % (d >> 2, a >> 2)

        # ---------------- paired-single arithmetic ----------------
        if n.startswith("ps_"):
            return self.emit_ps(n, d, a, b, c, rc)

        # ---------------- branches ----------------
        if n == "b":
            if i.lk:
                if i.target == pc + 4:
                    return "ctx->lr = %s;" % h(pc + 4)
                return self.call(i.target, pc + 4)
            return self.tail(f, i.target, pc)
        if n == "bc":
            pre, cond = self.bo_cond(i)
            if i.lk:
                return "%sif (%s) { %s }" % (pre, cond, self.call(i.target, pc + 4))
            return "%sif (%s) { %s }" % (pre, cond, self.tail(f, i.target, pc))
        if n == "bclr":
            pre, cond = self.bo_cond(i)
            if i.lk:
                body = "{ uint32_t t = ctx->lr; ctx->lr = %s; ppc_call(ctx, base, t); }" % h(pc + 4)
            else:
                body = "return;"
            if cond == "1":
                return pre + body
            return "%sif (%s) %s" % (pre, cond, body)
        if n == "bcctr":
            pre, cond = self.bo_cond(i)
            if i.lk:
                body = "{ ctx->lr = %s; ppc_call(ctx, base, ctx->ctr); }" % h(pc + 4)
            elif pc in f.tables:
                tbl, targets = f.tables[pc]
                cases = " ".join("case %s: goto L_%08X;" % (h(t), t) for t in sorted(set(targets)))
                body = "{ switch (ctx->ctr) { %s default: ppc_call(ctx, base, ctx->ctr); return; } }" % cases
            else:
                body = "{ ppc_call(ctx, base, ctx->ctr); return; }"
            if cond == "1":
                return pre + body
            return "%sif (%s) %s" % (pre, cond, body)

        # ---------------- condition register ----------------
        if n in ("crand", "cror", "crxor", "crnand", "crnor", "creqv", "crandc", "crorc"):
            e = {"crand": "%s & %s", "cror": "%s | %s", "crxor": "%s ^ %s", "crnand": "!(%s & %s)",
                 "crnor": "!(%s | %s)", "creqv": "!(%s ^ %s)", "crandc": "%s & !%s", "crorc": "%s | !%s"}[n]
            return "ctx->crb[%d] = %s;" % (d, e % ("ctx->crb[%d]" % a, "ctx->crb[%d]" % b))
        if n == "mcrf":
            dd, ss = d >> 2, a >> 2
            return "memcpy(&ctx->crb[%d], &ctx->crb[%d], 4);" % (dd * 4, ss * 4)
        if n == "mfcr":
            return "%s = ppc_get_cr(ctx);" % r(d)
        if n == "mtcrf":
            return "ppc_set_cr(ctx, %s, %s);" % (r(d), h(i.crm))
        if n == "mcrxr":
            f_ = d >> 2
            return "ctx->crb[%d] = ctx->xer_so; ctx->crb[%d] = ctx->xer_ov; ctx->crb[%d] = ctx->xer_ca; ctx->crb[%d] = 0; ctx->xer_so = ctx->xer_ov = ctx->xer_ca = 0;" % (
                f_ * 4, f_ * 4 + 1, f_ * 4 + 2, f_ * 4 + 3)

        # ---------------- special registers / system ----------------
        if n == "mfspr":
            spr = i.spr
            if spr == 8:
                return "%s = ctx->lr;" % r(d)
            if spr == 9:
                return "%s = ctx->ctr;" % r(d)
            if spr == 1:
                return "%s = ppc_get_xer(ctx);" % r(d)
            if 912 <= spr <= 919:
                return "%s = ctx->gqr[%d];" % (r(d), spr - 912)
            return "%s = ppc_mfspr(ctx, %d);" % (r(d), spr)
        if n == "mtspr":
            spr = i.spr
            if spr == 8:
                return "ctx->lr = %s;" % r(d)
            if spr == 9:
                return "ctx->ctr = %s;" % r(d)
            if spr == 1:
                return "ppc_set_xer(ctx, %s);" % r(d)
            if 912 <= spr <= 919:
                return "ctx->gqr[%d] = %s;" % (spr - 912, r(d))
            return "ppc_mtspr(ctx, base, %d, %s);" % (spr, r(d))
        if n == "mftb":
            return "%s = ppc_mftb(ctx, %d);" % (r(d), i.spr)
        if n == "mfmsr":
            return "%s = ctx->msr;" % r(d)
        if n == "mtmsr":
            return "ppc_mtmsr(ctx, %s);" % r(d)
        if n == "mfsr":
            return "%s = ctx->sr[%d];" % (r(d), a & 15)
        if n == "mtsr":
            return "ctx->sr[%d] = %s;" % (a & 15, r(d))
        if n in ("mfsrin", "mtsrin"):
            if n == "mfsrin":
                return "%s = ctx->sr[%s >> 28];" % (r(d), r(b))
            return "ctx->sr[%s >> 28] = %s;" % (r(b), r(d))
        if n in ("sync", "isync", "eieio", "dcbf", "dcbst", "dcbt", "dcbtst", "icbi", "tlbie",
                 "tlbsync", "tlbia", "dcbi"):
            if n == "dcbi":
                return "/* dcbi */;"
            return "/* %s */;" % n
        if n == "dcbz":
            return "ea = (%s + %s) & ~31u; ppc_dcbz(base, ea);" % (ra0(a), r(b))
        if n == "dcbz_l":
            return "ea = (%s + %s) & ~31u; ppc_dcbz(base, ea);" % (ra0(a), r(b))
        if n == "sc":
            return "ppc_syscall(ctx, base, %s);" % h(pc)
        if n == "rfi":
            return "ppc_rfi(ctx, base); return;"
        if n == "tw":
            return "ppc_tw(ctx, %s, %d, %s, %s);" % (h(pc), d, r(a), r(b))
        if n == "twi":
            return "ppc_tw(ctx, %s, %d, %s, %s);" % (h(pc), d, r(a), h(simm))
        if n in ("eciwx", "ecowx"):
            self.unimpl[n] += 1
            return "ppc_illegal(ctx, %s, %s);" % (h(pc), h(i.w))
        self.unimpl[n] += 1
        return "ppc_illegal(ctx, %s, %s); /* %s */" % (h(pc), h(i.w), n)

    def emit_ps(self, n, d, a, b, c, rc):
        A0, A1, B0, B1, C0, C1 = f0(a), p1(a), f0(b), p1(b), f0(c), p1(c)
        crf1 = " PPC_CR1(ctx);" if rc else ""

        def both(e0, e1, rnd=True):
            if rnd:
                return "{ double t0 = (double)(float)(%s), t1 = (double)(float)(%s); %s = t0; %s = t1; }%s" % (
                    e0, e1, f0(d), p1(d), crf1)
            return "{ double t0 = %s, t1 = %s; %s = t0; %s = t1; }%s" % (e0, e1, f0(d), p1(d), crf1)

        if n in ("ps_add", "ps_sub", "ps_div"):
            op = {"ps_add": "+", "ps_sub": "-", "ps_div": "/"}[n]
            return both("%s %s %s" % (A0, op, B0), "%s %s %s" % (A1, op, B1))
        if n == "ps_mul":
            return both("%s * %s" % (A0, C0), "%s * %s" % (A1, C1))
        if n in ("ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub"):
            s = "-" if "sub" in n else ""
            ng = "-" if n.startswith("ps_n") else ""
            return both("%sfma(%s, %s, %s%s)" % (ng, A0, C0, s, B0), "%sfma(%s, %s, %s%s)" % (ng, A1, C1, s, B1))
        if n == "ps_muls0":
            return both("%s * %s" % (A0, C0), "%s * %s" % (A1, C0))
        if n == "ps_muls1":
            return both("%s * %s" % (A0, C1), "%s * %s" % (A1, C1))
        if n == "ps_madds0":
            return both("fma(%s, %s, %s)" % (A0, C0, B0), "fma(%s, %s, %s)" % (A1, C0, B1))
        if n == "ps_madds1":
            return both("fma(%s, %s, %s)" % (A0, C1, B0), "fma(%s, %s, %s)" % (A1, C1, B1))
        if n == "ps_sum0":
            return both("%s + %s" % (A0, B1), C1)
        if n == "ps_sum1":
            return both(C0, "%s + %s" % (A0, B1))
        if n == "ps_sel":
            return both("(%s >= 0.0) ? %s : %s" % (A0, C0, B0), "(%s >= 0.0) ? %s : %s" % (A1, C1, B1), False)
        if n == "ps_res":
            return both("1.0 / %s" % B0, "1.0 / %s" % B1)
        if n == "ps_rsqrte":
            return both("1.0 / sqrt(%s)" % B0, "1.0 / sqrt(%s)" % B1)
        if n == "ps_merge00":
            return both(A0, B0, False)
        if n == "ps_merge01":
            return both(A0, B1, False)
        if n == "ps_merge10":
            return both(A1, B0, False)
        if n == "ps_merge11":
            return both(A1, B1, False)
        if n == "ps_mr":
            return both(B0, B1, False)
        if n == "ps_neg":
            return both("-%s" % B0, "-%s" % B1, False)
        if n == "ps_abs":
            return both("fabs(%s)" % B0, "fabs(%s)" % B1, False)
        if n == "ps_nabs":
            return both("-fabs(%s)" % B0, "-fabs(%s)" % B1, False)
        if n in ("ps_cmpu0", "ps_cmpo0"):
            return "PPC_FCMP(ctx, %d, %s, %s);" % (d >> 2, A0, B0)
        if n in ("ps_cmpu1", "ps_cmpo1"):
            return "PPC_FCMP(ctx, %d, %s, %s);" % (d >> 2, A1, B1)
        self.unimpl[n] += 1
        return "ppc_illegal(ctx, 0, 0); /* %s */" % n


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build", help="directory with image.bin/image.json")
    ap.add_argument("-o", "--out", default=None, help="output dir (default <build>/gen)")
    ap.add_argument("-c", "--config", default=None, help="config/functions.json")
    ap.add_argument("--per-file", type=int, default=1500, help="functions per .c file")
    args = ap.parse_args()
    out = args.out or os.path.join(args.build, "gen")
    os.makedirs(out, exist_ok=True)

    segs = image.read_image(os.path.join(args.build, "image.bin"))
    info = json.load(open(os.path.join(args.build, "image.json")))
    names, extra = {}, []
    if args.config and os.path.exists(args.config):
        cfg = json.load(open(args.config))
        for k, v in cfg.get("names", {}).items():
            names[int(k, 16)] = v
        extra = [int(x, 16) for x in cfg.get("entries", [])]
        extra += list(names)

    prog = Program(segs, info)
    funcs = discover(prog, extra)
    em = Emitter(prog, funcs, names)

    starts = sorted(funcs)
    with open(os.path.join(out, "ppc_funcs.h"), "w") as fh:
        fh.write("/* generated by tools/recomp.py - do not edit */\n#pragma once\n#include \"ppc_context.h\"\n\n")
        for a in starts:
            fh.write("PPC_FUNC_DECL(f_%08X);\n" % a)
    nfiles = 0
    for k in range(0, len(starts), args.per_file):
        chunk = starts[k:k + args.per_file]
        with open(os.path.join(out, "ppc_func_%03d.c" % nfiles), "w") as fh:
            fh.write("/* generated by tools/recomp.py - do not edit */\n#include \"ppc_funcs.h\"\n")
            for a in chunk:
                fh.write(em.emit_function(funcs[a]))
                fh.write("\n")
        nfiles += 1
    with open(os.path.join(out, "ppc_func_table.c"), "w") as fh:
        fh.write("/* generated by tools/recomp.py - do not edit */\n#include \"ppc_funcs.h\"\n\n")
        fh.write("const PPCFuncEntry ppc_func_table[] = {\n")
        for a in starts:
            fh.write("\t{ 0x%08X, f_%08X },\n" % (a, a))
        fh.write("};\nconst uint32_t ppc_func_count = %d;\n" % len(starts))
    with open(os.path.join(out, "functions.json"), "w") as fh:
        json.dump({"functions": [{"addr": "%08X" % a, "size": funcs[a].end - a,
                                  "name": names.get(a, "")} for a in starts]}, fh)
    with open(os.path.join(out, "sources.cmake"), "w") as fh:
        fh.write("set(PPC_GEN_SOURCES\n")
        for k in range(nfiles):
            fh.write("  ${GEN_DIR}/ppc_func_%03d.c\n" % k)
        fh.write("  ${GEN_DIR}/ppc_func_table.c\n)\n")
    print("wrote %d functions in %d files to %s" % (len(starts), nfiles, out))
    if em.unimpl:
        print("instructions routed to runtime helpers / unimplemented:", dict(em.unimpl))


if __name__ == "__main__":
    main()
