/*
 * ios.c - high-level emulation of the IOS IPC interface (0xCD000000).
 *
 * The PPC side writes the physical address of a 0x40-byte request to
 * IPC_PPCMSG and sets X1 in IPC_PPCCTRL. We service the request immediately,
 * write the result, and signal ack + reply (Y2/Y1) with the Hollywood IPC
 * interrupt, exactly like the real Starlet would (just with zero latency).
 *
 * Devices:
 *   /dev/di            disc reads served from the extracted file system (FST map)
 *   /dev/fs + paths    NAND backed by a host folder (saves, SYSCONF, title data)
 *   /dev/es            title identity for SMNP01
 *   /dev/stm/...       power/reset: immediate + eventhook (never fires)
 *   /dev/usb/oh1/...   Bluetooth: accepted but idle -> input comes via WPAD/KPAD HLE
 *   everything else    -6 (not found)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "rt_internal.h"

enum { IPC_OPEN = 1, IPC_CLOSE, IPC_READ, IPC_WRITE, IPC_SEEK, IPC_IOCTL, IPC_IOCTLV, IPC_REPLY };
enum { DEV_NONE, DEV_DI, DEV_FS, DEV_FILE, DEV_ES, DEV_STM, DEV_STM_EVENT, DEV_BT, DEV_NET, DEV_OTHER };

typedef struct { int type; FILE *f; char path[512]; } Fd;
static Fd g_fd[32];
static uint32_t g_ppcmsg, g_ppcctrl, g_armmsg, g_armctrl;
static char g_nand_root[1024];

/* ------------------------------------------------------------ disc map */
typedef struct { uint32_t off, size; char path[600]; } DiscFile;
static DiscFile *g_disc;
static int g_ndisc;
static FILE *g_disc_fp; static int g_disc_open_idx = -1;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int disc_cmp(const void *a, const void *b) {
    const DiscFile *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void disc_map_build(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/sys/fst.bin", rt_settings.game_dir);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *fst = malloc((size_t)n);
    if (fread(fst, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(fst); return; }
    fclose(f);
    uint32_t count = be32(fst + 8);
    const char *names = (const char *)fst + count * 12;
    g_disc = calloc(count + 8, sizeof *g_disc);
    /* directory stack for path building */
    struct { uint32_t end; char path[600]; } st[64]; int sp = 0;
    st[0].end = count; st[0].path[0] = 0;
    for (uint32_t i = 1; i < count; i++) {
        while (sp > 0 && i >= st[sp].end) sp--;
        const uint8_t *e = fst + i * 12;
        int is_dir = e[0];
        const char *nm = names + (be32(e) & 0xFFFFFF);
        char full[600];
        snprintf(full, sizeof full, "%s%s%s", st[sp].path, st[sp].path[0] ? "/" : "", nm);
        if (is_dir) { sp++; st[sp].end = be32(e + 8); snprintf(st[sp].path, sizeof st[sp].path, "%s", full); }
        else {
            DiscFile *d = &g_disc[g_ndisc++];
            d->off = be32(e + 4);          /* in 4-byte units on Wii */
            d->size = be32(e + 8);
            snprintf(d->path, sizeof d->path, "files/%s", full);
        }
    }
    free(fst);
    qsort(g_disc, (size_t)g_ndisc, sizeof *g_disc, disc_cmp);
    rt_log(1, "disc map: %d files", g_ndisc);
}

/* read `len` bytes at partition offset (bytes) */
static int disc_read(uint64_t off, uint8_t *dst, uint32_t len) {
    memset(dst, 0, len);
    /* system area */
    if (off < 0x2440 + 0x20000) {
        const char *nm = off < 0x440 ? "boot.bin" : off < 0x2440 ? "bi2.bin" : "apploader.img";
        uint64_t base = off < 0x440 ? 0 : off < 0x2440 ? 0x440 : 0x2440;
        char p[1200]; snprintf(p, sizeof p, "%s/sys/%s", rt_settings.game_dir, nm);
        FILE *f = fopen(p, "rb");
        if (f) { fseek(f, (long)(off - base), SEEK_SET); size_t r = fread(dst, 1, len, f); (void)r; fclose(f); }
        return 0;
    }
    uint32_t word = (uint32_t)(off >> 2);
    int lo = 0, hi = g_ndisc - 1, k = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_disc[m].off <= word) { k = m; lo = m + 1; } else hi = m - 1; }
    while (len && k >= 0 && k < g_ndisc) {
        DiscFile *d = &g_disc[k];
        uint64_t fstart = (uint64_t)d->off << 2, fend = fstart + d->size;
        if (off >= fend) { k++; if (k < g_ndisc && ((uint64_t)g_disc[k].off << 2) > off) { uint64_t gap = ((uint64_t)g_disc[k].off << 2) - off; if (gap >= len) break; dst += gap; off += gap; len -= (uint32_t)gap; } continue; }
        if (off < fstart) { uint64_t gap = fstart - off; if (gap >= len) break; dst += gap; off += gap; len -= (uint32_t)gap; }
        if (g_disc_open_idx != k) {
            if (g_disc_fp) fclose(g_disc_fp);
            char p[1400]; snprintf(p, sizeof p, "%s/%s", rt_settings.game_dir, d->path);
            g_disc_fp = fopen(p, "rb");
            g_disc_open_idx = k;
            if (getenv("RT_DI_TRACE")) rt_log(0, "disc: read %s", d->path);
            if (!g_disc_fp) rt_log(1, "disc: missing %s", p);
        }
        uint32_t n = (uint32_t)((fend - off) < len ? (fend - off) : len);
        if (g_disc_fp) { fseek(g_disc_fp, (long)(off - fstart), SEEK_SET); size_t r = fread(dst, 1, n, g_disc_fp); (void)r; }
        dst += n; off += n; len -= n; k++;
    }
    return 0;
}

