/*
 * driver_orbis.c — Orbis (PS4) runtime backend for opengnm.
 *
 * On PS4, the sceGnm* functions are provided by libSceGnmDriver.sprx at
 * runtime via NID resolution. This file provides:
 *
 *   1. Firmware extern declarations for the ~85 real sceGnm* functions
 *      (draw, dispatch, shader, submit, init, compute queue, VGT, markers,
 *      EQ, misc getters, workload). These resolve to firmware implementations
 *      at link/runtime via -lSceGnmDriver NID stubs.
 *
 *   2. sceGnmDriver* forwarding wrappers — opengnm-internal packet-builder
 *      functions called by drawcommandbuffer.c. They forward to the
 *      firmware's sceGnm* functions.
 *
 *   3. Error/OK stubs — functions that are stubs on retail firmware.
 *      Return values match shadPS4's firmware emulation (the authoritative
 *      reference for retail PS4 behavior):
 *        - SDMA, Sqtt, Spm, Debugger, Resource: ORBIS_GNM_ERROR_FAILURE
 *        - Many misc/coredump/CU/mipstats: ORBIS_OK (0)
 *        - Razor captures: ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL
 *        - DriverInternalRetrieveGnmInterface*: 0x80000000
 *        - GetDbgGcHandle: -1
 *        - GetProtectionFaultTimeStamp: 0
 *        - LogicalCuMaskToPhysicalCuMask: passthrough
 *
 *   4. 11 validate stubs — RE-3 confirmed all return 0 on FW 9.00.
 *
 * Reference: tools/gnm_driver_fw900_analysis.md,
 *            tools/gnm_sdma_debugprof_re_analysis.md,
 *            shadPS4 src/core/libraries/gnmdriver/gnmdriver.cpp
 */

#include "gnmdriver.h"
#include "gnm_error.h"
#include "gnm_shader.h"
#include "platform.h"
#include "pm4/sid.h"

#include "u/utility.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef __has_include
#define __has_include(_file) 0
#endif

#if __has_include(<orbis/libkernel.h>)
#include <orbis/libkernel.h>
#define OPENGNM_ORBIS_HAS_LIBKERNEL 1
#else
#define OPENGNM_ORBIS_HAS_LIBKERNEL 0
#endif

static uint32_t drawflagsraw(SceGnmDrawFlags flags) {
	uint32_t raw = 0;
	memcpy(&raw, &flags, sizeof(raw));
	return raw;
}

static uint32_t drawinitiator(SceGnmDrawFlags flags, uint32_t source_select) {
	if (sceGnmGpuMode() == GNM_GPU_NEO) {
		source_select |= drawflagsraw(flags) & 0xE0000000u;
	}
	return source_select;
}

static int g_submit_log_budget = 64;

/* Submit log helpers are only used when OPENGNM_ENABLE_SUBMIT_WRAPPERS is ON,
 * but resolvegnmdriversym also uses them for diagnostics. Mark unused to
 * avoid -Wunused-function warnings when wrappers are disabled. */
__attribute__((unused))
static void submitlog(const char* message) {
	if (g_submit_log_budget <= 0) {
		return;
	}
	g_submit_log_budget -= 1;

	FILE* file = fopen("/data/eden_ps4_runtime.log", "a");
	if (!file) {
		return;
	}
	fputs("opengnm_submit: ", file);
	fputs(message, file);
	fputc('\n', file);
	fclose(file);
}

__attribute__((unused))
static void submitlogu(const char* label, uint64_t value) {
	if (g_submit_log_budget <= 0) {
		return;
	}
	char message[160];
	snprintf(message, sizeof(message), "%s=%llu", label,
	         (unsigned long long)value);
	submitlog(message);
}

__attribute__((unused))
static void submitlogi(const char* label, int32_t value) {
	if (g_submit_log_budget <= 0) {
		return;
	}
	char message[160];
	snprintf(message, sizeof(message), "%s=%d", label, value);
	submitlog(message);
}

#if OPENGNM_ORBIS_HAS_LIBKERNEL
__attribute__((unused))
static void* resolvegnmdriversym(const char* name) {
	OrbisKernelModule modules[128];
	size_t available = 0;
	int result = sceKernelGetModuleList(
	    modules, uasize(modules), &available
	);
	if (result != 0) {
		submitlogi("module list failed", result);
		return NULL;
	}

	size_t count = available;
	if (count > uasize(modules)) {
		count = uasize(modules);
	}

	/* First pass: look for a GnmDriver module (real firmware).
	 * If a GnmDriver module is found but doesn't export the requested
	 * symbol (e.g. firmware version mismatch), fall through to the
	 * second pass instead of returning NULL immediately. */
	for (size_t i = 0; i < count; i += 1) {
		OrbisKernelModuleInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(modules[i], &info) != 0) {
			continue;
		}
		if (!strstr(info.name, "GnmDriver")) {
			continue;
		}

		void* symbol = NULL;
		result = sceKernelDlsym((int)modules[i], name, &symbol);
		if (result == 0 && symbol) {
			return symbol;
		}
		/* GnmDriver module found but symbol not in it — try other modules */
		submitlogi("dlsym failed in GnmDriver module", result);
		break;
	}

	/* Second pass: no GnmDriver module found, or GnmDriver didn't have
	   the symbol (e.g. HLE emulator, partial firmware). Try dlsym on
	   every module — the HLE layer may expose the symbol through any
	   module handle. */
	for (size_t i = 0; i < count; i += 1) {
		void* symbol = NULL;
		result = sceKernelDlsym((int)modules[i], name, &symbol);
		if (result == 0 && symbol) {
			return symbol;
		}
	}

	submitlog("GnmDriver module missing");
	return NULL;
}
#else
__attribute__((unused))
static void* resolvegnmdriversym(const char* name) {
	(void)name;
	submitlog("libkernel unavailable");
	return NULL;
}
#endif

/*
 * Runtime HLE detection. Returns true if running under an HLE emulator
 * (e.g. shadPS4) rather than real PS4 hardware. On real hardware, a
 * libSceGnmDriver module is loaded; on HLE, it is not. This determines
 * whether embedded shader wrappers should emit PM4 locally (real HW,
 * avoiding the FW 9.00 shader-set crash) or forward to the HLE-exposed
 * sceGnmSetEmbedded*Shader entry points (which provide valid shader
 * binaries that the HLE layer can resolve).
 */
#if OPENGNM_ORBIS_HAS_LIBKERNEL
static volatile bool s_is_hle_checked = false;
static bool s_is_hle = false;

