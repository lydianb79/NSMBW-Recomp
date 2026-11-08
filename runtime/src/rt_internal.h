#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "rt.h"

#define RT_MEM_SIZE      0x40000000u   /* 1 GiB host reservation */
#define RT_MEM1_SIZE     0x01800000u
#define RT_MEM2_BASE     0x10000000u   /* physical */
#define RT_MEM2_SIZE     0x04000000u

/* rt_main.c */
_Noreturn void rt_fatal(PPCContext *ctx, const char *fmt, ...);
void rt_interrupts_enabled(PPCContext *ctx);
void rt_raise_irq(uint32_t pi_cause_bit);
extern PPCContext *rt_main_ctx;
extern uint8_t *rt_base;
extern int rt_log_level;

/* ppc_runtime.c */
void rt_dispatch_init(void);
PPCFunc rt_lookup(uint32_t addr);
uint64_t rt_timebase(void);

/* memory.c */
bool rt_mem_init(void);
bool rt_image_load(const char *path);
bool rt_modules_load(const char *json_path);
void rt_boot_setup(PPCContext *ctx);
typedef struct { uint32_t id, addr, size, bss_addr, bss_size, prolog, epilog, unresolved;
                 uint32_t sec_count; uint32_t sec_addr[24], sec_size[24]; uint8_t sec_exec[24], sec_bss[24];
                 uint8_t *pristine; uint32_t pristine_lo, pristine_hi; } RtModule;
extern RtModule rt_modules[16];
extern int rt_module_count;
static inline uint32_t mem_r32(uint32_t ea) { return ld32(rt_base, ea); }
static inline void mem_w32(uint32_t ea, uint32_t v) { st32(rt_base, ea, v); }

/* mmio.c */
void rt_mmio_init(void);
extern uint32_t rt_vi_xfb_addr;      /* physical address of the top-field XFB */
extern uint32_t rt_vi_width, rt_vi_height, rt_vi_stride;
extern uint32_t rt_pi_intsr, rt_pi_intmr;
uint32_t rt_pi_cause(void);            /* current PI interrupt cause lines */
void rt_vi_retrace_irq(void);
void rt_pe_signal_finish(void);
void rt_pe_signal_token(uint16_t token, int irq);

/* ios.c */
void rt_ios_init(void);
void rt_ios_ipc_write(uint32_t reg, uint32_t v);
uint32_t rt_ios_ipc_read(uint32_t reg);
int rt_ios_poll(void);                 /* deliver due IPC replies; nonzero while some are pending */

/* gx_fifo.c */
void rt_gx_init(void);
void rt_gx_soft_init(int threads);              /* software rasteriser backend */
void rt_gx_start_async(void);            /* decode/draw on a separate GPU thread */
void rt_gx_write8(uint8_t v);
void rt_gx_write16(uint16_t v);
void rt_gx_write32(uint32_t v);
void rt_gx_copy_to_xfb(uint32_t dest, uint32_t w, uint32_t h);
extern volatile int rt_xfb_on_gpu;   /* display copies are presented from GPU memory; the guest XFB is not written */

/* video.c */
void rt_video_vi_retrace(void);

/* hle.c */
void rt_hle_init(void);

/* audio.c: one mixed AX frame (interleaved stereo, unclamped) */
void rt_audio_submit(const int32_t *mix, int frames);