/* ------------------------------------------------------------ NAND */
static void mkdirs(const char *path) {
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static void nand_path(const char *guest, char *out, size_t n) { snprintf(out, n, "%s%s", g_nand_root, guest); }

/* SYSCONF: "SCv0", u16 count, u16 offsets[count+1], items..., "SCed" at 0x3FFC */
static void sysconf_write_default(const char *path) {
    uint8_t buf[0x4000]; memset(buf, 0, sizeof buf);
    struct { const char *name; int type; uint8_t val[8]; int len; } items[] = {
        { "IPL.LNG", 3, { 1 }, 1 },         /* language: English */
        { "IPL.AR", 3, { 1 }, 1 },          /* 16:9 */
        { "IPL.E60", 3, { 1 }, 1 },         /* EuRGB60 allowed */
        { "IPL.PGS", 3, { 1 }, 1 },         /* progressive scan */
        { "IPL.SND", 3, { 1 }, 1 },         /* stereo */
        { "IPL.SSV", 3, { 0 }, 1 },         /* screen saver off */
        { "IPL.CB", 5, { 0 }, 4 },          /* counter bias */
        { "BT.SENS", 5, { 0, 0, 0, 3 }, 4 },
        { "BT.MOT", 3, { 1 }, 1 },
        { "BT.SPKV", 3, { 0x58 }, 1 },
        { "BT.BAR", 3, { 1 }, 1 },
        { "IPL.SADR", 0, { 0x6E }, 1 },     /* country placeholder (big array type below) */
    };
    int n = (int)(sizeof items / sizeof items[0]) - 1;   /* SADR omitted: optional */
    memcpy(buf, "SCv0", 4);
    buf[4] = 0; buf[5] = (uint8_t)n;
    int p = 6 + 2 * (n + 1);
    for (int i = 0; i < n; i++) {
        buf[6 + 2 * i] = (uint8_t)(p >> 8); buf[7 + 2 * i] = (uint8_t)p;
        int nl = (int)strlen(items[i].name);
        buf[p++] = (uint8_t)((items[i].type << 5) | (nl - 1));
        memcpy(buf + p, items[i].name, (size_t)nl); p += nl;
        memcpy(buf + p, items[i].val, (size_t)items[i].len); p += items[i].len;
    }
    buf[6 + 2 * n] = (uint8_t)(p >> 8); buf[7 + 2 * n] = (uint8_t)p;
    memcpy(buf + 0x3FFC, "SCed", 4);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, sizeof buf, f); fclose(f); }
}

