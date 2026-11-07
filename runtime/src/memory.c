/*
 * memory.c - guest memory reservation, loading the linked image and the
 * Wii low-memory boot environment that the IOS/apploader normally leave.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "rt_internal.h"

uint8_t *rt_base;
RtModule rt_modules[16];
int rt_module_count;

bool rt_mem_init(void) {
    void *p = mmap(NULL, RT_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON
#ifdef MAP_NORESERVE
                   | MAP_NORESERVE
#endif
                   , -1, 0);
    if (p == MAP_FAILED) { rt_log(0, "cannot reserve guest memory"); return false; }
    rt_base = p;
    return true;
}

static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (size) *size = (size_t)n;
    return d;
}

/* image.bin: "NSIM", count, { addr, size, bss, flags, data[size] }* */
bool rt_image_load(const char *path) {
    size_t n;
    uint8_t *d = read_file(path, &n);
    if (!d || n < 8 || memcmp(d, "NSIM", 4)) { rt_log(0, "bad image %s", path); free(d); return false; }
    uint32_t count = be32p(d + 4);
    /* pass 1: clear BSS ranges (the DOL's BSS range spans .sdata/.sdata2,
     * so it must be cleared before initialised sections are copied) */
    size_t p = 8;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = be32p(d + p), size = be32p(d + p + 4), bss = be32p(d + p + 8);
        if (bss) memset(ppc_host(rt_base, addr + size), 0, bss);
        p += 16 + size;
    }
    /* pass 2: copy initialised data */
    p = 8;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = be32p(d + p), size = be32p(d + p + 4);
        p += 16;
        memcpy(ppc_host(rt_base, addr), d + p, size);
        p += size;
    }
    free(d);
    return true;
}

/* Minimal reader for the "modules" array of image.json (no JSON library). */
static const char *find_num(const char *s, const char *key, uint32_t *out) {
    const char *k = strstr(s, key);
    if (!k) return NULL;
    k += strlen(key);
    while (*k == ' ' || *k == ':' || *k == '"') k++;
    *out = (uint32_t)strtoul(k, NULL, 0);
    return k;
}

bool rt_modules_load(const char *json_path) {
    char *js = (char *)read_file(json_path, NULL);
    if (!js) return false;
    char *m = strstr(js, "\"modules\"");
    while (m && (m = strstr(m, "\"name\"")) && rt_module_count < 16) {
        RtModule *mod = &rt_modules[rt_module_count];
        memset(mod, 0, sizeof *mod);
        char *end = strstr(m, "\"fix_size\"");
        if (!end) break;
        find_num(m, "\"id\"", &mod->id);
        find_num(m, "\"prolog\"", &mod->prolog);
        find_num(m, "\"epilog\"", &mod->epilog);
        find_num(m, "\"unresolved\"", &mod->unresolved);
        char *s = strstr(m, "\"sections\"");
        char *send = s ? strchr(s, ']') : NULL;
        uint32_t lo = 0xFFFFFFFF, hi = 0;
        while (s && (s = strstr(s, "\"addr\"")) && s < send && mod->sec_count < 24) {
            uint32_t a, z;
            find_num(s, "\"addr\"", &a);
            find_num(s, "\"size\"", &z);
            int k = mod->sec_count++;
            mod->sec_addr[k] = a;
            mod->sec_size[k] = z;
            char *obj_end = strchr(s, '}');
            char *ex = strstr(s, "\"exec\"");
            mod->sec_exec[k] = ex && ex < obj_end && strstr(ex, "true") && strstr(ex, "true") < strchr(ex, ',');
            char *bs = strstr(s, "\"bss\"");
            mod->sec_bss[k] = bs && bs < obj_end && strstr(bs, "true") && strstr(bs, "true") < obj_end;
            if (a && z) {
                if (mod->sec_bss[k]) { mod->bss_addr = a; mod->bss_size = z; }
                else { if (a < lo) lo = a; if (a + z > hi) hi = a + z; }
            }
            s = obj_end ? obj_end : s + 1;
        }
        if (hi > lo) {
            /* keep a pristine copy so re-linking a module restores its data */
            mod->pristine_lo = lo;
            mod->pristine_hi = hi;
            mod->pristine = malloc(hi - lo);
            memcpy(mod->pristine, ppc_host(rt_base, lo), hi - lo);
        }
        rt_module_count++;
        m = end;
    }
    free(js);
    rt_log(1, "modules: %d pre-linked", rt_module_count);
    return true;
}

