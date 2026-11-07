/*
 * hle_os.c - high-level replacements for the parts of the Revolution SDK OS
 * that cannot work under static recompilation as-is:
 *
 *  - OSLink / OSUnlink: modules are pre-linked at fixed addresses by
 *    tools/image.py; Link() just restores the module image, fixes up the
 *    header the way the SDK would, and chains it into the module list.
 *
 *  - OSSaveContext / OSLoadContext: guest threads. Every guest OSThread runs
 *    on its own host thread with its own PPCContext; only the holder of a
 *    global run baton executes guest code (the Wii has one core, so this is
 *    exactly the original semantics). OSLoadContext hands the baton to the
 *    thread owning the target context, starting it on first use.
 *
 *  - Interrupt return: rt_irq_deliver() saves state into the current
 *    OSContext and calls the guest external-interrupt handler; its final
 *    OSLoadContext of that same context longjmps back to the delivery point.
 */
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "../rt_internal.h"
#include "addrs_smnp01.h"


/* ------------------------------------------------------------ OSContext I/O */
#define OS_CONTEXT_STATE_EXC 0x0002   /* context holds an interrupted state */

enum { CTX_GPR = 0x000, CTX_CR = 0x080, CTX_LR = 0x084, CTX_CTR = 0x088, CTX_XER = 0x08C,
       CTX_FPR = 0x090, CTX_FPSCR = 0x194, CTX_SRR0 = 0x198, CTX_SRR1 = 0x19C,
       CTX_MODE = 0x1A0, CTX_STATE = 0x1A2, CTX_GQR = 0x1A4, CTX_PSF = 0x1C8 };

static void ctx_store(PPCContext *c, uint32_t a, uint32_t resume_pc) {
    for (int i = 0; i < 32; i++) st32(rt_base, a + CTX_GPR + 4 * i, c->r[i]);
    st32(rt_base, a + CTX_CR, ppc_get_cr(c));
    st32(rt_base, a + CTX_LR, c->lr);
    st32(rt_base, a + CTX_CTR, c->ctr);
    st32(rt_base, a + CTX_XER, ppc_get_xer(c));
    for (int i = 0; i < 32; i++) {
        st64(rt_base, a + CTX_FPR + 8 * i, c->f[i].u);
        PPCFReg p; p.d = c->ps1[i];
        st64(rt_base, a + CTX_PSF + 8 * i, p.u);
    }
    st32(rt_base, a + CTX_FPSCR, c->fpscr);
    st32(rt_base, a + CTX_SRR0, resume_pc);
    st32(rt_base, a + CTX_SRR1, c->msr);
    for (int i = 0; i < 8; i++) st32(rt_base, a + CTX_GQR + 4 * i, c->gqr[i]);
}

static void ctx_load(PPCContext *c, uint32_t a) {
    for (int i = 0; i < 32; i++) c->r[i] = ld32(rt_base, a + CTX_GPR + 4 * i);
    ppc_set_cr(c, ld32(rt_base, a + CTX_CR), 0xFF);
    c->lr = ld32(rt_base, a + CTX_LR);
    c->ctr = ld32(rt_base, a + CTX_CTR);
    ppc_set_xer(c, ld32(rt_base, a + CTX_XER));
    for (int i = 0; i < 32; i++) {
        c->f[i].u = ld64(rt_base, a + CTX_FPR + 8 * i);
        PPCFReg p; p.u = ld64(rt_base, a + CTX_PSF + 8 * i);
        c->ps1[i] = p.d;
    }
    c->fpscr = ld32(rt_base, a + CTX_FPSCR);
    c->msr = ld32(rt_base, a + CTX_SRR1) | 0x2000;   /* keep FP enabled */
    for (int i = 0; i < 8; i++) c->gqr[i] = ld32(rt_base, a + CTX_GQR + 4 * i);
}

/* ------------------------------------------------------------ guest threads */
typedef struct RtThread {
    uint32_t ctx_addr;          /* guest OSContext (== OSThread) address */
    uint32_t entry, exit_lr;
    PPCContext ctx;
    pthread_t host;
    pthread_cond_t cond;
    int started, dead;
    jmp_buf *irq_jmp;           /* innermost active interrupt delivery */
    uint32_t irq_ctx;
    jmp_buf *sel_jmp;           /* innermost active SelectThread call (idle-switch unwinding) */
    jmp_buf *irq_sel_jmp;       /* SelectThread call that was active when the current interrupt was taken */
    struct RtThread *next;
} RtThread;

static pthread_mutex_t g_baton = PTHREAD_MUTEX_INITIALIZER;
static RtThread *g_threads, *g_running;
static __thread RtThread *t_self;
static RtThread g_main_thread;

