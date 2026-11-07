#!/usr/bin/env python3
"""
rvz.py - Dolphin RVZ / WIA disc image reader and Wii partition extractor.

Reads the user's own RVZ dump, rebuilds the decrypted Wii game partition and
extracts its file system (sys/main.dol, files/rels/*.rel, assets ...).

RVZ stores Wii partition data already decrypted with the hash blocks removed,
so no Wii keys are needed: every 0x8000-byte disc sector contributes 0x7C00
bytes of user data.

Usage:
    python3 tools/rvz.py info    "New Super Mario Bros Wii (Europe).rvz"
    python3 tools/rvz.py extract "New Super Mario Bros Wii (Europe).rvz" game/
    python3 tools/rvz.py extract  image.rvz game/ --code-only   # main.dol + rels only

Compression back-ends: zstd (python 'zstandard' module or system libzstd via
ctypes - on macOS `brew install zstd`), lzma / lzma2 / bzip2 (stdlib), none.
numpy is used to speed up RVZ junk regeneration when available.
"""
import argparse
import bz2
import ctypes
import ctypes.util
import hashlib
import lzma
import os
import struct
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None

SECTOR = 0x8000          # Wii disc block (with hashes)
SECTOR_DATA = 0x7C00     # user data per block
GROUP_TOTAL = 0x200000   # 2 MiB hash group (64 sectors)

COMP_NONE, COMP_PURGE, COMP_BZIP2, COMP_LZMA, COMP_LZMA2, COMP_ZSTD = range(6)
COMP_NAMES = ["none", "purge", "bzip2", "lzma", "lzma2", "zstd"]


# --------------------------------------------------------------------------
# zstd
# --------------------------------------------------------------------------
class _Zstd:
    def __init__(self):
        self.mod = None
        self.lib = None
        try:
            import zstandard  # type: ignore
            self.mod = zstandard
            return
        except ImportError:
            pass
        cands = [ctypes.util.find_library("zstd"),
                 "/opt/homebrew/lib/libzstd.dylib", "/usr/local/lib/libzstd.dylib",
                 "/opt/local/lib/libzstd.dylib", "libzstd.so.1"]
        for c in cands:
            if not c:
                continue
            try:
                lib = ctypes.CDLL(c)
                lib.ZSTD_decompress.restype = ctypes.c_size_t
                lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                                ctypes.c_void_p, ctypes.c_size_t]
                lib.ZSTD_isError.restype = ctypes.c_uint
                lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
                lib.ZSTD_getErrorName.restype = ctypes.c_char_p
                lib.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
                self.lib = lib
                return
            except OSError:
                continue

    def decompress(self, data, capacity):
        if self.mod is not None:
            return self.mod.ZstdDecompressor().decompress(data, max_output_size=capacity)
        if self.lib is None:
            sys.exit("error: zstd support missing. Install it with `brew install zstd` "
                     "or `pip3 install zstandard`.")
        dst = ctypes.create_string_buffer(capacity)
        r = self.lib.ZSTD_decompress(dst, capacity, data, len(data))
        if self.lib.ZSTD_isError(r):
            raise IOError("zstd: " + self.lib.ZSTD_getErrorName(r).decode())
        return dst.raw[:r]


_zstd = None


def zstd_decompress(data, capacity):
    global _zstd
    if _zstd is None:
        _zstd = _Zstd()
    return _zstd.decompress(data, capacity)


# --------------------------------------------------------------------------
# Lagged Fibonacci generator used for GameCube/Wii junk data (port of Dolphin's)
# --------------------------------------------------------------------------
LFG_K, LFG_J, LFG_SEED = 521, 32, 17