static bool opengnm_is_hle_runtime(void) {
	if (s_is_hle_checked) {
		return s_is_hle;
	}

	bool detected = false;  /* default: real hardware */
	OrbisKernelModule modules[128];
	size_t available = 0;
	int result = sceKernelGetModuleList(
	    modules, uasize(modules), &available
	);
	if (result == 0 && available > 0) {
		size_t count = available;
		if (count > uasize(modules)) {
			count = uasize(modules);
		}

		bool found_gnmdriver = false;
		for (size_t i = 0; i < count; i += 1) {
			OrbisKernelModuleInfo info;
			memset(&info, 0, sizeof(info));
			info.size = sizeof(info);
			if (sceKernelGetModuleInfo(modules[i], &info) != 0) {
				continue;
			}
			if (strstr(info.name, "GnmDriver")) {
				found_gnmdriver = true;
				break;
			}
		}
		/* No GnmDriver module (and modules were enumerable) → HLE.
		 * If enumeration returned 0 modules, we can't tell — default
		 * to real HW to avoid emitting PM4 with firmware-embedded
		 * shader addresses on a misdetected HLE setup. */
		if (!found_gnmdriver) {
			detected = true;
		}
	}
	/* else: enumeration failed or returned 0 modules — assume real HW. */

	/* Publish the result: set s_is_hle BEFORE the flag so a concurrent
	 * thread that observes s_is_hle_checked==true also sees the correct
	 * s_is_hle value. The result is deterministic (module list doesn't
	 * change), so concurrent computation yields the same value. */
	s_is_hle = detected;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	s_is_hle_checked = true;
	return detected;
}
#else
static bool opengnm_is_hle_runtime(void) {
	/* Without libkernel we cannot enumerate modules. Assume real HW
	 * so we use the safe local PM4 emission path (avoids the FW 9.00
	 * crash). This branch is unreachable on shadPS4, which provides
	 * libkernel, so HLE emulators never hit this default. */
	return false;
}
#endif

typedef int32_t (*SubmitCommandBuffersFn)(
    uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes
);
typedef int32_t (*SubmitAndFlipCommandBuffersFn)(
    uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes, uint32_t vo_handle, uint32_t buf_idx,
    uint32_t flip_mode, int64_t flip_arg
);
typedef int32_t (*SubmitCommandBuffersForWorkloadFn)(
    uint32_t workload, uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes
);
typedef int32_t (*SubmitAndFlipCommandBuffersForWorkloadFn)(
    uint32_t workload, uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes, uint32_t vo_handle, uint32_t buf_idx,
    uint32_t flip_mode, int64_t flip_arg
);
typedef int32_t (*SubmitDoneFn)(void);

/*
 * Firmware shader-set helpers are not safe in every caller context. Eden's PS4
 * renderer reaches sceGnmDriverSetVsShader from its GPU work pump and FW 9.00
 * crashes inside sceGnmSetVsShader before returning. Emit the same PM4 locally
 * for driver-level shader binds and keep firmware forwarding for submission and
 * other true runtime operations.
 */
static uint32_t setcontextregisterrange(
    uint32_t* cmd, uint32_t regaddr, const uint32_t* regvalues,
    uint32_t numvalues
) {
	if (regaddr < SI_CONTEXT_REG_OFFSET ||
	    numvalues > (SI_CONTEXT_REG_END - regaddr) / sizeof(uint32_t)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Invalid context register 0x%x used",
		    regaddr
		);
		return 0;
	}

	cmd[0] = PKT3(PKT3_SET_CONTEXT_REG, numvalues, 0);
	cmd[1] = (regaddr - SI_CONTEXT_REG_OFFSET) >> 2;
	for (uint32_t i = 0; i < numvalues; i += 1) {
		cmd[2 + i] = regvalues[i];
	}

	return 2 + numvalues;
}

static inline uint32_t setcontextregister(
    uint32_t* cmd, uint32_t regaddr, uint32_t regvalue
) {
	return setcontextregisterrange(cmd, regaddr, &regvalue, 1);
}

static uint32_t setpersistentregisterrange(
    uint32_t* cmd, uint32_t regaddr, const uint32_t* regvalues,
    uint32_t numvalues
) {
	if (regaddr < SI_SH_REG_OFFSET ||
	    numvalues > (SI_SH_REG_END - regaddr) / sizeof(uint32_t)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Invalid persistent register 0x%x used",
		    regaddr
		);
		return 0;
	}

	cmd[0] = PKT3(PKT3_SET_SH_REG, numvalues, 0);
	cmd[1] = (regaddr - SI_SH_REG_OFFSET) >> 2;
	for (uint32_t i = 0; i < numvalues; i += 1) {
		cmd[2 + i] = regvalues[i];
	}

	return 2 + numvalues;
}

static inline uint32_t* setpshresources(
    uint32_t* cmd, const GnmPsStageRegisters* psregs
) {
	const uint32_t pgmps[2] = {psregs->spishaderpgmlops, 0};
	cmd += setpersistentregisterrange(
	    cmd, R_00B020_SPI_SHADER_PGM_LO_PS, pgmps, uasize(pgmps)
	);

	const uint32_t pgmrsrc[2] = {
	    psregs->spishaderpgmrsrc1ps, psregs->spishaderpgmrsrc2ps};
	cmd += setpersistentregisterrange(
	    cmd, R_00B028_SPI_SHADER_PGM_RSRC1_PS, pgmrsrc, uasize(pgmrsrc)
	);

	const uint32_t shfmt[2] = {
	    psregs->spishaderzformat, psregs->spishadercolformat};
	cmd += setcontextregisterrange(
	    cmd, R_028710_SPI_SHADER_Z_FORMAT, shfmt, uasize(shfmt)
	);

	const uint32_t shinput[2] = {
	    psregs->spipsinputena, psregs->spipsinputaddr};
	cmd += setcontextregisterrange(
	    cmd, R_0286CC_SPI_PS_INPUT_ENA, shinput, uasize(shinput)
	);

	cmd += setcontextregister(
	    cmd, R_0286D8_SPI_PS_IN_CONTROL, psregs->spipsincontrol
	);
	cmd += setcontextregister(
	    cmd, R_0286E0_SPI_BARYC_CNTL, psregs->spibaryccntl
	);
	cmd += setcontextregister(
	    cmd, R_02880C_DB_SHADER_CONTROL, psregs->dbshadercontrol
	);
	cmd += setcontextregister(
	    cmd, R_02823C_CB_SHADER_MASK, psregs->cbshadermask
	);

	return cmd;
}