static void nand_init(void) {
    const char *home = getenv("HOME");
#ifdef __APPLE__
    snprintf(g_nand_root, sizeof g_nand_root, "%s/Library/Application Support/NSMBWRecomp/nand", home ? home : ".");
#else
    snprintf(g_nand_root, sizeof g_nand_root, "%s/.local/share/nsmbw-recomp/nand", home ? home : ".");
#endif
    char p[1200];
    snprintf(p, sizeof p, "%s/shared2/sys", g_nand_root); mkdirs(p);
    snprintf(p, sizeof p, "%s/title/00010000/534d4e50/data", g_nand_root); mkdirs(p);
    snprintf(p, sizeof p, "%s/tmp", g_nand_root); mkdirs(p);
    snprintf(p, sizeof p, "%s/shared2/sys/SYSCONF", g_nand_root);
    struct stat sb;
    if (stat(p, &sb) != 0) sysconf_write_default(p);
    /* aspect ratio follows Settings > Graphics > Widescreen */
    FILE *f = fopen(p, "r+b");
    if (f) {
        uint8_t buf[0x4000];
        size_t n = fread(buf, 1, sizeof buf, f);
        for (size_t i = 0; i + 8 <= n; i++)
            if (!memcmp(buf + i, "IPL.AR", 6) && i > 0 && (buf[i - 1] & 0x1F) == 5) {
                uint8_t ar = rt_settings.widescreen ? 1 : 0;
                if (buf[i + 6] != ar) { buf[i + 6] = ar; fseek(f, (long)(i + 6), SEEK_SET); fwrite(&ar, 1, 1, f); }
                rt_log(1, "nand: SYSCONF aspect ratio %s", ar ? "16:9" : "4:3");
                break;
            }
        fclose(f);
    }
}

/* ------------------------------------------------------------ requests */
static int alloc_fd(void) { for (int i = 0; i < 32; i++) if (!g_fd[i].type) return i; return -22; }

static void cstr(uint32_t ea, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) { char c = (char)ld8(rt_base, ea + (uint32_t)i); if (!c) break; out[i] = c; }
    out[i] = 0;
}

static int32_t ios_open(const char *path, uint32_t mode) {
    int fd = alloc_fd();
    if (fd < 0) return fd;
    Fd *d = &g_fd[fd];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    if (!strcmp(path, "/dev/di")) d->type = DEV_DI;
    else if (!strcmp(path, "/dev/fs")) d->type = DEV_FS;
    else if (!strcmp(path, "/dev/es")) d->type = DEV_ES;
    else if (!strcmp(path, "/dev/stm/immediate")) d->type = DEV_STM;
    else if (!strcmp(path, "/dev/stm/eventhook")) d->type = DEV_STM_EVENT;
    else if (!strncmp(path, "/dev/usb/oh1", 12)) d->type = DEV_BT;
    else if (!strncmp(path, "/dev/net", 8)) d->type = DEV_NET;
    else if (!strncmp(path, "/dev/", 5)) { rt_log(2, "ios: open %s -> not found", path); return -6; }
    else {
        char hp[1600]; nand_path(path, hp, sizeof hp);
        const char *m = (mode & 2) ? "r+b" : "rb";
        d->f = fopen(hp, m);
        if (!d->f && (mode & 2)) d->f = fopen(hp, "w+b");
        if (!d->f) { rt_log(2, "ios: open %s -> -106", path); return -106; }
        d->type = DEV_FILE;
    }
    rt_log(2, "ios: open %s mode %u -> fd %d", path, mode, fd);
    return fd;
}

