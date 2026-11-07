/*
 * hle_dsp.c - DSP task manager.
 *
 * Audio libraries (AX) upload their microcode as a DSPTaskInfo and wait for
 * the DSP to boot it and call back. There is no DSP here: a task is reported
 * as booted straight away by calling its init callback, as the SDK's DSP
 * interrupt handler would once the microcode says hello. Mixing itself is a
 * job for an AX HLE (roadmap); until then the game runs silently.
 *
 * DSPTaskInfo: +0 state, +4 priority, +8 flags, ..., +0x28 init_cb,
 *              +0x2C res_cb, +0x30 done_cb, +0x34 req_cb
 */
#include "../rt_internal.h"
#include "addrs_smnp01.h"

enum { DSP_TASK_STATE_INIT = 0, DSP_TASK_STATE_RUN = 1 };

PPC_HLE(HLE_DSPAddTask) {
    uint32_t task = ctx->r[3];
    rt_stats.hle_hits++;
    st32(base, task + 0x00, DSP_TASK_STATE_RUN);
    st32(base, task + 0x08, 1);                       /* DSP_TASK_FLAG_ATTACHED */
    uint32_t init_cb = ld32(base, task + 0x28);
    rt_log(2, "dsp: task %08X added, init callback %08X", task, init_cb);
    if (init_cb) {
        uint32_t lr = ctx->lr;
        uint32_t saved[4] = { ctx->r[3], ctx->r[4], ctx->r[30], ctx->r[31] };
        ctx->r[3] = task;
        ppc_call(ctx, base, init_cb);
        ctx->r[30] = saved[2];
        ctx->r[31] = saved[3];
        ctx->lr = lr;
        (void)saved;
    }
    ctx->r[3] = task;
}
