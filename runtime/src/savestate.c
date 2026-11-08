/*
 * savestate.c - save / load state, within one app session.
 *
 * A request from the UI is serviced by the running guest thread at its next
 * interrupt check (all other guest threads are then parked waiting for the
 * baton, and the GX thread is drained). A state holds:
 *   - guest memory: MEM1 plus the pre-linked module arena, and MEM2,
 *   - hardware / runtime state registered by the modules (rt_ss_add),
 *   - open NAND files (path + position),
 *   - every guest thread's registers and host stack (hle_os.c).
 * Host stacks hold the recompiled guest call frames and are tied to this
 * process's addresses, so states live in memory and are lost when the app
 * quits. NAND contents (the game's own saves) are not rewound.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt_internal.h"

#define SS_SLOTS 3
#define MEM1_SPAN 0x02000000u          /* MEM1 + module arena above it */
#define MEM2_BASE 0x10000000u
#define MEM2_SPAN 0x04000000u

void *rt_ss_threads_alloc(void);
int rt_ss_threads_save(void *set, jmp_buf *self_jb, uintptr_t self_sp);
void rt_ss_threads_load(void *set);
void rt_mmio_ss_register(void);
void rt_ios_ss_register(void);
size_t rt_ios_ss_size(void);
void rt_ios_ss_save(void *buf);
void rt_ios_ss_load(const void *buf);
void rt_gx_ss_register(void);
void rt_gx_soft_ss_register(void);
void rt_gx_soft_ss_loaded(void);

typedef struct { void *p; size_t n; } Blob;
static Blob g_blobs[64];
static int g_nblobs;
static size_t g_blob_total;

void rt_ss_add(void *p, size_t n) {
    if (g_nblobs < 64) { g_blobs[g_nblobs].p = p; g_blobs[g_nblobs].n = n; g_nblobs++; g_blob_total += n; }
}

typedef struct {
    int valid;
    time_t when;
    uint8_t *mem1, *mem2, *blobs, *ios;
    void *threads;
} Slot;
static Slot g_slots[SS_SLOTS];

volatile int rt_ss_request;
static volatile int g_req_op, g_req_slot;     /* 1 save, 2 load */
static char g_msg[128];
static volatile int g_msg_seq;
static int g_cur_slot = -1;                    /* slot being loaded (for the post-load message) */

static void set_msg(const char *m) { snprintf(g_msg, sizeof g_msg, "%s", m); g_msg_seq++; }

static void ensure_init(void) {
    static int done;
    if (done) return;
    done = 1;
    rt_mmio_ss_register();
    rt_ios_ss_register();
    rt_gx_ss_register();
    rt_gx_soft_ss_register();
}

bool rt_savestate_request(bool op_save, int slot) {
    if (slot < 0 || slot >= SS_SLOTS || rt_ss_request) return false;
    if (!op_save && !g_slots[slot].valid) { char m[64]; snprintf(m, sizeof m, "Slot %d is empty", slot + 1); set_msg(m); return false; }
    g_req_slot = slot;
    g_req_op = op_save ? 1 : 2;
    rt_ss_request = 1;
    ppc_irq_pending = 1;
    return true;
}

bool rt_savestate_slot_info(int slot, long *when) {
    if (slot < 0 || slot >= SS_SLOTS || !g_slots[slot].valid) return false;
    if (when) *when = (long)g_slots[slot].when;
    return true;
}

int rt_savestate_message(char *buf, int n) {
    if (buf && n > 0) snprintf(buf, (size_t)n, "%s", g_msg);
    return g_msg_seq;
}

static __attribute__((noinline)) void do_save(Slot *s, int slot) {
    volatile int marker = 0;
    jmp_buf jb;
    if (setjmp(jb)) {
        /* resumed by a load: memory, devices and all threads are back */
        char m[64]; snprintf(m, sizeof m, "State loaded from slot %d", g_cur_slot + 1); set_msg(m);
        rt_log(1, "savestate: loaded slot %d", g_cur_slot + 1);
        return;
    }
    if (!s->mem1) s->mem1 = malloc(MEM1_SPAN);
    if (!s->mem2) s->mem2 = malloc(MEM2_SPAN);
    if (!s->blobs) s->blobs = malloc(g_blob_total ? g_blob_total : 1);
    if (!s->ios) s->ios = malloc(rt_ios_ss_size());
    if (!s->threads) s->threads = rt_ss_threads_alloc();
    if (!s->mem1 || !s->mem2 || !s->blobs || !s->ios || !s->threads) { set_msg("Save state failed: out of memory"); return; }
    s->valid = 0;
    memcpy(s->mem1, ppc_host(rt_base, 0), MEM1_SPAN);
    memcpy(s->mem2, ppc_host(rt_base, MEM2_BASE), MEM2_SPAN);
    size_t o = 0;
    for (int i = 0; i < g_nblobs; i++) { memcpy(s->blobs + o, g_blobs[i].p, g_blobs[i].n); o += g_blobs[i].n; }
    rt_ios_ss_save(s->ios);
    int n = rt_ss_threads_save(s->threads, &jb, (uintptr_t)&marker);
    if (n <= 0) { set_msg("Save state failed (thread state)"); rt_log(0, "savestate: thread capture failed"); return; }
    s->valid = 1;
    s->when = time(NULL);
    char m[64]; snprintf(m, sizeof m, "State saved to slot %d", slot + 1); set_msg(m);
    rt_log(1, "savestate: saved slot %d (%d threads)", slot + 1, n);
}

static void do_load(Slot *s, int slot) {
    if (!s->valid) return;
    g_cur_slot = slot;
    memcpy(ppc_host(rt_base, 0), s->mem1, MEM1_SPAN);
    memcpy(ppc_host(rt_base, MEM2_BASE), s->mem2, MEM2_SPAN);
    size_t o = 0;
    for (int i = 0; i < g_nblobs; i++) { memcpy(g_blobs[i].p, s->blobs + o, g_blobs[i].n); o += g_blobs[i].n; }
    rt_ios_ss_load(s->ios);
    rt_gx_soft_ss_loaded();
    rt_ss_threads_load(s->threads);       /* does not return */
}

void rt_savestate_service(PPCContext *ctx) {
    (void)ctx;
    int op = g_req_op, slot = g_req_slot;
    rt_ss_request = 0;
    ensure_init();
    rt_gx_idle();
    if (op == 1) do_save(&g_slots[slot], slot);
    else if (op == 2) do_load(&g_slots[slot], slot);
}
