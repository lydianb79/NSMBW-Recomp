/*
 * MetalGX.h - Metal rasteriser for the GX pipeline (see runtime/src/gx_gpu.h).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Creates the Metal device objects, compiles the TEV shader and installs the
 * sink. Returns false (and the software renderer stays active) on failure. */
bool rt_metal_gx_install(void);

#ifdef __OBJC__
#import <Metal/Metal.h>
/* The device the rasteriser runs on (nil before install). */
id<MTLDevice> rt_metal_gx_device(void);
/* Full-resolution image of the last display copy made to `xfb` (internal
 * resolution above 1x), or nil: then present the XFB itself. *w / *h receive
 * the image size in native (1x) EFB pixels. */
id<MTLTexture> rt_metal_gx_frame(uint32_t xfb, uint32_t *w, uint32_t *h);
#endif