static int32_t ios_ioctl(Fd *d, uint32_t cmd, uint32_t in, uint32_t inlen, uint32_t out, uint32_t outlen) {
    switch (d->type) {
    case DEV_DI:
        switch (cmd) {
        case 0x71: {   /* DVDLowRead(len, offset>>2) */
            uint32_t len = ld32(rt_base, in + 4);
            uint64_t off = (uint64_t)ld32(rt_base, in + 8) << 2;
            if (len > outlen) len = outlen;
            uint8_t *tmp = malloc(len);
            disc_read(off, tmp, len);
            memcpy(ppc_host(rt_base, out), tmp, len);
            free(tmp);
            return 1;
        }
        case 0x70: {   /* DVDLowReadDiskID */
            uint8_t tmp[0x20]; disc_read(0, tmp, 0x20);
            memcpy(ppc_host(rt_base, out), tmp, outlen < 0x20 ? outlen : 0x20);
            return 1;
        }
        case 0x88: st32(rt_base, out, 0); return 1;           /* GetCoverStatus: closed */
        case 0x7A: st32(rt_base, out, 2); return 1;           /* GetCoverRegister: cover closed */
        case 0xE0: st32(rt_base, out, 0); return 1;           /* RequestError: none */
        case 0x8E: case 0x8D: case 0x95: case 0x96: case 0x86: case 0x8C: case 0xA8: case 0xE3: case 0xD9:
            return 1;
        default:
            rt_log(2, "ios: di ioctl %02X", cmd);
            return 1;
        }
    case DEV_FS:
        switch (cmd) {
        case 0x02: {                                                          /* GetStats: a mostly empty NAND */
            static const uint32_t stats[7] = { 0x4000, 0x6000, 0x1000, 0, 0, 0x4000, 0x400 };
            for (uint32_t k = 0; k < 7 && (k + 1) * 4 <= outlen; k++) st32(rt_base, out + 4 * k, stats[k]);
            return 0;
        }
        case 0x03: { char p[128], hp[1600]; cstr(in + 6, p, sizeof p); nand_path(p, hp, sizeof hp); mkdirs(hp); return 0; }  /* CreateDir */
        case 0x09: { char p[128], hp[1600]; cstr(in + 6, p, sizeof p); nand_path(p, hp, sizeof hp);   /* CreateFile */
                     rt_log(1, "nand: create %s", p);
                     struct stat sb; if (stat(hp, &sb) == 0) return -105;
                     FILE *f = fopen(hp, "wb"); if (f) fclose(f); return f ? 0 : -102; }
        case 0x07: { char p[128], hp[1600]; cstr(in, p, sizeof p); nand_path(p, hp, sizeof hp);       /* Delete */
                     return remove(hp) == 0 ? 0 : -106; }
        case 0x08: { char a[128], b[128], ha[1600], hb[1600]; cstr(in, a, sizeof a); cstr(in + 64, b, sizeof b);
                     nand_path(a, ha, sizeof ha); nand_path(b, hb, sizeof hb); return rename(ha, hb) == 0 ? 0 : -106; }
        case 0x06: {                                                          /* GetAttr(path) */
            char p[128], hp[1600]; cstr(in, p, sizeof p); nand_path(p, hp, sizeof hp);
            struct stat sb;
            if (stat(hp, &sb) != 0) { rt_log(2, "ios: fs GetAttr %s -> -106", p); return -106; }
            memset(ppc_host(rt_base, out), 0, outlen);
            if (outlen >= 0x4C) {
                st32(rt_base, out + 0x00, 0x00001000);                        /* owner id */
                st16(rt_base, out + 0x04, 0x3031);                            /* group id ("01") */
                for (int k = 0; k < 64 && p[k]; k++) st8(rt_base, out + 0x06 + k, (uint8_t)p[k]);
                st8(rt_base, out + 0x46, 3); st8(rt_base, out + 0x47, 3); st8(rt_base, out + 0x48, 3);   /* rw for owner/group/other */
                st8(rt_base, out + 0x49, 0);                                  /* attributes */
            }
            return 0;
        }
        case 0x05: case 0x0A: return 0;                                       /* SetAttr, SetFileVersionControl */
        default: rt_log(2, "ios: fs ioctl %u", cmd); return 0;
        }
    case DEV_FILE:
        if (cmd == 0x0B) {   /* GetFileStats: size, position */
            long pos = ftell(d->f); fseek(d->f, 0, SEEK_END); long sz = ftell(d->f); fseek(d->f, pos, SEEK_SET);
            st32(rt_base, out, (uint32_t)sz); st32(rt_base, out + 4, (uint32_t)pos);
            return 0;
        }
        return -4;
    case DEV_STM:
        return 0;
    default:
        (void)inlen;
        rt_log(2, "ios: ioctl %u on %s", cmd, d->path);
        return 0;
    }
}

