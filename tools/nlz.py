#!/usr/bin/env python3
"""nlz.py - Nintendo LZ77 (type 0x10) / LZ11 (type 0x11) decompressor.

NSMBW ships its REL modules as files/rels/*.rel.LZ.
Usage: python3 tools/nlz.py in.rel.LZ out.rel
"""
import struct
import sys


def decompress(src):
    typ = src[0]
    size = src[1] | (src[2] << 8) | (src[3] << 16)
    p = 4
    if size == 0:
        size = struct.unpack_from("<I", src, 4)[0]
        p = 8
    out = bytearray()
    if typ not in (0x10, 0x11):
        raise ValueError("not Nintendo LZ (type byte 0x%02x)" % typ)
    while len(out) < size:
        flags = src[p]
        p += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if not (flags >> bit) & 1:
                out.append(src[p])
                p += 1
                continue
            if typ == 0x10:
                b0, b1 = src[p], src[p + 1]
                p += 2
                length = (b0 >> 4) + 3
                disp = ((b0 & 0xF) << 8 | b1) + 1
            else:
                b0 = src[p]
                ind = b0 >> 4
                if ind == 0:
                    b1, b2 = src[p + 1], src[p + 2]
                    p += 3
                    length = ((b0 & 0xF) << 4 | (b1 >> 4)) + 0x11
                    disp = ((b1 & 0xF) << 8 | b2) + 1
                elif ind == 1:
                    b1, b2, b3 = src[p + 1], src[p + 2], src[p + 3]
                    p += 4
                    length = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111
                    disp = ((b2 & 0xF) << 8 | b3) + 1
                else:
                    b1 = src[p + 1]
                    p += 2
                    length = ind + 1
                    disp = ((b0 & 0xF) << 8 | b1) + 1
            start = len(out) - disp
            for i in range(length):
                out.append(out[start + i])
    return bytes(out[:size])


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    open(sys.argv[2], "wb").write(decompress(data))