static RtThread *thread_by_ctx(uint32_t a) {
    for (RtThread *t = g_threads; t; t = t->next) if (t->ctx_addr == a && !t->dead) return t;
    return NULL;
}

static void baton_pass_and_wait(RtThread *self, RtThread *to) {
    pthread_mutex_lock(&g_baton);
    g_running = to;
    pthread_cond_signal(&to->cond);
    while (g_running != self) pthread_cond_wait(&self->cond, &g_baton);
    pthread_mutex_unlock(&g_baton);
}

static void *thread_main(void *arg) {
    RtThread *t = arg;
    t_self = t;
    pthread_mutex_lock(&g_baton);
    while (g_running != t) pthread_cond_wait(&t->cond, &g_baton);
    pthread_mutex_unlock(&g_baton);
    rt_log(2, "thread %08X: start entry %08X sp %08X", t->ctx_addr, t->entry, t->ctx.r[1]);
    t->ctx.lr = t->exit_lr;
    ppc_call(&t->ctx, rt_base, t->entry);
    /* entry returned: continue at the context's LR (OSExitThread) with r3 = result */
    ppc_call(&t->ctx, rt_base, t->exit_lr);
    rt_fatal(&t->ctx, "thread %08X returned from OSExitThread", t->ctx_addr);
}

static RtThread *thread_create(uint32_t ctx_addr) {
    RtThread *t = calloc(1, sizeof *t);
    t->ctx_addr = ctx_addr;
    pthread_cond_init(&t->cond, NULL);
    ctx_load(&t->ctx, ctx_addr);
    t->entry = ld32(rt_base, ctx_addr + CTX_SRR0);
    t->exit_lr = ld32(rt_base, ctx_addr + CTX_LR);
    t->started = 1;
    t->next = g_threads;
    g_threads = t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);   /* recompiled code recurses deeply */
    pthread_create(&t->host, &at, thread_main, t);
    pthread_attr_destroy(&at);
    return t;
}

void rt_threads_init_main(PPCContext *ctx) {
    (void)ctx;                       /* g_main_thread.ctx already holds the boot state */
    pthread_cond_init(&g_main_thread.cond, NULL);
    g_main_thread.started = 1;
    g_threads = &g_main_thread;
    g_running = &g_main_thread;
    t_self = &g_main_thread;
}
PPCContext *rt_thread_main_ctx(void) { return &g_main_thread.ctx; }

/* The host thread that calls rt_run() becomes the boot guest thread. (The
 * app initialises on the UI thread but runs the game on its own thread.) */
void rt_threads_bind_current(void) {
    t_self = &g_main_thread;
    pthread_mutex_lock(&g_baton);
    g_running = &g_main_thread;
    pthread_mutex_unlock(&g_baton);
}

void rt_dump_threads(void) {
    for (RtThread *t = g_threads; t; t = t->next) {
        rt_log(0, "guest thread %08X%s%s entry %08X", t->ctx_addr, t == g_running ? " [running]" : "",
               t == &g_main_thread ? " (boot)" : "", t->entry);
        rt_dump_state(&t->ctx);
    }
}

PPC_HLE(HLE_OSSaveContext) {
    uint32_t a = ctx->r[3];
    RtThread *self = t_self;
    /* Bind the context to this host thread only when it is the OS's current
     * context (SelectThread saving the running thread). The SDK also saves
     * into scratch contexts (exceptions, FPU), which must not re-tag us. */
    if (self && self->ctx_addr != a && !thread_by_ctx(a) && a == ld32(rt_base, OS_CURRENT_CONTEXT))
        self->ctx_addr = a;
    rt_log(3, "OSSaveContext(%08X) lr %08X sp %08X cur %08X in_irq %d", a, ctx->lr, ctx->r[1],
           ld32(rt_base, OS_CURRENT_CONTEXT), self && self->irq_jmp ? 1 : 0);
    ctx->r[3] = 1;                 /* value seen if this context is ever resumed via rfi */
    ctx_store(ctx, a, ctx->lr);
    ctx->r[3] = 0;
    rt_stats.hle_hits++;
}