static int32_t ios_ioctlv(Fd *d, uint32_t cmd, uint32_t nin, uint32_t nio, uint32_t vec) {
    if (d->type == DEV_ES) {
        uint32_t o0 = ld32(rt_base, vec + nin * 8), o0len = ld32(rt_base, vec + nin * 8 + 4);
        switch (cmd) {
        case 0x20: st32(rt_base, o0, 0x00010000); st32(rt_base, o0 + 4, 0x534D4E50); return 0;   /* GetTitleID */
        case 0x1D: {   /* ES_GetDataDir(titleID) -> "/title/<hi>/<lo>/data" */
            const char *dir = "/title/00010000/534d4e50/data";
            uint32_t ob = ld32(rt_base, vec + nin * 8) | 0x80000000u, olen = ld32(rt_base, vec + nin * 8 + 4);
            for (uint32_t k = 0; k < olen; k++) st8(rt_base, ob + k, k < strlen(dir) ? (uint8_t)dir[k] : 0);
            return 0;
        }
        case 0x1E: case 0x1B: if (o0len >= 4) st32(rt_base, o0, 0); return 0;
        default: rt_log(2, "ios: es ioctlv %02X (in %u io %u)", cmd, nin, nio); return 0;
        }
    }
    if (d->type == DEV_FS && cmd == 0x04) {            /* ReadDir(path [, max]) -> [names,] count */
        char p[128], hp[1600];
        cstr(ld32(rt_base, vec) | 0x80000000u, p, sizeof p);
        nand_path(p, hp, sizeof hp);
        struct stat sb;
        if (stat(hp, &sb) != 0) { rt_log(2, "ios: fs ReadDir %s -> -106", p); return -106; }
        if (!S_ISDIR(sb.st_mode)) return -101;          /* a file: NANDGetType tells them apart this way */
        DIR *dir = opendir(hp);
        if (!dir) return -102;
        uint32_t max = nin > 1 ? ld32(rt_base, ld32(rt_base, vec + 8) | 0x80000000u) : 0;
        uint32_t names = nin > 1 && nio > 1 ? ld32(rt_base, vec + nin * 8) | 0x80000000u : 0;
        uint32_t cnt_ptr = ld32(rt_base, vec + (nin + nio - 1) * 8) | 0x80000000u;
        uint32_t count = 0, w = 0;
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.') continue;
            if (names) {
                if (count >= max) break;
                size_t l = strlen(e->d_name); if (l > 12) l = 12;
                for (size_t k = 0; k < l; k++) st8(rt_base, names + w + (uint32_t)k, (uint8_t)e->d_name[k]);
                st8(rt_base, names + w + (uint32_t)l, 0);
                w += (uint32_t)l + 1;
            }
            count++;
        }
        closedir(dir);
        st32(rt_base, cnt_ptr, count);
        rt_log(2, "ios: fs ReadDir %s -> %u entries", p, count);
        return 0;
    }
    if (d->type == DEV_FS && cmd == 0x0C) {            /* GetUsage(path) -> clusters, inodes */
        if (nio >= 2) {
            st32(rt_base, ld32(rt_base, vec + nin * 8) | 0x80000000u, 4);
            st32(rt_base, ld32(rt_base, vec + nin * 8 + 8) | 0x80000000u, 3);
        }
        return 0;
    }
    if (d->type == DEV_DI) return 1;                    /* OpenPartition etc */
    rt_log(2, "ios: ioctlv %u on %s", cmd, d->path);
    return 0;
}

