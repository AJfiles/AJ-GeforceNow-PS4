/*
 * platform_orbis.c — Orbis (PS4) platform backend for opengnm.
 *
 * Provides sceGnmGpuMode (queries sceKernelIsNeoMode) and
 * sceGnmPlatGetBufferLabelAddress (queries sceVideoOutGetBufferLabelAddress).
 *
 * The firmware functions are declared as externs here to avoid depending
 * on the incomplete OpenOrbis headers.
 */

#include "platform.h"

#include <stdint.h>

/* Firmware externs — resolved at runtime via NID stubs */
extern int32_t sceKernelIsNeoMode(void);
extern void* sceVideoOutGetBufferLabelAddress(int32_t videohandle);

GnmGpuMode PS4_SYSV_ABI sceGnmGpuMode(void) {
	static GnmGpuMode mode = GNM_GPU_BASE;
	static int init = 0;

	if (!init) {
		switch (sceKernelIsNeoMode()) {
		case 0:
		default:
			/* mode is already GNM_GPU_BASE */
			break;
		case 1:
			mode = GNM_GPU_NEO;
			break;
		}
		init = 1;
	}

	return mode;
}

void sceGnmPlatInit(GnmPlatParams* params) {
	/* Not needed on orbis — firmware provides the real implementations */
	(void)params;
}

int32_t PS4_SYSV_ABI sceGnmPlatGetBufferLabelAddress(
    int32_t videohandle, uint64_t* outaddr
) {
	*outaddr = (uint64_t)sceVideoOutGetBufferLabelAddress(videohandle);
	return 0;
}
