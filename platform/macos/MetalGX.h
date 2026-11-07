/*
 * MetalGX.h - Metal rasteriser for the GX pipeline (see runtime/src/gx_gpu.h).
 */
#pragma once
#include <stdbool.h>

/* Creates the Metal device objects, compiles the TEV shader and installs the
 * sink. Returns false (and the software renderer stays active) on failure. */
bool rt_metal_gx_install(void);