PPC_HLE(HLE_OSLoadContext) {
    uint32_t a = ctx->r[3];
    RtThread *self = t_self;
    rt_stats.hle_hits++;
    rt_log(3, "OSLoadContext(%08X) from thread %08X, srr0 %08X sp %08X", a, self ? self->ctx_addr : 0,
           ld32(rt_base, a + CTX_SRR0), ld32(rt_base, a + 4));
    if (self && self->irq_jmp && a == self->irq_ctx) {
        ctx_load(ctx, a);          /* return from interrupt */
        st16(rt_base, a + CTX_STATE, ld16(rt_base, a + CTX_STATE) & ~OS_CONTEXT_STATE_EXC);
        longjmp(*self->irq_jmp, 1);
    }
    RtThread *t = thread_by_ctx(a);
    if (t == self) { ctx_load(ctx, a); return; }
    if (!t) {
        if (!ld32(rt_base, a + CTX_SRR0) || !ld32(rt_base, a + 4))
            rt_fatal(ctx, "OSLoadContext(%08X): not a known thread and not a startable context", a);
        t = thread_create(a);
    }
    baton_pass_and_wait(self, t);
    /* Resumed. If we were switched away while handling an interrupt, the
     * scheduler did not save us (SelectThread skips OSSaveContext for an
     * exception context): our context still holds the interrupted state, and
     * resuming means returning to the interrupted code, as the CPU's rfi would. */
    if (self->irq_jmp && self->irq_ctx == self->ctx_addr &&
        (ld16(rt_base, self->ctx_addr + CTX_STATE) & OS_CONTEXT_STATE_EXC)) {
        ctx_load(ctx, self->ctx_addr);
        st16(rt_base, self->ctx_addr + CTX_STATE, ld16(rt_base, self->ctx_addr + CTX_STATE) & ~OS_CONTEXT_STATE_EXC);
        longjmp(*self->irq_jmp, 1);
    }
    /* Resumed while still inside an interrupt that was taken in the scheduler's
     * idle loop (current context = IdleContext, not ours): on hardware the
     * handler's OSLoadContext never returned, and we resume exactly where our
     * own context was saved - SelectThread returning to its caller. Unwind the
     * stale interrupt frames back to that SelectThread call. */
    if (self->irq_sel_jmp && self->irq_jmp && self->irq_ctx != self->ctx_addr)
        longjmp(*self->irq_sel_jmp, 1);
    /* otherwise behave as if OSSaveContext returned 1 -> caller just returns */
    ctx->r[3] = 0;
}

/* SelectThread wrapper: gives the idle-switch case above a place to unwind to.
 * When unwound, SelectThread "returns" with its caller's registers intact, as
 * after OSSaveContext() returned 1 on the real scheduler. */
void __imp_f_801B4FE0(PPCContext *ctx, uint8_t *base);
PPC_HLE(HLE_SelectThread) {
    RtThread *self = t_self;
    if (!self) { __imp_f_801B4FE0(ctx, base); return; }
    PPCContext entry = *ctx;
    jmp_buf jb, *prev_sel = self->sel_jmp, *prev_irq = self->irq_jmp, *prev_irq_sel = self->irq_sel_jmp;
    uint32_t prev_irq_ctx = self->irq_ctx;
    self->sel_jmp = &jb;
    if (setjmp(jb) == 0) {
        __imp_f_801B4FE0(ctx, base);
    } else {
        rt_log(3, "SelectThread: thread %08X resumed after an idle-time switch", self->ctx_addr);
        *ctx = entry;
        ctx->r[3] = 0;
        self->irq_jmp = prev_irq;
        self->irq_ctx = prev_irq_ctx;
        self->irq_sel_jmp = prev_irq_sel;
    }
    self->sel_jmp = prev_sel;
}

/* ------------------------------------------------------------ interrupts */
volatile int ppc_irq_pending;
static volatile int g_vi_due;

void rt_irq_vi_tick(void) { g_vi_due = 1; ppc_irq_pending = 1; }

void rt_raise_irq(uint32_t bit) {
    rt_pi_intsr |= bit;
    ppc_irq_pending = 1;
}


static void rt_irq_deliver(PPCContext *ctx, uint8_t *base) {
    RtThread *self = t_self;
    uint32_t osctx = ld32(base, OS_CURRENT_CONTEXT);
    uint32_t handler = ld32(base, OS_EXCEPTION_TABLE + 4 * OS_EXCEPTION_EXTERNAL);
    if (!osctx || !handler || !self) return;
    PPCContext saved = *ctx;
    jmp_buf jb, *prev_jb = self->irq_jmp, *prev_sel = self->irq_sel_jmp;
    uint32_t prev_ctx = self->irq_ctx;
    self->irq_sel_jmp = self->sel_jmp;
    ctx_store(ctx, osctx, ctx->lr);
    st16(base, osctx + CTX_STATE, ld16(base, osctx + CTX_STATE) | OS_CONTEXT_STATE_EXC);
    rt_log(3, "irq: cause %08X intmr %08X -> handler %08X ctx %08X sp %08X", rt_pi_cause(), rt_pi_intmr, handler, osctx, ctx->r[1]);
    self->irq_jmp = &jb;
    self->irq_ctx = osctx;
    if (setjmp(jb) == 0) {
        ctx->msr &= ~0x8000u;          /* EE off inside the handler */
        ctx->r[3] = OS_EXCEPTION_EXTERNAL;
        ctx->r[4] = osctx;
        /* like the hardware vector, run on the interrupted stack: the guest
         * handler saves r1 into the context itself (no red zone in the EABI) */
        ppc_call(ctx, base, handler);
        /* handler returned without OSLoadContext: restore ourselves */
        *ctx = saved;
    } else {
        /* came back through OSLoadContext(osctx): registers reloaded from the context;
         * keep host-only state */
        ctx->thread = saved.thread;
    }
    self->irq_jmp = prev_jb;
    self->irq_ctx = prev_ctx;
    self->irq_sel_jmp = prev_sel;
}