class LFG:
    def __init__(self, seed_bytes):
        b = [struct.unpack_from(">I", seed_bytes, i * 4)[0] for i in range(LFG_SEED)]
        for i in range(LFG_SEED, LFG_K):
            b.append((((b[i - 17] << 23) & 0xFFFFFFFF) ^ (b[i - 16] >> 9) ^ b[i - 1]) & 0xFFFFFFFF)
        # fold the ">> 18 instead of >> 16" oddity in and store as big-endian bytes
        b = [(x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000) for x in b]
        raw = b"".join(struct.pack(">I", x) for x in b)
        if np is not None:
            self.buf = np.frombuffer(raw, dtype=np.uint8).copy().view(np.uint32)
        else:
            self.buf = list(struct.unpack("<%dI" % LFG_K, raw))  # byte-order irrelevant for XOR
        self.pos = 0
        for _ in range(4):
            self._forward()

    def _forward(self):
        b = self.buf
        if np is not None:
            b[:LFG_J] ^= b[LFG_K - LFG_J:LFG_K]
            # second loop has a dependency distance of 32: process in 32-word strides
            for i in range(LFG_J, LFG_K, LFG_J):
                e = min(i + LFG_J, LFG_K)
                b[i:e] ^= b[i - LFG_J:e - LFG_J]
        else:
            for i in range(LFG_J):
                b[i] ^= b[i + LFG_K - LFG_J]
            for i in range(LFG_J, LFG_K):
                b[i] ^= b[i - LFG_J]

    def _bytes(self):
        if np is not None:
            return self.buf.tobytes()
        return struct.pack("<%dI" % LFG_K, *self.buf)

    def skip(self, count):
        self.pos += count
        while self.pos >= LFG_K * 4:
            self._forward()
            self.pos -= LFG_K * 4

    def get(self, count):
        out = bytearray()
        while count > 0:
            n = min(count, LFG_K * 4 - self.pos)
            out += self._bytes()[self.pos:self.pos + n]
            self.pos += n
            count -= n
            if self.pos == LFG_K * 4:
                self._forward()
                self.pos = 0
        return bytes(out)


def rvz_unpack(data, data_offset, out_size):
    """Expand RVZ 'packed' data (literal runs + LFG junk runs)."""
    out = bytearray()
    p = 0
    while len(out) < out_size and p < len(data):
        size = struct.unpack_from(">I", data, p)[0]
        p += 4
        if size & 0x80000000:
            size &= 0x7FFFFFFF
            lfg = LFG(data[p:p + LFG_SEED * 4])
            p += LFG_SEED * 4
            lfg.skip((data_offset + len(out)) % SECTOR)
            out += lfg.get(size)
        else:
            out += data[p:p + size]
            p += size
    return bytes(out[:out_size])


