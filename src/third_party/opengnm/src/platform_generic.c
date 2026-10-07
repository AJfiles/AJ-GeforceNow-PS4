/*
 * platform_generic.c — Generic (host) platform backend for opengnm.
 *
 * Used for testing without a PS4. sceGnmGpuMode returns GNM_GPU_BASE.
 * sceGnmPlatGetBufferLabelAddress returns a malloc'd label area.
 */

#include "platform.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static GnmPlatParams s_params = {0};

GnmGpuMode PS4_SYSV_ABI sceGnmGpuMode(void) {
	return s_params.gpumode;
}

void sceGnmPlatInit(GnmPlatParams* params) {
	if (params) {
		s_params = *params;
	} else {
		memset(&s_params, 0, sizeof(s_params));
		s_params.gpumode = GNM_GPU_BASE;
	}
}

int32_t PS4_SYSV_ABI sceGnmPlatGetBufferLabelAddress(
    int32_t videohandle, uint64_t* outaddr
) {
	if (s_params.getbufferlabeladdress) {
		return s_params.getbufferlabeladdress(videohandle, outaddr);
	}

	/* Fallback: allocate a static label area (16 labels * 8 bytes) */
	static void* s_labelarea = NULL;
	if (!s_labelarea) {
		s_labelarea = calloc(16, 8);
		if (!s_labelarea) {
			return -1;
		}
	}

	(void)videohandle;
	if (outaddr) {
		*outaddr = (uint64_t)s_labelarea;
	}
	return 0;
}