void rt_ai_dma_apply(void);
void ppc_irq_check(PPCContext *ctx, uint8_t *base) {
    if (g_vi_due) { g_vi_due = 0; rt_vi_retrace_irq(); }
    rt_ai_dma_apply();
    int more = rt_ios_poll();
    uint32_t cause = rt_pi_cause() & rt_pi_intmr;
    if (!(ctx->msr & 0x8000)) { ppc_irq_pending = more || cause != 0; return; }
    ppc_irq_pending = more;
    if (cause) rt_irq_deliver(ctx, base);
    if (g_vi_due || rt_ios_poll() || (rt_pi_cause() & rt_pi_intmr)) ppc_irq_pending = 1;
}

void rt_interrupts_enabled(PPCContext *ctx) {
    if (ppc_irq_pending) ppc_irq_check(ctx, rt_base);
}

/* ------------------------------------------------------------ OSLink */
static RtModule *module_by_id(uint32_t id) {
    for (int i = 0; i < rt_module_count; i++) if (rt_modules[i].id == id) return &rt_modules[i];
    return NULL;
}

PPC_HLE(HLE_OSLink_Link) {
    uint32_t mod = ctx->r[3];
    uint32_t id = ld32(base, mod);
    RtModule *m = module_by_id(id);
    rt_stats.hle_hits++;
    if (!m) { rt_log(0, "OSLink: module id %u was not pre-linked (add its .rel to image.py)", id); ctx->r[3] = 0; return; }
    if (ld32(base, mod + 0x1C) >= 1 && ld32(base, mod + 0x10) >= 0x80000000u) {
        rt_log(1, "OSLink: module %u already linked", id); ctx->r[3] = 1; return;
    }
    /* pristine image + cleared bss, exactly what Link() would have produced */
    if (m->pristine) memcpy(ppc_host(base, m->pristine_lo), m->pristine, m->pristine_hi - m->pristine_lo);
    if (m->bss_size) memset(ppc_host(base, m->bss_addr), 0, m->bss_size);
    /* header fix-ups: offsets -> absolute addresses */
    uint32_t nsec = ld32(base, mod + 0x0C);
    uint32_t sec_info = mod + ld32(base, mod + 0x10);
    st32(base, mod + 0x10, sec_info);
    if (ld32(base, mod + 0x14)) st32(base, mod + 0x14, mod + ld32(base, mod + 0x14));
    st32(base, mod + 0x24, mod + ld32(base, mod + 0x24));   /* relOffset */
    st32(base, mod + 0x28, mod + ld32(base, mod + 0x28));   /* impOffset */
    for (uint32_t i = 0; i < nsec && i < m->sec_count; i++) {
        uint32_t e = sec_info + i * 8;
        if (!ld32(base, e + 4) || !m->sec_addr[i]) continue;
        st32(base, e, m->sec_addr[i] | (m->sec_exec[i] ? 1u : 0u));
    }
    st32(base, mod + 0x34, m->prolog);
    st32(base, mod + 0x38, m->epilog);
    st32(base, mod + 0x3C, m->unresolved);
    /* append to the OS module queue */
    uint32_t tail = ld32(base, OS_MODULE_LIST_TAIL);
    st32(base, mod + 4, 0);
    st32(base, mod + 8, tail);
    if (tail) st32(base, tail + 4, mod); else st32(base, OS_MODULE_LIST_HEAD, mod);
    st32(base, OS_MODULE_LIST_TAIL, mod);
    rt_log(1, "OSLink: module %u -> prolog %08X", id, m->prolog);
    ctx->r[3] = 1;
}

PPC_HLE(HLE_OSUnlink) {
    uint32_t mod = ctx->r[3];
    uint32_t next = ld32(base, mod + 4), prev = ld32(base, mod + 8);
    if (next) st32(base, next + 8, prev); else st32(base, OS_MODULE_LIST_TAIL, prev);
    if (prev) st32(base, prev + 4, next); else st32(base, OS_MODULE_LIST_HEAD, next);
    rt_stats.hle_hits++;
    ctx->r[3] = 1;
}

void rt_hle_init(void) {}