# --------------------------------------------------------------------------
# RVZ / WIA container
# --------------------------------------------------------------------------
class RVZ:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0x48)
        magic = h[:4]
        if magic == b"RVZ\x01":
            self.rvz = True
        elif magic == b"WIA\x01":
            self.rvz = False
        else:
            raise ValueError("not an RVZ/WIA file")
        (self.version, self.version_compat, disc_size) = struct.unpack_from(">III", h, 4)
        self.iso_size, self.wia_size = struct.unpack_from(">QQ", h, 0x24)
        d = self.f.read(disc_size)
        (self.disc_type, self.compression, self.level, self.chunk_size) = struct.unpack_from(">IIiI", d, 0)
        self.dhead = d[0x10:0x90]
        (self.n_part, self.part_t_size, self.part_off) = struct.unpack_from(">IIQ", d, 0x90)
        (self.n_raw, self.raw_off, self.raw_size, self.n_groups, self.group_off,
         self.group_size) = struct.unpack_from(">IQIIQI", d, 0xB4)
        clen = d[0xD4]
        self.compr_data = d[0xD5:0xD5 + clen]

        self.parts = []
        self.f.seek(self.part_off)
        pt = self.f.read(self.n_part * self.part_t_size)
        for i in range(self.n_part):
            e = pt[i * self.part_t_size:(i + 1) * self.part_t_size]
            key = e[:16]
            pds = [struct.unpack_from(">IIII", e, 16 + j * 16) for j in range(2)]
            self.parts.append({"key": key, "pd": pds})

        self.f.seek(self.raw_off)
        raw = self._decompress(self.f.read(self.raw_size), self.n_raw * 24)
        self.raws = [struct.unpack_from(">QQII", raw, i * 24) for i in range(self.n_raw)]

        gsz = 12 if self.rvz else 8
        self.f.seek(self.group_off)
        g = self._decompress(self.f.read(self.group_size), self.n_groups * gsz)
        self.groups = []
        for i in range(self.n_groups):
            if self.rvz:
                off4, size, packed = struct.unpack_from(">III", g, i * 12)
            else:
                off4, size = struct.unpack_from(">II", g, i * 8)
                packed = 0
            self.groups.append((off4 << 2, size, packed))
        self._cache = {}

    # -- compression ------------------------------------------------------
    def _decompress(self, data, capacity):
        c = self.compression
        if c in (COMP_NONE, COMP_PURGE):
            if c == COMP_PURGE:
                raise NotImplementedError("WIA purge compression is not supported")
            return data
        if c == COMP_ZSTD:
            return zstd_decompress(data, capacity)
        if c == COMP_BZIP2:
            return bz2.decompress(data)
        if c in (COMP_LZMA, COMP_LZMA2):
            if c == COMP_LZMA:
                props = self.compr_data
                pb = props[0] // 45
                lp = (props[0] // 9) % 5
                lc = props[0] % 9
                dict_size = struct.unpack_from("<I", props, 1)[0]
                filt = {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}
            else:
                d = self.compr_data[0]
                dict_size = (2 | (d & 1)) << (d // 2 + 11) if d < 40 else 0xFFFFFFFF
                filt = {"id": lzma.FILTER_LZMA2, "dict_size": dict_size}
            return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[filt]).decompress(data, capacity)
        raise NotImplementedError("compression %d" % c)

    def _group_payload(self, index, out_size, data_offset, exc_lists):
        """Return the user data of a group (exception lists stripped)."""
        key = index
        if key in self._cache:
            return self._cache[key]
        off, size, packed = self.groups[index]
        if size == 0:
            data = bytes(out_size)
            self._cache = {key: data}
            return data
        compressed = bool(size & 0x80000000) if self.rvz else self.compression > COMP_PURGE
        size &= 0x7FFFFFFF
        self.f.seek(off)
        blob = self.f.read(size)
        if compressed:
            blob = self._decompress(blob, out_size + 0x10000 + (packed or 0) + 0x1000)
        p = 0
        for _ in range(exc_lists):
            n = struct.unpack_from(">H", blob, p)[0]
            p += 2 + n * 22
        if exc_lists and not compressed:
            p = (p + 3) & ~3
        blob = blob[p:]
        if self.rvz and packed:
            data = rvz_unpack(blob, data_offset, out_size)
        else:
            data = blob[:out_size]
        if len(data) < out_size:
            data = data + bytes(out_size - len(data))
        self._cache = {key: data}   # one-entry cache is enough for sequential reads
        return data

    # -- raw (non-partition) disc reads -----------------------------------
    def read_raw(self, offset, length):
        out = bytearray()
        if offset < 0x80:
            n = min(length, 0x80 - offset)
            out += self.dhead[offset:offset + n]
            offset += n
            length -= n
        while length > 0:
            for (r_off, r_size, g_idx, n_g) in self.raws:
                skipped = r_off % SECTOR
                start = r_off - skipped
                end = r_off + r_size
                if start <= offset < end:
                    rel = offset - start
                    gi = rel // self.chunk_size
                    gstart = start + gi * self.chunk_size
                    gsize = min(self.chunk_size, end - gstart)
                    data = self._group_payload(g_idx + gi, gsize, gstart, 0)
                    o = offset - gstart
                    n = min(length, gsize - o)
                    out += data[o:o + n]
                    offset += n
                    length -= n
                    break
            else:
                # not covered by raw data (inside a partition) or past the end
                n = length
                out += bytes(n)
                offset += n
                length -= n
        return bytes(out)

    # -- partition data reads (decrypted, hash-stripped space) -------------
    def partition_size(self, pi):
        return sum(pd[1] for pd in self.parts[pi]["pd"]) * SECTOR_DATA

    def read_partition(self, pi, offset, length):
        part = self.parts[pi]
        base_sector = part["pd"][0][0]
        sectors_per_group = self.chunk_size // SECTOR
        exc_lists = max(1, self.chunk_size // GROUP_TOTAL)
        out = bytearray()
        while length > 0:
            sec = offset // SECTOR_DATA + base_sector
            for (first, n_sec, g_idx, n_g) in part["pd"]:
                if first <= sec < first + n_sec:
                    gi = (sec - first) // sectors_per_group
                    g_first = first + gi * sectors_per_group
                    g_secs = min(sectors_per_group, first + n_sec - g_first)
                    g_off = (g_first - base_sector) * SECTOR_DATA
                    g_len = g_secs * SECTOR_DATA
                    data = self._group_payload(g_idx + gi, g_len, g_off, exc_lists)
                    o = offset - g_off
                    n = min(length, g_len - o)
                    out += data[o:o + n]
                    offset += n
                    length -= n
                    break
            else:
                raise IOError("partition read out of range at 0x%x" % offset)
        return bytes(out)


# --------------------------------------------------------------------------
# Wii partition file system
# --------------------------------------------------------------------------
def parse_fst(fst):
    n = struct.unpack_from(">I", fst, 8)[0]
    names = fst[n * 12:]

    def name_at(o):
        e = names.index(b"\0", o)
        return names[o:e].decode("shift_jis", "replace")

    entries = []
    stack = [(n, "")]
    i = 1
    while i < n:
        while stack and i >= stack[-1][0]:
            stack.pop()
        w0, off, size = struct.unpack_from(">III", fst, i * 12)
        is_dir = w0 >> 24
        nm = name_at(w0 & 0xFFFFFF)
        parent = stack[-1][1] if stack else ""
        path = parent + "/" + nm if parent else nm
        if is_dir:
            stack.append((size, path))
            entries.append(("dir", path, 0, 0))
        else:
            entries.append(("file", path, off << 2, size))
        i += 1
    return entries


def open_game_partition(img):
    best = None
    for pi in range(len(img.parts)):
        hdr = img.read_partition(pi, 0, 0x440)
        dol_off, fst_off, fst_size = (struct.unpack_from(">I", hdr, o)[0] << 2 for o in (0x420, 0x424, 0x428))
        if fst_size and (best is None or img.partition_size(pi) > img.partition_size(best[0])):
            best = (pi, hdr, dol_off, fst_off, fst_size)
    if best is None:
        raise IOError("no partition with a file system found")
    return best


def dol_size(img, pi, off):
    h = img.read_partition(pi, off, 0x100)
    end = 0x100
    for i in range(18):
        o = struct.unpack_from(">I", h, i * 4)[0]
        s = struct.unpack_from(">I", h, 0x90 + i * 4)[0]
        if o and s:
            end = max(end, o + s)
    return end


def cmd_info(args):
    img = RVZ(args.image)
    gid = img.dhead[:6].decode("ascii", "replace")
    title = img.dhead[0x20:0x60].split(b"\0")[0].decode("ascii", "replace")
    print("format       : %s v%08x" % ("RVZ" if img.rvz else "WIA", img.version))
    print("game         : %s  %s" % (gid, title))
    print("disc type    : %s" % {1: "GameCube", 2: "Wii"}.get(img.disc_type, img.disc_type))
    print("compression  : %s level %d, chunk 0x%x" % (COMP_NAMES[img.compression], img.level, img.chunk_size))
    print("iso size     : 0x%x  (%.2f GiB)" % (img.iso_size, img.iso_size / 2**30))
    print("partitions   : %d, raw regions %d, groups %d" % (img.n_part, img.n_raw, img.n_groups))
    for pi, p in enumerate(img.parts):
        print("  P%d sectors %s  data 0x%x" % (pi, [(hex(a), b) for a, b, _, _ in p["pd"]], img.partition_size(pi)))
    pi, hdr, dol_off, fst_off, fst_size = open_game_partition(img)
    print("game partition P%d: id %s dol @0x%x  fst @0x%x size 0x%x" % (pi, hdr[:6].decode(), dol_off, fst_off, fst_size))


def cmd_extract(args):
    img = RVZ(args.image)
    pi, hdr, dol_off, fst_off, fst_size = open_game_partition(img)
    out = args.out
    sysd = os.path.join(out, "sys")
    os.makedirs(sysd, exist_ok=True)

    def write(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)

    write(os.path.join(sysd, "boot.bin"), img.read_partition(pi, 0, 0x440))
    write(os.path.join(sysd, "bi2.bin"), img.read_partition(pi, 0x440, 0x2000))
    al = img.read_partition(pi, 0x2440, 0x20)
    al_size = 0x20 + struct.unpack_from(">I", al, 0x14)[0] + struct.unpack_from(">I", al, 0x18)[0]
    write(os.path.join(sysd, "apploader.img"), img.read_partition(pi, 0x2440, al_size))
    dol = img.read_partition(pi, dol_off, dol_size(img, pi, dol_off))
    write(os.path.join(sysd, "main.dol"), dol)
    fst = img.read_partition(pi, fst_off, fst_size)
    write(os.path.join(sysd, "fst.bin"), fst)
    print("main.dol  %8d bytes  sha1 %s" % (len(dol), hashlib.sha1(dol).hexdigest()))

    entries = parse_fst(fst)
    files = [e for e in entries if e[0] == "file"]
    if args.code_only:
        files = [e for e in files if e[1].lower().endswith((".rel", ".rel.lz", ".map", ".str"))]
    total = sum(e[3] for e in files)
    done = 0
    for k, (_, path, off, size) in enumerate(files):
        data = img.read_partition(pi, off, size) if size else b""
        write(os.path.join(out, "files", path), data)
        done += size
        if args.verbose or ".rel" in path.lower():
            print("  %-48s %9d" % (path, size))
        elif k % 64 == 0:
            print("  [%3d%%] %s" % (done * 100 // max(1, total), path), flush=True)
    print("extracted %d files, %.1f MiB -> %s" % (len(files), total / 2**20, out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("info"); a.add_argument("image"); a.set_defaults(fn=cmd_info)
    a = sub.add_parser("extract"); a.add_argument("image"); a.add_argument("out")
    a.add_argument("--code-only", action="store_true", help="only main.dol, rels and symbol maps")
    a.add_argument("-v", "--verbose", action="store_true")
    a.set_defaults(fn=cmd_extract)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