static void w32(uint32_t ea, uint32_t v) { st32(rt_base, ea, v); }

static bool load_sys_file(const char *name, uint32_t ea, uint32_t *size_out) {
    char path[1200];
    snprintf(path, sizeof path, "%s/sys/%s", rt_settings.game_dir, name);
    size_t n;
    uint8_t *d = read_file(path, &n);
    if (!d) return false;
    memcpy(ppc_host(rt_base, ea), d, n);
    free(d);
    if (size_out) *size_out = (uint32_t)n;
    return true;
}

/* Recreates what the system menu / apploader leave in low memory
 * (mirrors Dolphin's HLE boot for Wii titles). */
void rt_boot_setup(PPCContext *ctx) {
    uint32_t sz = 0;
    if (!load_sys_file("boot.bin", 0x80000000, &sz)) rt_log(1, "warning: sys/boot.bin not found - disc header missing");
    memset(ppc_host(rt_base, 0x80000020), 0, 0x20);
    w32(0x80000020, 0x0D15EA5E);      /* boot magic */
    w32(0x80000024, 0x00000001);      /* version */
    w32(0x80000028, RT_MEM1_SIZE);    /* physical MEM1 */
    w32(0x8000002C, 0x00000023);      /* console type: retail RVL */
    w32(0x80000030, 0x00000000);      /* ArenaLo (OSInit derives it) */
    w32(0x80000034, 0x817FEC60);      /* ArenaHi */
    w32(0x800000CC, 0x00000001);      /* video mode: PAL */
    w32(0x800000EC, 0x81800000);      /* debug monitor location */
    w32(0x800000F0, RT_MEM1_SIZE);    /* simulated memory size */
    w32(0x800000F8, 0x0E7BE2C0);      /* bus clock 243 MHz */
    w32(0x800000FC, 0x2B73A840);      /* CPU clock 729 MHz */
    w32(0x800030D8, 0xFFFFFFFF);      /* EXI */
    w32(0x80003100, RT_MEM1_SIZE);
    w32(0x80003104, RT_MEM1_SIZE);
    w32(0x80003118, RT_MEM2_SIZE);
    w32(0x8000311C, RT_MEM2_SIZE);
    w32(0x80003120, 0x93400000);      /* MEM2 end */
    w32(0x80003124, 0x90000800);      /* MEM2 arena lo */
    w32(0x80003128, 0x933E0000);      /* MEM2 arena hi */
    w32(0x80003130, 0x933E0000);      /* IOS IPC buffer */
    w32(0x80003134, 0x93400000);
    w32(0x80003138, 0x00000011);      /* hollywood revision */
    w32(0x80003140, 0x00230000 | 0x1E19); /* IOS35 (NSMBW title IOS), rev */
    w32(0x80003144, 0x00062507);      /* IOS build date */
    w32(0x80003158, 0x0000FF16);      /* DDR vendor */
    w32(0x8000315C, 0x80000000);
    w32(0x80003160, 0x00000000);
    w32(0x80003180, ld32(rt_base, 0x80000000)); /* game id */
    w32(0x80003188, 0x00230000);

    /* bi2 + FST at the top of MEM1, below the arena */
    uint32_t bi2 = 0x817FE000;
    if (load_sys_file("bi2.bin", bi2, NULL)) w32(0x800000F4, bi2);
    char path[1200];
    snprintf(path, sizeof path, "%s/sys/fst.bin", rt_settings.game_dir);
    size_t fst_n = 0;
    uint8_t *fst = read_file(path, &fst_n);
    if (fst) {
        uint32_t fst_addr = (bi2 - (uint32_t)fst_n) & ~0x1Fu;
        memcpy(ppc_host(rt_base, fst_addr), fst, fst_n);
        free(fst);
        w32(0x80000038, fst_addr);
        w32(0x8000003C, (uint32_t)fst_n);
        w32(0x80000034, fst_addr);       /* ArenaHi below FST */
    } else {
        rt_log(1, "warning: sys/fst.bin not found - DVD file lookups will fail");
    }

    memset(ctx, 0, sizeof *ctx);
    ctx->msr = 0x00002032;               /* FP | IR | DR */
    ctx->spr[920] = 0xE0000000;          /* HID2: paired singles + locked cache enabled */
    ctx->spr[1008] = 0x0011C464;         /* HID0 */
    ctx->r[1] = 0x816FFFF0;              /* temporary stack until __init_registers */
}