static int handle(uint32_t req, int *defer) {
    uint32_t cmd = ld32(rt_base, req), fd = ld32(rt_base, req + 8);
    uint32_t a0 = ld32(rt_base, req + 12), a1 = ld32(rt_base, req + 16), a2 = ld32(rt_base, req + 20),
             a3 = ld32(rt_base, req + 24), a4 = ld32(rt_base, req + 28);
    *defer = 0;
    rt_log(3, "ipc: req %08X cmd %u fd %d args %08X %08X %08X %08X %08X", req, cmd, (int)fd, a0, a1, a2, a3, a4);
    if (cmd == IPC_OPEN) { char p[256]; cstr(a0 | 0x80000000u, p, sizeof p); return ios_open(p, a1); }
    if (fd >= 32 || !g_fd[fd].type) return -4;
    Fd *d = &g_fd[fd];
    switch (cmd) {
    case IPC_CLOSE: if (d->f) fclose(d->f); memset(d, 0, sizeof *d); return 0;
    case IPC_READ:
        if (d->type == DEV_FILE) return (int32_t)fread(ppc_host(rt_base, a0), 1, a1, d->f);
        return 0;
    case IPC_WRITE:
        if (d->type == DEV_FILE) return (int32_t)fwrite(ppc_host(rt_base, a0), 1, a1, d->f);
        return (int32_t)a1;
    case IPC_SEEK:
        if (d->type == DEV_FILE) { fseek(d->f, (long)(int32_t)a0, (int)a1); return (int32_t)ftell(d->f); }
        return 0;
    case IPC_IOCTL:
        if (d->type == DEV_STM_EVENT) { *defer = 1; return 0; }   /* replies only on power/reset */
        return ios_ioctl(d, a0, a1 | 0x80000000u, a2, a3 | 0x80000000u, a4);
    case IPC_IOCTLV: {
        if (d->type == DEV_BT) {
            /* USB ioctlv 0 = control transfer (HCI commands): accept immediately.
             * 1/2 = bulk/interrupt reads (ACL data, HCI events): no Bluetooth
             * radio here, so they stay pending; Wii Remotes come via WPAD HLE. */
            if (a0 == 0) { rt_log(3, "ios: bt control transfer accepted"); return (int32_t)ld32(rt_base, (a3 | 0x80000000u) + 8 * 6 + 4); }
            *defer = 1; return 0;
        }
        uint32_t vec = a3 | 0x80000000u;
        /* convert physical buffer pointers in the vector to effective addresses for helpers */
        for (uint32_t i = 0; i < a1 + a2; i++) st32(rt_base, vec + i * 8, ld32(rt_base, vec + i * 8) | 0x80000000u);
        return ios_ioctlv(d, a0, a1, a2, vec);
    }
    }
    return -4;
}

/* Replies are delivered with a small latency, like the real Starlet. With
 * zero latency the reply interrupt can fire before the requesting thread has
 * gone to sleep on the reply queue, and the wake-up would be lost. */
typedef struct { uint32_t req, msg; int32_t ret; uint64_t due; } PendingReply;
static PendingReply g_pending[16];
static int g_npending;
#define IPC_REPLY_LATENCY_TICKS 3000   /* ~50 us of timebase */

static void ipc_signal(void);

int rt_ios_poll(void) {
    if (!g_npending) return 0;
    if (g_ppcctrl & 0x4) return 1;                    /* previous reply not yet acknowledged */
    PendingReply *p = &g_pending[0];
    if (rt_timebase() < p->due) return 1;
    uint32_t cmd = ld32(rt_base, p->req);
    st32(rt_base, p->req + 4, (uint32_t)p->ret);
    st32(rt_base, p->req + 8, cmd);
    st32(rt_base, p->req, IPC_REPLY);
    g_armmsg = p->msg;
    g_ppcctrl |= 0x4;                                  /* Y1: reply ready */
    memmove(&g_pending[0], &g_pending[1], sizeof g_pending[0] * (size_t)(--g_npending));
    ipc_signal();
    return g_npending > 0;
}