static int32_t setpsshaderlocal(
    uint32_t* cmd, uint32_t numdwords, const void* psregs,
    uint32_t maxdwords, bool setdefaultmask
) {
	const GnmPsStageRegisters* ppsregs = psregs;
	if (!cmd || numdwords < maxdwords) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmd;

	if (psregs) {
		if (ppsregs->spishaderpgmhips) {
			return GNM_ERROR_CMD_FAILED;
		}
		cmd = setpshresources(cmd, ppsregs);
	} else {
		const uint32_t pgmps[2] = {0};
		cmd += setpersistentregisterrange(
		    cmd, R_00B020_SPI_SHADER_PGM_LO_PS, pgmps, uasize(pgmps)
		);
		cmd += setcontextregister(cmd, R_02880C_DB_SHADER_CONTROL, 0);
		if (setdefaultmask) {
			cmd += setcontextregister(cmd, R_02823C_CB_SHADER_MASK, 0xf);
		}
	}

	const uint32_t remainingdwords = maxdwords - (cmd - startcmd);
	if (remainingdwords) {
		cmd[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmd[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

static int32_t setvsshaderlocal(
    uint32_t* cmd, uint32_t numdwords, const void* vsregs,
    uint32_t shadermodifier
) {
	const uint32_t maxdwords = 29;
	const GnmVsStageRegisters* pvsregs = vsregs;

	if (!cmd || numdwords < maxdwords || !vsregs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (shadermodifier & 0xfcfffc3f) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (pvsregs->spishaderpgmhivs) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmd;

	const uint32_t pgmvs[2] = {pvsregs->spishaderpgmlovs, 0};
	cmd += setpersistentregisterrange(
	    cmd, R_00B120_SPI_SHADER_PGM_LO_VS, pgmvs, uasize(pgmvs)
	);

	const uint32_t rsrc1 = shadermodifier == 0
	    ? pvsregs->spishaderpgmrsrc1vs
	    : ((pvsregs->spishaderpgmrsrc1vs & 0xfcfffc3f) | shadermodifier);
	const uint32_t pgmrsrc[2] = {rsrc1, pvsregs->spishaderpgmrsrc2vs};
	cmd += setpersistentregisterrange(
	    cmd, R_00B128_SPI_SHADER_PGM_RSRC1_VS, pgmrsrc, uasize(pgmrsrc)
	);

	cmd += setcontextregister(
	    cmd, R_02881C_PA_CL_VS_OUT_CNTL, pvsregs->paclvsoutcntl
	);
	cmd += setcontextregister(
	    cmd, R_0286C4_SPI_VS_OUT_CONFIG, pvsregs->spivsoutconfig
	);
	cmd += setcontextregister(
	    cmd, R_02870C_SPI_SHADER_POS_FORMAT, pvsregs->spishaderposformat
	);

	const uint32_t remainingdwords = maxdwords - (cmd - startcmd);
	if (remainingdwords) {
		cmd[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmd[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

/*
 * Embedded shader register blobs.
 * These are the register-level representations of the firmware's built-in
 * embedded shaders (fullscreen VS, dummy PS). On real hardware we emit
 * them locally via setvsshaderlocal/setpsshaderlocal instead of calling
 * the firmware sceGnmSetEmbeddedVsShader/sceGnmSetEmbeddedPsShader,
 * because the firmware shader-set helpers crash on FW 9.00 in certain
 * caller contexts (see comment above). Local PM4 emission produces
 * identical GPU state and is safe in all contexts.
 * On HLE emulators (shadPS4), these blobs are NOT used — the embedded
 * shader wrappers forward to sceGnmSetEmbedded*Shader instead, since
 * the blobs reference firmware-embedded shader addresses that don't
 * exist under HLE.
 */
static const uint8_t s_embedded_vs_fullscreen[] = {
    0xf1, 0x00, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
_Static_assert(sizeof(s_embedded_vs_fullscreen) == 0x1c, "");

static const uint8_t s_embedded_ps_dummy[] = {
    0xf0, 0x00, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
_Static_assert(sizeof(s_embedded_ps_dummy) == 0x30, "");

static const uint8_t s_embedded_ps_dummyrg32[] = {
    0xf2, 0x00, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
_Static_assert(sizeof(s_embedded_ps_dummyrg32) == 0x30, "");

/*
 * Part 1: Firmware extern declarations.
 *
 * These declare the sceGnm* functions that have real implementations in
 * the PS4 firmware (libSceGnmDriver.sprx). They are resolved at link time
 * via -lSceGnmDriver NID stubs and at runtime by the PS4 dynamic linker.
 */
/* Draw functions */
extern int32_t sceGnmDrawIndexAuto(
    uint32_t* cmd, uint32_t numdwords, uint32_t indexcount, uint32_t flags);
extern int32_t sceGnmDrawIndex(
    uint32_t* cmd, uint32_t numdwords, uint32_t indexcount,
    uintptr_t indexaddr, uint32_t flags, uint32_t type);
extern int32_t sceGnmDrawIndexIndirect(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t stage,
    uint32_t vertexoffusgpr, uint32_t instanceoffusgpr, uint32_t flags);
extern int32_t sceGnmDrawIndirect(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t stage,
    uint32_t vertexoffusgpr, uint32_t instanceoffusgpr, uint32_t flags);
extern int32_t sceGnmDrawIndexIndirectMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint32_t shader_stage, uint32_t vertex_sgpr_offset,
    uint32_t instance_sgpr_offset, uint32_t flags);
extern int32_t sceGnmDrawIndirectMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint32_t shader_stage, uint32_t vertex_sgpr_offset,
    uint32_t instance_sgpr_offset, uint32_t flags);
extern int32_t sceGnmDrawIndexIndirectCountMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint64_t count_addr, uint32_t shader_stage, uint32_t vertex_sgpr_offset,
    uint32_t instance_sgpr_offset, uint32_t flags);
extern int32_t sceGnmDrawIndexOffset(
    uint32_t* cmd, uint32_t numdwords, uint32_t index_offset,
    uint32_t index_count, uint32_t flags);

/* Shader set functions */
extern int32_t sceGnmSetVsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* vsregs,
    uint32_t shadermodifier);
extern int32_t sceGnmSetPsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* psregs);
extern int32_t sceGnmSetPsShader350(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* psregs);
extern int32_t sceGnmSetEmbeddedVsShader(
    uint32_t* cmd, uint32_t numdwords, uint32_t shaderid,
    uint32_t shadermodifier);
extern int32_t sceGnmSetEmbeddedPsShader(
    uint32_t* cmd, uint32_t numdwords, uint32_t shaderid,
    uint32_t shadermodifier);
extern int32_t sceGnmSetCsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* csregs);
extern int32_t sceGnmSetCsShaderWithModifier(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* csregs,
    uint32_t modifier);
extern int32_t sceGnmSetEsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* esregs,
    uint32_t shadermodifier);
extern int32_t sceGnmSetGsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* gsregs);
extern int32_t sceGnmSetHsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* hsregs,
    uint32_t param4);
extern int32_t sceGnmSetLsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* lsregs,
    uint32_t shadermodifier);

/* Shader update functions */
extern int32_t sceGnmUpdateGsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* gsregs);
extern int32_t sceGnmUpdateHsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* hsregs,
    uint32_t lshsconfig);
