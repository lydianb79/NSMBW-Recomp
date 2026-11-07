"""
ppc.py - Gekko/Broadway (PowerPC 750CL) instruction decoder + disassembler.

Shared by recomp.py (C code generation) and analyze tooling. Decoding is
table-free: `decode(word)` returns an Ins with the mnemonic and fields.
"""


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def mask32(mb, me):
    """PowerPC MASK(mb, me) with big-endian bit numbering."""
    if mb <= me:
        m = ((0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    else:
        m = ((0xFFFFFFFF >> mb) | (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    return m


class Ins:
    __slots__ = ("w", "op", "name", "d", "a", "b", "c", "imm", "rc", "oe", "xo",
                 "bo", "bi", "target", "lk", "aa", "spr", "sh", "mb", "me", "crm", "w_", "i_")

    def __init__(self, w):
        self.w = w
        self.op = w >> 26
        self.d = (w >> 21) & 31
        self.a = (w >> 16) & 31
        self.b = (w >> 11) & 31
        self.c = (w >> 6) & 31
        self.imm = w & 0xFFFF
        self.rc = w & 1
        self.oe = (w >> 10) & 1
        self.xo = (w >> 1) & 0x3FF
        self.name = None
        self.target = None
        self.lk = w & 1
        self.aa = (w >> 1) & 1
        self.spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
        self.sh = self.b
        self.mb = (w >> 6) & 31
        self.me = (w >> 1) & 31
        self.bo = self.d
        self.bi = self.a
        self.crm = (w >> 12) & 0xFF
        self.w_ = 0
        self.i_ = 0


# primary opcodes with D-form semantics
_D = {
    7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 24: "ori", 25: "oris", 26: "xori", 27: "xoris",
    28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu",
    38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau",
    44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu",
    54: "stfd", 55: "stfdu",
}

_X31 = {
    0: "cmp", 4: "tw", 8: "subfc", 10: "addc", 11: "mulhwu", 19: "mfcr", 20: "lwarx",
    23: "lwzx", 24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl", 40: "subf", 54: "dcbst",
    55: "lwzux", 60: "andc", 75: "mulhw", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 104: "neg",
    119: "lbzux", 124: "nor", 136: "subfe", 138: "adde", 144: "mtcrf", 146: "mtmsr",
    150: "stwcx.", 151: "stwx", 183: "stwux", 200: "subfze", 202: "addze", 210: "mtsr",
    215: "stbx", 232: "subfme", 234: "addme", 235: "mullw", 242: "mtsrin", 246: "dcbtst",
    247: "stbux", 266: "add", 278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie",
    310: "eciwx", 311: "lhzux", 316: "xor", 339: "mfspr", 343: "lhax", 370: "tlbia",
    371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc", 438: "ecowx", 439: "sthux",
    444: "or", 459: "divwu", 467: "mtspr", 470: "dcbi", 476: "nand", 491: "divw",
    512: "mcrxr", 533: "lswx", 534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync",
    567: "lfsux", 595: "mfsr", 597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux",
    659: "mfsrin", 661: "stswx", 662: "stwbrx", 663: "stfsx", 695: "stfsux", 725: "stswi",
    727: "stfdx", 759: "stfdux", 790: "lhbrx", 792: "sraw", 824: "srawi", 854: "eieio",
    918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi", 983: "stfiwx", 1014: "dcbz",
}
# XO-form (9-bit XO with OE) arithmetic, mapped from the 10-bit value with OE=1
_OE_FORMS = {8, 10, 40, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491}

_X19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync",
        193: "crxor", 225: "crnand", 257: "crand", 289: "creqv", 417: "crorc",
        449: "cror", 528: "bcctr"}

_X63 = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1",
        40: "fneg", 64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs",
        264: "fabs", 583: "mffs", 711: "mtfsf"}
_A63 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul",
        26: "frsqrte", 28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
_A59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls",
        28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}

_PS_A = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
         15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel",
         24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd",
         30: "ps_nmsub", 31: "ps_nmadd"}
_PS_X = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr",
         96: "ps_cmpo1", 136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00",
         560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l"}
_PS_Q = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}


def decode(w, pc=0):
    i = Ins(w)
    op = i.op
    if op in _D:
        i.name = _D[op]
    elif op == 3:
        i.name = "twi"
    elif op == 4:
        x10 = i.xo
        x5 = (w >> 1) & 0x1F
        x6 = (w >> 1) & 0x3F
        if x6 in _PS_Q:
            i.name = _PS_Q[x6]
            i.w_ = (w >> 10) & 1
            i.i_ = (w >> 7) & 7
        elif x10 in _PS_X:
            i.name = _PS_X[x10]
        elif x5 in _PS_A:
            i.name = _PS_A[x5]
    elif op in (56, 57, 60, 61):
        i.name = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}[op]
        i.w_ = (w >> 15) & 1
        i.i_ = (w >> 12) & 7
        i.imm = sext(w & 0xFFF, 12)
    elif op == 16:
        i.name = "bc"
        bd = sext(w & 0xFFFC, 16)
        i.target = (bd if i.aa else pc + bd) & 0xFFFFFFFF
    elif op == 17:
        i.name = "sc"
    elif op == 18:
        i.name = "b"
        li = sext(w & 0x03FFFFFC, 26)
        i.target = (li if i.aa else pc + li) & 0xFFFFFFFF
    elif op == 19:
        i.name = _X19.get(i.xo)
    elif op == 20:
        i.name = "rlwimi"
    elif op == 21:
        i.name = "rlwinm"
    elif op == 23:
        i.name = "rlwnm"
    elif op == 31:
        xo = i.xo
        if xo in _X31:
            i.name = _X31[xo]
            i.oe = 0
        elif (xo & 0x1FF) in _OE_FORMS and xo & 0x200:
            i.name = _X31[xo & 0x1FF]
            i.oe = 1
    elif op == 59:
        i.name = _A59.get((w >> 1) & 0x1F)
    elif op == 63:
        x5 = (w >> 1) & 0x1F
        if x5 >= 16 and x5 in _A63:
            i.name = _A63[x5]
        else:
            i.name = _X63.get(i.xo)
    return i


def is_branch(i):
    return i.name in ("b", "bc", "bclr", "bcctr")


def is_unconditional(i):
    return (i.bo & 0x14) == 0x14


def ends_block(i):
    """Instruction after which execution never falls through."""
    n = i.name
    if n == "b" and not i.lk:
        return True
    if n in ("bclr", "bcctr") and not i.lk and is_unconditional(i):
        return True
    if n == "rfi":
        return True
    return False


def disasm(i, pc=0):
    n = i.name or ".word"
    if n == ".word":
        return ".word 0x%08x" % i.w
    if n in ("b", "bc"):
        return "%s%s%s 0x%08x" % (n, "l" if i.lk else "", "a" if i.aa else "", i.target)
    if n in ("bclr", "bcctr"):
        return "%s%s bo=%d bi=%d" % (n, "l" if i.lk else "", i.bo, i.bi)
    if i.op in _D:
        return "%s r%d, r%d, %d" % (n, i.d, i.a, sext16(i.imm))
    return "%s %d,%d,%d" % (n, i.d, i.a, i.b)