/* IPC interrupt line: reply ready (Y1) with IY1, or ack (Y2) with IY2 */
int rt_ios_irq_line(void) {
    return ((g_ppcctrl & 0x4) && (g_ppcctrl & 0x10)) || ((g_ppcctrl & 0x2) && (g_ppcctrl & 0x20));
}

static void ipc_signal(void) {
    if (rt_ios_irq_line()) ppc_irq_pending = 1;
}

void rt_ios_ipc_write(uint32_t reg, uint32_t v) {
    switch (reg) {
    case 0x0: g_ppcmsg = v; break;
    case 0x4: {
        /* bits: 0 X1 execute, 1 Y2 ack, 2 Y1 reply, 3 X2 relaunch, 4 IY1, 5 IY2 */
        g_ppcctrl = (g_ppcctrl & 0x06u & ~(v & 0x06u)) | (v & 0x39u);   /* Y1/Y2 are write-1-to-clear */
        if (v & 1) {
            uint32_t req = g_ppcmsg | 0x80000000u;
            int defer;
            int32_t ret = handle(req, &defer);
            g_ppcctrl &= ~1u;
            g_ppcctrl |= 0x2;             /* Y2: ack */
            if (!defer && g_npending < 16) {
                g_pending[g_npending++] = (PendingReply){ req, g_ppcmsg, ret, rt_timebase() + IPC_REPLY_LATENCY_TICKS };
                ppc_irq_pending = 1;
            }
            ipc_signal();
        }
        break;
    }
    case 0xC: g_armctrl = v; break;
    }
}

uint32_t rt_ios_ipc_read(uint32_t reg) {
    switch (reg) {
    case 0x0: return g_ppcmsg;
    case 0x4: return g_ppcctrl;
    case 0x8: return g_armmsg;
    case 0xC: return g_armctrl;
    }
    return 0;
}

void rt_ios_init(void) {
    memset(g_fd, 0, sizeof g_fd);
    nand_init();
    disc_map_build();
}

/* save states: IPC registers and queued replies are plain data; open NAND
 * files are saved as (path, position) and reopened on load */
void rt_ios_ss_register(void) {
    rt_ss_add(&g_ppcmsg, sizeof g_ppcmsg); rt_ss_add(&g_ppcctrl, sizeof g_ppcctrl);
    rt_ss_add(&g_armmsg, sizeof g_armmsg); rt_ss_add(&g_armctrl, sizeof g_armctrl);
    rt_ss_add(g_pending, sizeof g_pending); rt_ss_add(&g_npending, sizeof g_npending);
}
typedef struct { int type; long pos; char path[512]; } FdSave;
size_t rt_ios_ss_size(void) { return sizeof(FdSave) * 32; }
void rt_ios_ss_save(void *buf) {
    FdSave *o = buf;
    for (int i = 0; i < 32; i++) {
        o[i].type = g_fd[i].type;
        o[i].pos = g_fd[i].f ? ftell(g_fd[i].f) : 0;
        memcpy(o[i].path, g_fd[i].path, sizeof o[i].path);
    }
}
void rt_ios_ss_load(const void *buf) {
    const FdSave *in = buf;
    for (int i = 0; i < 32; i++) {
        if (g_fd[i].f) fclose(g_fd[i].f);
        memset(&g_fd[i], 0, sizeof g_fd[i]);
        g_fd[i].type = in[i].type;
        memcpy(g_fd[i].path, in[i].path, sizeof g_fd[i].path);
        if (in[i].type == DEV_FILE) {
            char hp[1600]; nand_path(in[i].path, hp, sizeof hp);
            g_fd[i].f = fopen(hp, "r+b");
            if (!g_fd[i].f) g_fd[i].f = fopen(hp, "rb");
            if (g_fd[i].f) fseek(g_fd[i].f, in[i].pos, SEEK_SET);
        }
    }
}