extern int32_t sceGnmUpdatePsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* psregs);
extern int32_t sceGnmUpdatePsShader350(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* psregs);
extern int32_t sceGnmUpdateVsShader(
    uint32_t* cmd, uint32_t numdwords, const uint32_t* vsregs,
    uint32_t shadermodifier);

/* Init / default hardware state */
extern uint32_t sceGnmDrawInitDefaultHardwareState(
    uint32_t* cmdbuf, uint32_t size);
extern uint32_t sceGnmDrawInitDefaultHardwareState175(
    uint32_t* cmdbuf, uint32_t size);
extern uint32_t sceGnmDrawInitDefaultHardwareState200(
    uint32_t* cmdbuf, uint32_t size);
extern uint32_t sceGnmDrawInitDefaultHardwareState350(
    uint32_t* cmdbuf, uint32_t size);
extern uint32_t sceGnmDispatchInitDefaultHardwareState(
    uint32_t* cmdbuf, uint32_t size);
extern uint32_t sceGnmDrawInitToDefaultContextState(
    uint32_t* cmdbuf, uint32_t size);
extern uint32_t sceGnmDrawInitToDefaultContextState400(
    uint32_t* cmdbuf, uint32_t size);
extern int32_t sceGnmDrawInitToDefaultContextStateInternalCommand(
    uint32_t* cmdbuf, uint32_t size);
extern int32_t sceGnmDrawInitToDefaultContextStateInternalSize(void);

/* Dispatch functions */
extern int32_t sceGnmDispatchDirect(
    uint32_t* cmdbuf, uint32_t size, uint32_t threadsx, uint32_t threadsy,
    uint32_t threadsz, uint32_t flags);
extern int32_t sceGnmDispatchIndirect(
    uint32_t* cmdbuf, uint32_t size, uint32_t dataoffset, uint32_t flags);
extern int32_t sceGnmDispatchIndirectOnMec(
    uint32_t* cmdbuf, uint32_t size, uintptr_t args, uint32_t modifier);

/* Submit functions */
extern int sceGnmAreSubmitsAllowed(void);

__attribute__((unused))
static int32_t validatesubmitargs(
    uint32_t count, void* const dcbgpuaddrs[], uint32_t* dcbsizes
) {
	if (count == 0 || count > 64 || !dcbgpuaddrs || !dcbsizes) {
		submitlog("invalid submit arrays");
		return GNM_ERROR_CMD_FAILED;
	}

	for (uint32_t i = 0; i < count; i += 1) {
		const uintptr_t addr = (uintptr_t)dcbgpuaddrs[i];
		const uint32_t size = dcbsizes[i];
		if (!addr || size == 0 || (size & 3) != 0 ||
		    size > GNM_INDIRECT_BUFFER_MAX_BYTESIZE) {
			submitlogu("invalid submit index", i);
			submitlogu("invalid submit addr", addr);
			submitlogu("invalid submit size", size);
			return GNM_ERROR_CMD_FAILED;
		}
	}

	return GNM_ERROR_OK;
}

#if OPENGNM_ENABLE_SUBMIT_WRAPPERS
/* Submit wrappers with validation and logging.
 * These intercept sceGnmSubmit* calls, validate arguments, log debug info,
 * then forward to the firmware implementation via resolvegnmdriversym().
 * Enable via CMake option OPENGNM_SUBMIT_WRAPPERS=ON.
 * Disabled by default — submit calls resolve directly to libSceGnmDriver.so PLT. */
int32_t PS4_SYSV_ABI sceGnmSubmitCommandBuffers(
    uint32_t count, void* const dcbgpuaddrs[], uint32_t* dcbsizes,
    void* const ccbgpuaddrs[], uint32_t* ccbsizes
) {
	static SubmitCommandBuffersFn real_submit = NULL;
	if (!real_submit) {
		real_submit = (SubmitCommandBuffersFn)resolvegnmdriversym(
		    "sceGnmSubmitCommandBuffers"
		);
	}
	if (!real_submit) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	const int32_t validation = validatesubmitargs(
	    count, dcbgpuaddrs, dcbsizes
	);
	if (validation != GNM_ERROR_OK) {
		return validation;
	}

	submitlog("submit begin");
	submitlogu("count", count);
	submitlogu("dcb0", (uintptr_t)dcbgpuaddrs[0]);
	submitlogu("dcb0_size", dcbsizes[0]);
	const int32_t result = real_submit(
	    count, dcbgpuaddrs, dcbsizes, ccbgpuaddrs, ccbsizes
	);
	submitlogi("submit result", result);
	return result;
}

int32_t PS4_SYSV_ABI sceGnmSubmitAndFlipCommandBuffers(
    uint32_t count, void* const dcbgpuaddrs[], uint32_t* dcbsizes,
    void* const ccbgpuaddrs[], uint32_t* ccbsizes, uint32_t vohandle,
    uint32_t bufidx, uint32_t flipmode, int64_t fliparg
) {
	static SubmitAndFlipCommandBuffersFn real_submit = NULL;
	if (!real_submit) {
		real_submit = (SubmitAndFlipCommandBuffersFn)resolvegnmdriversym(
		    "sceGnmSubmitAndFlipCommandBuffers"
		);
	}
	if (!real_submit) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	const int32_t validation = validatesubmitargs(
	    count, dcbgpuaddrs, dcbsizes
	);
	if (validation != GNM_ERROR_OK) {
		return validation;
	}

	submitlog("submit_and_flip begin");
	const int32_t result = real_submit(
	    count, dcbgpuaddrs, dcbsizes, ccbgpuaddrs, ccbsizes, vohandle,
	    bufidx, flipmode, fliparg
	);
	submitlogi("submit_and_flip result", result);
	return result;
}

int32_t PS4_SYSV_ABI sceGnmSubmitCommandBuffersForWorkload(
    uint32_t workload, uint32_t count, void* const dcbgpuaddrs[],
    uint32_t* dcbsizes, void* const ccbgpuaddrs[], uint32_t* ccbsizes
) {
	static SubmitCommandBuffersForWorkloadFn real_submit = NULL;
	if (!real_submit) {
		real_submit = (SubmitCommandBuffersForWorkloadFn)resolvegnmdriversym(
		    "sceGnmSubmitCommandBuffersForWorkload"
		);
	}
	if (!real_submit) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	const int32_t validation = validatesubmitargs(
	    count, dcbgpuaddrs, dcbsizes
	);
	if (validation != GNM_ERROR_OK) {
		return validation;
	}

	submitlog("submit_workload begin");
	const int32_t result = real_submit(
	    workload, count, dcbgpuaddrs, dcbsizes, ccbgpuaddrs, ccbsizes
	);
	submitlogi("submit_workload result", result);
	return result;
}

int32_t PS4_SYSV_ABI sceGnmSubmitAndFlipCommandBuffersForWorkload(
    uint32_t workload, uint32_t count, void* const dcbgpuaddrs[],
    uint32_t* dcbsizes, void* const ccbgpuaddrs[], uint32_t* ccbsizes,
    uint32_t vohandle, uint32_t bufidx, uint32_t flipmode, int64_t fliparg
) {
	static SubmitAndFlipCommandBuffersForWorkloadFn real_submit = NULL;
	if (!real_submit) {
		real_submit = (SubmitAndFlipCommandBuffersForWorkloadFn)
		    resolvegnmdriversym(
		        "sceGnmSubmitAndFlipCommandBuffersForWorkload"
		    );
	}
	if (!real_submit) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	const int32_t validation = validatesubmitargs(
	    count, dcbgpuaddrs, dcbsizes
	);
	if (validation != GNM_ERROR_OK) {
		return validation;
	}

	submitlog("submit_flip_workload begin");
	const int32_t result = real_submit(
	    workload, count, dcbgpuaddrs, dcbsizes, ccbgpuaddrs, ccbsizes,
	    vohandle, bufidx, flipmode, fliparg
	);
	submitlogi("submit_flip_workload result", result);
	return result;
}

int32_t PS4_SYSV_ABI sceGnmSubmitDone(void) {
	static SubmitDoneFn real_submit_done = NULL;
	if (!real_submit_done) {
		real_submit_done = (SubmitDoneFn)resolvegnmdriversym(
		    "sceGnmSubmitDone"
		);
	}
	if (!real_submit_done) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	submitlog("submit_done begin");
	const int32_t result = real_submit_done();
	submitlogi("submit_done result", result);
	return result;
}

int32_t PS4_SYSV_ABI sceGnmRequestFlipAndSubmitDone(void) {
	static SubmitDoneFn real_request = NULL;
	if (!real_request) {
		real_request = (SubmitDoneFn)resolvegnmdriversym(
		    "sceGnmRequestFlipAndSubmitDone"
		);
	}
	if (!real_request) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}
	return real_request();
}

int32_t PS4_SYSV_ABI sceGnmRequestFlipAndSubmitDoneForWorkload(void) {
	static SubmitDoneFn real_request = NULL;
	if (!real_request) {
		real_request = (SubmitDoneFn)resolvegnmdriversym(
		    "sceGnmRequestFlipAndSubmitDoneForWorkload"
		);
	}
	if (!real_request) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}
	return real_request();
}
#endif  /* OPENGNM_ENABLE_SUBMIT_WRAPPERS */

/* Compute queue management */
extern int32_t sceGnmMapComputeQueue(
    uint32_t pipeid, uint32_t queueid, uintptr_t ringbaseaddr,
    uint32_t ringsizedwords, uint32_t* readptraddr);
extern int32_t sceGnmMapComputeQueueWithPriority(
    uint32_t pipeid, uint32_t queueid, uintptr_t ringbaseaddr,
    uint32_t ringsizedwords, uint32_t* readptraddr, uint32_t pipepriority);
extern int32_t sceGnmUnmapComputeQueue(uint32_t vqid);
extern void sceGnmDingDong(uint32_t gnmvqid, uint32_t nextoffsdw);
extern void sceGnmDingDongForWorkload(
    uint32_t gnmvqid, uint32_t nextoffsdw, uint64_t workloadid);
extern int32_t sceGnmComputeWaitOnAddress(
    uint32_t* cmdbuf, uint32_t size, uintptr_t addr, uint32_t mask,
    uint32_t cmpfunc, uint32_t ref);

/* VGT / wave control */
extern int32_t sceGnmResetVgtControl(uint32_t* cmdbuf, uint32_t size);
extern int32_t sceGnmSetVgtControl(
    uint32_t* cmdbuf, uint32_t size, uint32_t primgroupszminusone,
    uint32_t partialvswavemode, uint32_t wdswitchonlyoneopmode);
extern int32_t sceGnmSetGsRingSizes(void);
extern int32_t sceGnmSetWaveLimitMultiplier(void);
extern int32_t sceGnmSetWaveLimitMultipliers(void);
extern int32_t sceGnmSetSpiEnableSqCounters(void);
extern int32_t sceGnmSetSpiEnableSqCountersForUnitInstance(void);

/* Markers (real on firmware — PM4 packet builders) */
extern int32_t sceGnmInsertDingDongMarker(uint32_t* cmdbuf, uint32_t size);
extern int32_t sceGnmInsertPopMarker(uint32_t* cmdbuf, uint32_t size);
extern int32_t sceGnmInsertPushColorMarker(
    uint32_t* cmdbuf, uint32_t size, const char* marker, uint32_t color);
extern int32_t sceGnmInsertPushMarker(
    uint32_t* cmdbuf, uint32_t size, const char* marker);
extern int32_t sceGnmInsertSetMarker(
    uint32_t* cmdbuf, uint32_t size, const char* marker);
extern int32_t sceGnmInsertWaitFlipDone(
    uint32_t* cmdbuf, uint32_t size, int32_t vohandle, uint32_t bufidx);

/* Misc getters (real on firmware) */
extern uintptr_t sceGnmGetTheTessellationFactorRingBufferBaseAddress(void);
extern int32_t sceGnmGetOffChipTessellationBufferSize(void);
extern uint32_t sceGnmGetGpuCoreClockFrequency(void);
extern void sceGnmFlushGarlic(void);

/* Event queue (real on firmware) */
extern int32_t sceGnmAddEqEvent(void* eq, uint64_t id, void* udata);
extern int32_t sceGnmDeleteEqEvent(void* eq, uint64_t id);
extern int32_t sceGnmGetEqEventType(const void* ev);
extern int32_t sceGnmGetEqTimeStamp(void);

/* Workload (real on firmware) */
extern int sceGnmBeginWorkload(uint32_t workloadstream, uint64_t* workload);
extern int sceGnmEndWorkload(uint64_t workload);
extern int sceGnmCreateWorkloadStream(uint64_t param1,
                                      uint32_t* workloadstream);
/*
 * Part 2: sceGnmDriver* forwarding wrappers.
 *
 * These are opengnm-internal functions called by drawcommandbuffer.c.
 * They forward to the firmware's sceGnm* functions (declared above).
 */
int32_t sceGnmDriverDrawInitDefaultHardwareState350(
    uint32_t* cmd, uint32_t numdwords
) {
	return sceGnmDrawInitDefaultHardwareState350(cmd, numdwords);
}

int32_t sceGnmDriverDrawIndex(
    uint32_t* cmd, uint32_t numdwords, uint32_t indexcount,
    const void* indexaddr, SceGnmDrawFlags flags
) {
	/* sceGnmDrawIndex takes a `type` param (index type: 0=auto, 1=u16,
	 * 2=u32). The internal wrapper does not have it because the index
	 * type is set separately via SetIndexSize. Pass 0 (auto). */
	return sceGnmDrawIndex(
	    cmd, numdwords, indexcount, (uintptr_t)indexaddr,
	    *(uint32_t*)&flags, 0
	);
}

int32_t sceGnmDriverDrawIndexAuto(
    uint32_t* cmd, uint32_t numdwords, uint32_t indexcount,
    SceGnmDrawFlags flags
) {
	const uint32_t rawflags = drawflagsraw(flags);
	if (!cmd || numdwords != 7 || (rawflags & 0x1FFFFFFE) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	cmd[0] = PKT3(PKT3_DRAW_INDEX_AUTO, 1, flags.predication);
	cmd[1] = indexcount;
	cmd[2] = drawinitiator(
	    flags, S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX)
	);
	cmd += 3;

	cmd[0] = PKT3(PKT3_NOP, 2, 0);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;

	return GNM_ERROR_OK;
}

int32_t sceGnmDriverDrawIndexIndirect(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t stage,
    uint8_t vertexoffusgpr, uint8_t instanceoffusgpr, SceGnmDrawFlags flags
) {
	return sceGnmDrawIndexIndirect(
	    cmd, numdwords, dataoffset, stage, vertexoffusgpr,
	    instanceoffusgpr, *(uint32_t*)&flags
	);
}

int32_t sceGnmDriverDrawIndirect(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t stage,
    uint8_t vertexoffusgpr, uint8_t instanceoffusgpr, SceGnmDrawFlags flags
) {
	return sceGnmDrawIndirect(
	    cmd, numdwords, dataoffset, stage, vertexoffusgpr,
	    instanceoffusgpr, *(uint32_t*)&flags
	);
}

int32_t sceGnmDriverDrawIndexIndirectMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint32_t stage, uint8_t vertexoffusgpr, uint8_t instanceoffusgpr,
    SceGnmDrawFlags flags
) {
	return sceGnmDrawIndexIndirectMulti(
	    cmd, numdwords, dataoffset, maxcount, stage, vertexoffusgpr,
	    instanceoffusgpr, *(uint32_t*)&flags
	);
}

int32_t sceGnmDriverDrawIndirectMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint32_t stage, uint8_t vertexoffusgpr, uint8_t instanceoffusgpr,
    SceGnmDrawFlags flags
) {
	return sceGnmDrawIndirectMulti(
	    cmd, numdwords, dataoffset, maxcount, stage, vertexoffusgpr,
	    instanceoffusgpr, *(uint32_t*)&flags
	);
}

int32_t sceGnmDriverDrawIndexIndirectCountMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint64_t countaddr, uint32_t stage, uint8_t vertexoffusgpr,
    uint8_t instanceoffusgpr, SceGnmDrawFlags flags
) {
	return sceGnmDrawIndexIndirectCountMulti(
	    cmd, numdwords, dataoffset, maxcount, countaddr, stage,
	    vertexoffusgpr, instanceoffusgpr, *(uint32_t*)&flags
	);
}

int32_t sceGnmDriverSetVsShader(
    uint32_t* cmd, uint32_t numdwords, const void* vsregs,
    uint32_t shadermodifier
) {
	return setvsshaderlocal(cmd, numdwords, vsregs, shadermodifier);
}

int32_t sceGnmDriverSetPsShader(
    uint32_t* cmd, uint32_t numdwords, const void* psregs
) {
	return setpsshaderlocal(cmd, numdwords, psregs, 34, false);
}

int32_t sceGnmDriverSetPsShader350(
    uint32_t* cmd, uint32_t numdwords, const void* psregs
) {
	return setpsshaderlocal(cmd, numdwords, psregs, 40, true);
}

int32_t sceGnmDriverSetEmbeddedVsShader(
    uint32_t* cmd, uint32_t numdwords, int32_t shaderid, uint32_t shadermodifier
) {
	/* On real hardware, emit PM4 locally using the embedded shader
	 * register blob. This avoids the FW 9.00 crash in firmware
	 * shader-set helpers (see design comment at top of file) and
	 * produces identical GPU state.
	 * On HLE emulators (shadPS4), forward to the HLE-exposed
	 * sceGnmSetEmbeddedVsShader so the emulator can provide its own
	 * built-in shader binaries (the local blobs reference firmware-
	 * embedded shader addresses that don't exist under HLE). */
	if (!opengnm_is_hle_runtime()) {
		const void* shaderptr = NULL;
		switch (shaderid) {
		case GNM_EMBEDDED_VSH_FULLSCREEN:
			shaderptr = s_embedded_vs_fullscreen;
			break;
		default:
			return GNM_ERROR_INTERNAL_FAILURE;
		}
		return setvsshaderlocal(cmd, numdwords, shaderptr, shadermodifier);
	}
	/* HLE path — validate shaderid before forwarding. */
	switch (shaderid) {
	case GNM_EMBEDDED_VSH_FULLSCREEN:
		return sceGnmSetEmbeddedVsShader(cmd, numdwords, (uint32_t)shaderid, shadermodifier);
	default:
		return GNM_ERROR_INTERNAL_FAILURE;
	}
}

int32_t sceGnmDriverSetEmbeddedPsShader(
    uint32_t* cmd, uint32_t numdwords, int32_t shaderid
) {
	/* Same HLE detection rationale as embedded VS above. */
	if (!opengnm_is_hle_runtime()) {
		const void* shaderptr = NULL;
		switch (shaderid) {
		case GNM_EMBEDDED_PSH_DUMMY:
			shaderptr = s_embedded_ps_dummy;
			break;
		case GNM_EMBEDDED_PSH_DUMMY_RG32:
			shaderptr = s_embedded_ps_dummyrg32;
			break;
		default:
			return GNM_ERROR_INTERNAL_FAILURE;
		}
		return setpsshaderlocal(cmd, numdwords, shaderptr, 40, true);
	}
	switch (shaderid) {
	case GNM_EMBEDDED_PSH_DUMMY:
	case GNM_EMBEDDED_PSH_DUMMY_RG32:
		return sceGnmSetEmbeddedPsShader(cmd, numdwords, (uint32_t)shaderid, 0);
	default:
		return GNM_ERROR_INTERNAL_FAILURE;
	}
}

int32_t sceGnmDriverInsertWaitFlipDone(
    uint32_t* cmd, uint32_t numdwords, int32_t videohandle,
    uint32_t displaybufidx
) {
	return sceGnmInsertWaitFlipDone(
	    cmd, numdwords, videohandle, displaybufidx
	);
}
/*
 * Part 3: Retail firmware stubs.
 *
 * These functions are stubs on retail PS4 firmware. Return values match
 * shadPS4's emulation of the real firmware behavior.
 */
/* --- SDMA (RE-7: 8 functions, all return ORBIS_GNM_ERROR_FAILURE) --- */

int PS4_SYSV_ABI sceGnmSdmaOpen(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSdmaClose(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSdmaConstFill(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSdmaCopyLinear(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSdmaCopyTiled(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSdmaCopyWindow(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSdmaFlush(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSdmaGetMinCmdSize(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}

/* --- Resource registration (RE-9: 19 functions, all return FAILURE) --- */

int32_t PS4_SYSV_ABI sceGnmFindResourcesPublic(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmFindResources(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetResourceBaseAddressAndSizeInBytes(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetResourceName(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetResourceRegistrationBuffers(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetResourceShaderGuid(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetResourceType(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetResourceUserData(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmQueryResourceRegistrationUserMemoryRequirements(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmRegisterGdsResource(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int32_t PS4_SYSV_ABI sceGnmRegisterOwner(void* handle, const char* name) {
	(void)handle;
	(void)name;
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmRegisterOwnerForSystem(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int32_t PS4_SYSV_ABI sceGnmRegisterResource(void* res_handle,
                                            void* owner_handle,
                                            const void* addr, size_t size,
                                            const char* name, int res_type,
                                            uint64_t user_data) {
	(void)res_handle;
	(void)owner_handle;
	(void)addr;
	(void)size;
	(void)name;
	(void)res_type;
	(void)user_data;
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSetResourceRegistrationUserMemory(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSetResourceUserData(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmUnregisterAllResourcesForOwner(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmUnregisterOwnerAndResources(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmUnregisterResource(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}

/* --- Workload: DestroyWorkloadStream returns OK, rest are real --- */

int PS4_SYSV_ABI sceGnmDestroyWorkloadStream(void) {
	return 0; /* ORBIS_OK — stubbed but returns success */
}

/* --- Sqtt (RE-8: 25 functions, all return FAILURE) --- */

int PS4_SYSV_ABI sceGnmInsertThreadTraceMarker(void) {
	return 0; /* ORBIS_OK — stubbed but returns success */
}
int PS4_SYSV_ABI sceGnmSqttFini(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttFinishTrace(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetBcInfo(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetGpuClocks(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetHiWater(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetStatus(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetTraceCounter(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetTraceWptr(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetWrapCounts(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetWrapCounts2(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttGetWritebackLabels(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttInit(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSelectMode(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSelectTarget(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSelectTokens(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSetCuPerfMask(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSetDceEventWrite(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSetHiWater(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSetTraceBuffer2(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSetTraceBuffers(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSetUserData(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSetUserdataTimer(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttStartTrace(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttStopTrace(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSwitchTraceBuffer(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttSwitchTraceBuffer2(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSqttWaitForEvent(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}

/* --- Spm (RE-8: 10 functions, all return FAILURE) --- */

int PS4_SYSV_ABI sceGnmSpmEndSpm(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmInit(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmInit2(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmSetDelay(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmSetMuxRam(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmSetMuxRam2(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmSetSelectCounter(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmSetSpmSelects(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmSetSpmSelects2(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSpmStartSpm(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}

/* --- Debugger (RE-8: all return FAILURE except DebugHardwareStatus) --- */

int PS4_SYSV_ABI sceGnmDebugHardwareStatus(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmDebugModuleReset(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebugReset(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerGetAddressWatch(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerHaltWavefront(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerReadGds(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerReadSqIndirectRegister(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerResumeWavefront(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerResumeWavefrontCreation(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerSetAddressWatch(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerWriteGds(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDebuggerWriteSqIndirectRegister(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetDbgGcHandle(void) {
	return -1;
}

/* --- Razor GPU profiler/debugger exports --- */

int PS4_SYSV_ABI sceRazorCaptureCommandBuffersOnlyImmediate(void) {
	return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL;
}
int PS4_SYSV_ABI sceRazorCaptureCommandBuffersOnlySinceLastFlip(void) {
	return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL;
}
int PS4_SYSV_ABI sceRazorCaptureImmediate(void) {
	return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL;
}
int PS4_SYSV_ABI sceRazorCaptureSinceLastFlip(void) {
	return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL;
}
bool PS4_SYSV_ABI sceRazorIsLoaded(void) {
	return false;
}

/* --- Markers: InsertSetColorMarker returns OK --- */

int PS4_SYSV_ABI sceGnmInsertSetColorMarker(void) {
	return 0; /* ORBIS_OK */
}

/* --- Coredump / misc (mixed: some OK, some FAILURE) --- */

int PS4_SYSV_ABI sceGnmGetCoredumpAddress(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetCoredumpMode(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetCoredumpProtectionFaultTimestamp(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetDebugTimestamp(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetGpuBlockStatus(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetGpuInfoStatus(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetLastWaitedAddress(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetNumTcaUnits(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetOwnerName(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmGetPhysicalCounterFromVirtualized(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
uint32_t PS4_SYSV_ABI sceGnmGetProtectionFaultTimeStamp(void) {
	return 0;
}
int PS4_SYSV_ABI sceGnmGetShaderProgramBaseAddress(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmGetShaderStatus(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmIsCoredumpValid(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmRaiseUserExceptionEvent(void) {
	return 0; /* ORBIS_OK */
}

/* --- Driver internal --- */

bool PS4_SYSV_ABI sceGnmDriverCaptureInProgress(void) {
	return false;
}
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterface(void) {
	return 0x80000000;
}
uint32_t PS4_SYSV_ABI
sceGnmDriverInternalRetrieveGnmInterfaceForGpuDebugger(void) {
	return 0x80000000;
}
uint32_t PS4_SYSV_ABI
sceGnmDriverInternalRetrieveGnmInterfaceForGpuException(void) {
	return 0x80000000;
}
uint32_t PS4_SYSV_ABI
sceGnmDriverInternalRetrieveGnmInterfaceForHDRScopes(void) {
	return 0x80000000;
}
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterfaceForReplay(void) {
	return 0x80000000;
}
uint32_t PS4_SYSV_ABI
sceGnmDriverInternalRetrieveGnmInterfaceForResourceRegistration(void) {
	return 0x80000000;
}
uint32_t PS4_SYSV_ABI
sceGnmDriverInternalRetrieveGnmInterfaceForValidation(void) {
	return 0x80000000;
}
int PS4_SYSV_ABI sceGnmDriverInternalVirtualQuery(void) {
	return 0; /* ORBIS_OK */
}
bool PS4_SYSV_ABI sceGnmDriverTraceInProgress(void) {
	return false;
}
int PS4_SYSV_ABI sceGnmDriverTriggerCapture(void) {
	return ORBIS_GNM_ERROR_CAPTURE_RAZOR_NOT_LOADED;
}
void PS4_SYSV_ABI sceGnmRegisterGnmLiveCallbackConfig(void) {
	/* no-op on retail */
}

/* --- Logical CU / GPU PA --- */

void PS4_SYSV_ABI sceGnmGpuPaDebugEnter(void) {
	/* no-op on retail */
}
void PS4_SYSV_ABI sceGnmGpuPaDebugLeave(void) {
	/* no-op on retail */
}
bool PS4_SYSV_ABI sceGnmIsUserPaEnabled(void) {
	return false;
}
int PS4_SYSV_ABI sceGnmLogicalCuIndexToPhysicalCuIndex(void) {
	return 0; /* ORBIS_OK */
}
int32_t PS4_SYSV_ABI sceGnmLogicalCuMaskToPhysicalCuMask(
    int64_t a, int32_t logicalcumask
) {
	(void)a;
	return logicalcumask; /* passthrough */
}
int PS4_SYSV_ABI sceGnmLogicalTcaUnitToPhysical(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmPaDisableFlipCallbacks(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmPaEnableFlipCallbacks(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmPaHeartbeat(void) {
	return 0; /* ORBIS_OK */
}

/* --- Mip stats --- */

int PS4_SYSV_ABI sceGnmDisableMipStatsReport(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmRequestMipStatsReportAndReset(void) {
	return 0; /* ORBIS_OK */
}
int PS4_SYSV_ABI sceGnmSetupMipStatsReport(void) {
	return 0; /* ORBIS_OK */
}

/* --- Draw functions with void params (stubs on retail) --- */

int PS4_SYSV_ABI sceGnmDrawIndexMultiInstanced(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDrawIndirectCountMulti(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmDrawOpaqueAuto(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmComputeWaitSemaphore(void) {
	return ORBIS_GNM_ERROR_FAILURE;
}

/* --- Unnamed NID-only exports (39 functions, all return OK) --- */

int PS4_SYSV_ABI Func_063D065A2D6359C3(void) { return 0; }
int PS4_SYSV_ABI Func_0CABACAFB258429D(void) { return 0; }
int PS4_SYSV_ABI Func_150CF336FC2E99A3(void) { return 0; }
int PS4_SYSV_ABI Func_17CA687F9EE52D49(void) { return 0; }
int PS4_SYSV_ABI Func_1870B89F759C6B45(void) { return 0; }
int PS4_SYSV_ABI Func_26F9029EF68A955E(void) { return 0; }
int PS4_SYSV_ABI Func_301E3DBBAB092DB0(void) { return 0; }
int PS4_SYSV_ABI Func_30BAFE172AF17FEF(void) { return 0; }
int PS4_SYSV_ABI Func_3E6A3E8203D95317(void) { return 0; }
int PS4_SYSV_ABI Func_40FEEF0C6534C434(void) { return 0; }
int PS4_SYSV_ABI Func_416B9079DE4CBACE(void) { return 0; }
int PS4_SYSV_ABI Func_4774D83BB4DDBF9A(void) { return 0; }
int PS4_SYSV_ABI Func_50678F1CCEEB9A00(void) { return 0; }
int PS4_SYSV_ABI Func_54A2EC5FA4C62413(void) { return 0; }
int PS4_SYSV_ABI Func_5A9C52C83138AE6B(void) { return 0; }
int PS4_SYSV_ABI Func_5D22193A31EA1142(void) { return 0; }
int PS4_SYSV_ABI Func_725A36DEBB60948D(void) { return 0; }
int PS4_SYSV_ABI Func_8021A502FA61B9BB(void) { return 0; }
int PS4_SYSV_ABI Func_9D002FE0FA40F0E6(void) { return 0; }
int PS4_SYSV_ABI Func_9D297F36A7028B71(void) { return 0; }
int PS4_SYSV_ABI Func_A2D7EC7A7BCF79B3(void) { return 0; }
int PS4_SYSV_ABI Func_AA12A3CB8990854A(void) { return 0; }
int PS4_SYSV_ABI Func_ADC8DDC005020BC6(void) { return 0; }
int PS4_SYSV_ABI Func_B0A8688B679CB42D(void) { return 0; }
int PS4_SYSV_ABI Func_B489020B5157A5FF(void) { return 0; }
int PS4_SYSV_ABI Func_BADE7B4C199140DD(void) { return 0; }
int PS4_SYSV_ABI Func_C4C328B7CF3B4171(void) { return 0; }
int PS4_SYSV_ABI Func_D1511B9DCFFB3DD9(void) { return 0; }
int PS4_SYSV_ABI Func_D53446649B02E58E(void) { return 0; }
int PS4_SYSV_ABI Func_D8B6E8E28E1EF0A3(void) { return 0; }
int PS4_SYSV_ABI Func_D93D733A19DD7454(void) { return 0; }
int PS4_SYSV_ABI Func_DE995443BC2A8317(void) { return 0; }
int PS4_SYSV_ABI Func_DF6E9528150C23FF(void) { return 0; }
int PS4_SYSV_ABI Func_ECB4C6BA41FE3350(void) { return 0; }
int PS4_SYSV_ABI Func_1C43886B16EE5530(void) { return 0; }
int PS4_SYSV_ABI Func_81037019ECCD0E01(void) { return 0; }
int PS4_SYSV_ABI Func_BFB41C057478F0BF(void) { return 0; }
int PS4_SYSV_ABI Func_E51D44DB8151238C(void) { return 0; }
int PS4_SYSV_ABI Func_F916890425496553(void) { return 0; }
/* Part 4: Validate stubs - 11 functions (RE-3: all return 0 on FW 9.00). */
int32_t PS4_SYSV_ABI sceGnmValidateCommandBuffers(void) {
	return 0;
}
int PS4_SYSV_ABI sceGnmValidateDisableDiagnostics(void) {
	return 0;
}
int PS4_SYSV_ABI sceGnmValidateDisableDiagnostics2(void) {
	return 0;
}
int32_t PS4_SYSV_ABI sceGnmValidateDispatchCommandBuffers(void) {
	return 0;
}
int32_t PS4_SYSV_ABI sceGnmValidateDrawCommandBuffers(void) {
	return 0;
}
int PS4_SYSV_ABI sceGnmValidateGetDiagnosticInfo(void) {
	return 0;
}
int PS4_SYSV_ABI sceGnmValidateGetDiagnostics(void) {
	return 0;
}
int PS4_SYSV_ABI sceGnmValidateGetVersion(void) {
	return 0;
}
bool PS4_SYSV_ABI sceGnmValidateOnSubmitEnabled(void) {
	return false;
}
int PS4_SYSV_ABI sceGnmValidateResetState(void) {
	return 0;
}
int PS4_SYSV_ABI sceGnmValidationRegisterMemoryCheckCallback(void) {
	return 0;
}
