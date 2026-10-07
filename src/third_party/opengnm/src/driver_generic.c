/*
 * driver_generic.c — Generic (host) backend for opengnm.
 *
 * Software implementation for testing without a PS4. Emits PM4 packets
 * directly into command buffers (per RE-1/RE-2), matching the firmware's
 * packet formats. Functions that require real hardware (submit, compute
 * queue mapping, SDMA, debugger/profiler) return error codes.
 *
 * Structure mirrors driver_orbis.c:
 *   1. sceGnmDriver* PM4 packet builder wrappers (called by drawcommandbuffer.c)
 *   2. Real sceGnm* functions (draw/dispatch/shader/submit/init/etc)
 *   3. Stub functions (SDMA/resource/Sqtt/Spm/Debugger/etc)
 *   4. Validate stubs (return 0)
 *
 * PM4 emission logic ported from freegnm's driver_generic.c, adapted to
 * opengnm's sceGnm* naming convention.
 */

#include "gnmdriver.h"
#include "gnm_error.h"
#include "gnm_shader.h"
#include "platform.h"
#include "pm4/sid.h"
#include "hwinit_sequences.h"

#include "u/utility.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* PM4 register-setting helpers (shared by all packet builders). */
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

static inline uint32_t setpersistentregister(
    uint32_t* cmd, uint32_t regaddr, uint32_t regvalue
) {
	return setpersistentregisterrange(cmd, regaddr, &regvalue, 1);
}

static uint32_t setusercfgrange(
    uint32_t* cmd, uint32_t regaddr, const uint32_t* regvalues,
    uint32_t numvalues
) {
	if (regaddr < CIK_UCONFIG_REG_OFFSET ||
	    numvalues > (CIK_UCONFIG_REG_END - regaddr) / sizeof(uint32_t)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Invalid user config register 0x%x used",
		    regaddr
		);
		return 0;
	}

	cmd[0] = PKT3(PKT3_SET_UCONFIG_REG, numvalues, 0);
	cmd[1] = (regaddr - CIK_UCONFIG_REG_OFFSET) >> 2;
	for (uint32_t i = 0; i < numvalues; i += 1) {
		cmd[2 + i] = regvalues[i];
	}

	return 2 + numvalues;
}

static inline uint32_t setusercfg(
    uint32_t* cmd, uint32_t regaddr, uint32_t regvalue
) {
	return setusercfgrange(cmd, regaddr, &regvalue, 1);
}

static const uint32_t s_indirectsgproffsets[GNM_STAGE_LS + 1] = {
    0,   /* GNM_STAGE_CS */
    0,   /* GNM_STAGE_PS */
    76,  /* GNM_STAGE_VS */
    0,   /* GNM_STAGE_GS */
    204, /* GNM_STAGE_ES */
    0,   /* GNM_STAGE_HS */
    332, /* GNM_STAGE_LS */
};

static SceGnmDrawFlags makedrawflags(uint32_t rawflags) {
	SceGnmDrawFlags flags = {0};
	memcpy(&flags, &rawflags, sizeof(flags));
	return flags;
}

static uint32_t drawflagsraw(SceGnmDrawFlags flags) {
	uint32_t rawflags = 0;
	memcpy(&rawflags, &flags, sizeof(rawflags));
	return rawflags;
}

static uint32_t drawinitiator(SceGnmDrawFlags flags, uint32_t source_select) {
	if (sceGnmGpuMode() == GNM_GPU_NEO) {
		source_select |= drawflagsraw(flags) & 0xE0000000u;
	}
	return source_select;
}
/*
 * Part 1: sceGnmDriver* PM4 packet builder wrappers.
 *
 * These emit PM4 packets directly into the command buffer, matching
 * the firmware's packet formats (RE-1, RE-2).
 */
int32_t sceGnmDriverDrawInitDefaultHardwareState350(
    uint32_t* cmd, uint32_t numdwords
) {
	const uint32_t maxdwords = HW_INIT_PACKET_SIZE;
	if (!cmd || numdwords < maxdwords) {
		return 0;
	}

	uint32_t* startcmd = cmd;

	/* ClearContextState preamble (12 dwords) */
	memcpy(cmd, CLEAR_STATE_SEQUENCE, CLEAR_STATE_SEQUENCE_LEN * 4);
	cmd += CLEAR_STATE_SEQUENCE_LEN;

	/* Select the firmware-captured init sequence based on GPU mode */
	const uint32_t* seq;
	size_t seqlen;
	const GnmGpuMode gpumode = sceGnmGpuMode();
	if (gpumode == GNM_GPU_NEO) {
		seq = INIT_SEQUENCE_350_NEO;
		seqlen = INIT_SEQUENCE_350_NEO_LEN;
	} else {
		seq = INIT_SEQUENCE_350;
		seqlen = INIT_SEQUENCE_350_LEN;
	}

	/* Copy init sequence, skipping the first 2 dwords (CLEAR_STATE
	 * already emitted by ClearContextState above) */
	const size_t copylen = seqlen - 2;
	memcpy(cmd, &seq[2], copylen * 4);
	cmd += copylen;

	/* Pad remaining space with NOP */
	const uint32_t remaining = maxdwords - (uint32_t)(cmd - startcmd);
	if (remaining >= 2) {
		cmd[0] = PKT3(PKT3_NOP, remaining - 2, 0);
		for (uint32_t i = 1; i < remaining; i += 1) {
			cmd[i] = 0;
		}
	}

	return maxdwords;
}

int32_t sceGnmDriverDrawIndex(
    uint32_t* cmd, uint32_t numdwords, uint32_t indexcount,
    const void* indexaddr, SceGnmDrawFlags flags
) {
	const uint32_t rawflags = drawflagsraw(flags);
	if (!cmd || numdwords != 10 || !indexaddr || (uintptr_t)indexaddr & 1 ||
	    (rawflags & 0x1FFFFFFE) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	cmd[0] = PKT3(PKT3_DRAW_INDEX_2, 4, flags.predication);
	cmd[1] = indexcount;
	cmd[2] = (uintptr_t)indexaddr & 0xfffffffe;
	cmd[3] = ((uintptr_t)indexaddr >> 32) & 0xffffffff;
	cmd[4] = indexcount;
	cmd[5] = drawinitiator(flags, 0);
	cmd += 6;

	cmd[0] = PKT3(PKT3_NOP, 2, 0);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;

	return GNM_ERROR_OK;
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
	const uint32_t maxdwords = 9;
	const uint32_t rawflags = drawflagsraw(flags);

	if (!cmd || numdwords != maxdwords || vertexoffusgpr > 15 ||
	    instanceoffusgpr > 15 || stage > GNM_STAGE_LS ||
	    (rawflags & 0x1FFFFFFE) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	const uint32_t sgproff = s_indirectsgproffsets[stage];

	cmd[0] = PKT3(PKT3_DRAW_INDEX_INDIRECT, 3, flags.predication);
	cmd[1] = dataoffset;
	cmd[2] = (vertexoffusgpr ? sgproff + vertexoffusgpr : 0) & 0xffff;
	cmd[3] = (instanceoffusgpr ? sgproff + instanceoffusgpr : 0) & 0xffff;
	cmd[4] = drawinitiator(flags, 0);
	cmd += 5;

	cmd[0] = PKT3(PKT3_NOP, 2, 0);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;

	return GNM_ERROR_OK;
}

int32_t sceGnmDriverDrawIndirect(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t stage,
    uint8_t vertexoffusgpr, uint8_t instanceoffusgpr, SceGnmDrawFlags flags
) {
	const uint32_t maxdwords = 9;
	const uint32_t rawflags = drawflagsraw(flags);

	if (!cmd || numdwords != maxdwords || vertexoffusgpr > 15 ||
	    instanceoffusgpr > 15 || stage > GNM_STAGE_LS ||
	    (rawflags & 0x1FFFFFFE) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	const uint32_t sgproff = s_indirectsgproffsets[stage];

	cmd[0] = PKT3(PKT3_DRAW_INDIRECT, 3, flags.predication);
	cmd[1] = dataoffset;
	cmd[2] = (vertexoffusgpr ? sgproff + vertexoffusgpr : 0) & 0xffff;
	cmd[3] = (instanceoffusgpr ? sgproff + instanceoffusgpr : 0) & 0xffff;
	cmd[4] = drawinitiator(
	    flags, S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX)
	);
	cmd += 5;

	cmd[0] = PKT3(PKT3_NOP, 2, 0);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;

	return GNM_ERROR_OK;
}

int32_t sceGnmDriverDrawIndexIndirectMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint32_t stage, uint8_t vertexoffusgpr, uint8_t instanceoffusgpr,
    SceGnmDrawFlags flags
) {
	const uint32_t maxdwords = 11;
	const uint32_t rawflags = drawflagsraw(flags);
	if (!cmd || numdwords != maxdwords || vertexoffusgpr > 15 ||
	    instanceoffusgpr > 15 ||
	    (stage != GNM_STAGE_VS && stage != GNM_STAGE_ES &&
	     stage != GNM_STAGE_LS) ||
	    (rawflags & 0x1FFFFFFE) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	const uint32_t sgproff = s_indirectsgproffsets[stage];

	cmd[0] = PKT3(PKT3_DRAW_INDEX_INDIRECT_MULTI, 6, flags.predication);
	cmd[1] = dataoffset;
	cmd[2] = (vertexoffusgpr ? (sgproff + vertexoffusgpr) & 0xffff : 0);
	cmd[3] = (instanceoffusgpr ? (sgproff + instanceoffusgpr) & 0xffff : 0);
	cmd[4] = maxcount;
	cmd[5] = 0x14; /* sizeof(DrawIndexedIndirectArgs) */
	cmd[6] = drawinitiator(flags, 0);
	cmd += 7;

	cmd[0] = PKT3(PKT3_NOP, 3, 0);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;

	return GNM_ERROR_OK;
}

int32_t sceGnmDriverDrawIndirectMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint32_t stage, uint8_t vertexoffusgpr, uint8_t instanceoffusgpr,
    SceGnmDrawFlags flags
) {
	const uint32_t maxdwords = 11;
	const uint32_t rawflags = drawflagsraw(flags);
	if (!cmd || numdwords != maxdwords || vertexoffusgpr > 15 ||
	    instanceoffusgpr > 15 || stage > GNM_STAGE_LS ||
	    (rawflags & 0x1FFFFFFE) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	const uint32_t sgproff = s_indirectsgproffsets[stage];

	cmd[0] = PKT3(PKT3_DRAW_INDIRECT_MULTI, 6, flags.predication);
	cmd[1] = dataoffset;
	cmd[2] = (vertexoffusgpr ? (sgproff + vertexoffusgpr) & 0xffff : 0);
	cmd[3] = (instanceoffusgpr ? (sgproff + instanceoffusgpr) & 0xffff : 0);
	cmd[4] = maxcount;
	cmd[5] = 0x10; /* sizeof(DrawIndirectArgs) */
	cmd[6] = drawinitiator(
	    flags, S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX)
	);
	cmd += 7;

	cmd[0] = PKT3(PKT3_NOP, 3, 0);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;

	return GNM_ERROR_OK;
}

int32_t sceGnmDriverDrawIndexIndirectCountMulti(
    uint32_t* cmd, uint32_t numdwords, uint32_t dataoffset, uint32_t maxcount,
    uint64_t countaddr, uint32_t stage, uint8_t vertexoffusgpr,
    uint8_t instanceoffusgpr, SceGnmDrawFlags flags
) {
	const uint32_t maxdwords = 16;
	const uint32_t rawflags = drawflagsraw(flags);
	if (!cmd || numdwords != maxdwords || vertexoffusgpr > 15 ||
	    instanceoffusgpr > 15 ||
	    (stage != GNM_STAGE_VS && stage != GNM_STAGE_ES &&
	     stage != GNM_STAGE_LS) ||
	    (rawflags & 0x1FFFFFFE) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	const uint32_t sgproff = s_indirectsgproffsets[stage];

	cmd[0] = PKT3(PKT3_NOP, 2, 0);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd += 3;

	cmd[0] = PKT3(PKT3_DRAW_INDEX_INDIRECT_COUNT_MULTI, 9, flags.predication);
	cmd[1] = dataoffset;
	cmd[2] = (vertexoffusgpr ? (sgproff + vertexoffusgpr) & 0xffff : 0);
	cmd[3] = (instanceoffusgpr ? (sgproff + instanceoffusgpr) & 0xffff : 0);
	cmd[4] = (countaddr != 0 ? 1u : 0u) << 30;
	cmd[5] = maxcount;
	cmd[6] = (uint32_t)(countaddr & 0xffffffff);
	cmd[7] = (uint32_t)(countaddr >> 32);
	cmd[8] = 0x14; /* sizeof(DrawIndexedIndirectArgs) */
	cmd[9] = drawinitiator(flags, 0);
	cmd += 10;

	cmd[0] = PKT3(PKT3_NOP, 2, 0);
	cmd[1] = 0;
	cmd[2] = 0;

	return GNM_ERROR_OK;
}

/* --- PS shader helpers --- */

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

int32_t sceGnmDriverSetPsShader(
    uint32_t* cmd, uint32_t numdwords, const void* psregs
) {
	const uint32_t maxdwords = 40;
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

int32_t sceGnmDriverSetPsShader350(
    uint32_t* cmd, uint32_t numdwords, const void* psregs
) {
	const uint32_t maxdwords = 40;
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
		cmd += setcontextregister(cmd, R_02823C_CB_SHADER_MASK, 0xf);
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

int32_t sceGnmDriverSetVsShader(
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

/* --- Embedded shaders --- */

static const uint8_t s_embedded_vs_fullscreen[] = {
    0xf1, 0x00, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
_Static_assert(sizeof(s_embedded_vs_fullscreen) == 0x1c, "");

int32_t sceGnmDriverSetEmbeddedVsShader(
    uint32_t* cmd, uint32_t numdwords, int32_t shaderid, uint32_t shadermodifier
) {
	const void* shaderptr = 0;
	switch (shaderid) {
	case GNM_EMBEDDED_VSH_FULLSCREEN:
		shaderptr = s_embedded_vs_fullscreen;
		break;
	default:
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	return sceGnmDriverSetVsShader(cmd, numdwords, shaderptr, shadermodifier);
}

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

int32_t sceGnmDriverSetEmbeddedPsShader(
    uint32_t* cmd, uint32_t numdwords, int32_t shaderid
) {
	const void* shaderptr = 0;
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

	return sceGnmDriverSetPsShader350(cmd, numdwords, shaderptr);
}

int32_t sceGnmDriverInsertWaitFlipDone(
    uint32_t* cmd, uint32_t numdwords, int32_t videohandle,
    uint32_t displaybufidx
) {
	const uint32_t maxdwords = 7;
	if (!cmd || numdwords != maxdwords) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint64_t labeladdr = 0;
	sceGnmPlatGetBufferLabelAddress(videohandle, &labeladdr);
	labeladdr += displaybufidx * 8;
	const uint32_t highaddr = (uint32_t)(labeladdr >> 32);
	if (highaddr > 0xffff) {
		return GNM_ERROR_CMD_FAILED;
	}

	cmd[0] = PKT3(PKT3_WAIT_REG_MEM, 5, 0);
	cmd[1] = WAIT_REG_MEM_EQUAL | WAIT_REG_MEM_MEM_SPACE(1);
	cmd[2] = labeladdr & 0xffffffff;
	cmd[3] = highaddr;
	cmd[4] = 0;
	cmd[5] = 0xffffffff;
	cmd[6] = 10; /* poll interval */

	return GNM_ERROR_OK;
}
/*
 * Part 2: Real sceGnm* functions.
 *
 * Draw/Dispatch/Shader-set/Shader-update/Init functions emit PM4
 * directly (delegating to the sceGnmDriver* wrappers above).
 * Submit/Compute-queue/VGT/Markers/EQ/Workload return error or OK
 * (no real hardware on generic platform).
 */
/* --- Draw functions --- */

int32_t PS4_SYSV_ABI sceGnmDrawIndex(uint32_t* cmdbuf, uint32_t size,
                                     uint32_t index_count, uintptr_t index_addr,
                                     uint32_t flags, uint32_t type) {
	(void)type;
	SceGnmDrawFlags f = makedrawflags(flags);
	return sceGnmDriverDrawIndex(cmdbuf, size, index_count, (void*)index_addr, f);
}

int32_t PS4_SYSV_ABI sceGnmDrawIndexAuto(uint32_t* cmdbuf, uint32_t size,
                                         uint32_t index_count, uint32_t flags) {
	SceGnmDrawFlags f = makedrawflags(flags);
	return sceGnmDriverDrawIndexAuto(cmdbuf, size, index_count, f);
}

int32_t PS4_SYSV_ABI sceGnmDrawIndexIndirect(uint32_t* cmdbuf, uint32_t size,
                                             uint32_t data_offset,
                                             uint32_t shader_stage,
                                             uint32_t vertex_sgpr_offset,
                                             uint32_t instance_sgpr_offset,
                                             uint32_t flags) {
	if (vertex_sgpr_offset >= 0x10 || instance_sgpr_offset >= 0x10) {
		return GNM_ERROR_CMD_FAILED;
	}
	SceGnmDrawFlags f = makedrawflags(flags);
	return sceGnmDriverDrawIndexIndirect(
	    cmdbuf, size, data_offset, shader_stage,
	    (uint8_t)vertex_sgpr_offset, (uint8_t)instance_sgpr_offset, f
	);
}

int32_t PS4_SYSV_ABI sceGnmDrawIndirect(uint32_t* cmdbuf, uint32_t size,
                                        uint32_t data_offset,
                                        uint32_t shader_stage,
                                        uint32_t vertex_sgpr_offset,
                                        uint32_t instance_sgpr_offset,
                                        uint32_t flags) {
	if (vertex_sgpr_offset >= 0x10 || instance_sgpr_offset >= 0x10) {
		return GNM_ERROR_CMD_FAILED;
	}
	SceGnmDrawFlags f = makedrawflags(flags);
	return sceGnmDriverDrawIndirect(
	    cmdbuf, size, data_offset, shader_stage,
	    (uint8_t)vertex_sgpr_offset, (uint8_t)instance_sgpr_offset, f
	);
}

int PS4_SYSV_ABI sceGnmDrawIndexIndirectMulti(uint32_t* cmdbuf, uint32_t size,
                                              uint32_t data_offset,
                                              uint32_t max_count,
                                              uint32_t shader_stage,
                                              uint32_t vertex_sgpr_offset,
                                              uint32_t instance_sgpr_offset,
                                              uint32_t flags) {
	if (vertex_sgpr_offset >= 0x10 || instance_sgpr_offset >= 0x10) {
		return GNM_ERROR_CMD_FAILED;
	}
	SceGnmDrawFlags f = makedrawflags(flags);
	return sceGnmDriverDrawIndexIndirectMulti(
	    cmdbuf, size, data_offset, max_count, shader_stage,
	    (uint8_t)vertex_sgpr_offset, (uint8_t)instance_sgpr_offset, f
	);
}

int PS4_SYSV_ABI sceGnmDrawIndirectMulti(uint32_t* cmdbuf, uint32_t size,
                                             uint32_t data_offset,
                                             uint32_t max_count,
                                             uint32_t shader_stage,
                                             uint32_t vertex_sgpr_offset,
                                             uint32_t instance_sgpr_offset,
                                             uint32_t flags) {
	if (vertex_sgpr_offset >= 0x10 || instance_sgpr_offset >= 0x10) {
		return GNM_ERROR_CMD_FAILED;
	}
	SceGnmDrawFlags f = makedrawflags(flags);
	return sceGnmDriverDrawIndirectMulti(
	    cmdbuf, size, data_offset, max_count, shader_stage,
	    (uint8_t)vertex_sgpr_offset, (uint8_t)instance_sgpr_offset, f
	);
}

int32_t PS4_SYSV_ABI sceGnmDrawIndexIndirectCountMulti(
    uint32_t* cmdbuf, uint32_t size, uint32_t data_offset, uint32_t max_count,
    uint64_t count_addr, uint32_t shader_stage, uint32_t vertex_sgpr_offset,
    uint32_t instance_sgpr_offset, uint32_t flags) {
	if (vertex_sgpr_offset >= 0x10 || instance_sgpr_offset >= 0x10) {
		return GNM_ERROR_CMD_FAILED;
	}
	SceGnmDrawFlags f = makedrawflags(flags);
	return sceGnmDriverDrawIndexIndirectCountMulti(
	    cmdbuf, size, data_offset, max_count, count_addr, shader_stage,
	    (uint8_t)vertex_sgpr_offset, (uint8_t)instance_sgpr_offset, f
	);
}

int32_t PS4_SYSV_ABI sceGnmDrawIndexOffset(uint32_t* cmdbuf, uint32_t size,
                                           uint32_t index_offset,
                                           uint32_t index_count,
                                           uint32_t flags) {
	/* DrawIndexOffset uses DRAW_INDEX_OFFSET_2 packet (9 dwords total).
	 * flags packs SceGnmDrawFlags: bit 0 = predication, bits 29-31 = RT slice offset. */
	const uint32_t maxdwords = 9;
	if (!cmdbuf || size != maxdwords) {
		return GNM_ERROR_CMD_FAILED;
	}
	SceGnmDrawFlags f = makedrawflags(flags);
	const uint32_t predicate = f.predication;
	cmdbuf[0] = PKT3(PKT3_DRAW_INDEX_OFFSET_2, 4, predicate);
	cmdbuf[1] = index_count;
	cmdbuf[2] = index_offset;
	cmdbuf[3] = index_count;
	cmdbuf[4] = drawinitiator(f, 0);
	cmdbuf[5] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[6] = 0;
	cmdbuf[7] = 0;
	cmdbuf[8] = 0;
	return GNM_ERROR_OK;
}

/* --- Shader set functions --- */

int32_t PS4_SYSV_ABI sceGnmSetVsShader(uint32_t* cmdbuf, uint32_t size,
                                       const uint32_t* vs_regs,
                                       uint32_t shader_modifier) {
	return sceGnmDriverSetVsShader(cmdbuf, size, vs_regs, shader_modifier);
}

int32_t PS4_SYSV_ABI sceGnmSetPsShader(uint32_t* cmdbuf, uint32_t size,
                                       const uint32_t* ps_regs) {
	return sceGnmDriverSetPsShader(cmdbuf, size, ps_regs);
}

int32_t PS4_SYSV_ABI sceGnmSetPsShader350(uint32_t* cmdbuf, uint32_t size,
                                          const uint32_t* ps_regs) {
	return sceGnmDriverSetPsShader350(cmdbuf, size, ps_regs);
}

int32_t PS4_SYSV_ABI sceGnmSetEmbeddedVsShader(uint32_t* cmdbuf, uint32_t size,
                                               uint32_t shader_id,
                                               uint32_t modifier) {
	return sceGnmDriverSetEmbeddedVsShader(cmdbuf, size, (int32_t)shader_id, modifier);
}

int32_t PS4_SYSV_ABI sceGnmSetEmbeddedPsShader(uint32_t* cmdbuf, uint32_t size,
                                               uint32_t shader_id,
                                               uint32_t shader_modifier) {
	(void)shader_modifier;
	return sceGnmDriverSetEmbeddedPsShader(cmdbuf, size, (int32_t)shader_id);
}

int32_t PS4_SYSV_ABI sceGnmSetCsShader(uint32_t* cmdbuf, uint32_t size,
                                       const uint32_t* cs_regs) {
	return sceGnmSetCsShaderWithModifier(cmdbuf, size, cs_regs, 0);
}

int32_t PS4_SYSV_ABI sceGnmSetCsShaderWithModifier(uint32_t* cmdbuf,
                                                   uint32_t size,
                                                   const uint32_t* cs_regs,
                                                   uint32_t modifier) {
	const uint32_t maxdwords = 25;
	const GnmCsStageRegisters* regs = (const GnmCsStageRegisters*)cs_regs;
	if (!cmdbuf || size < maxdwords || !cs_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if ((modifier & 0xfffffc3f) != 0) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->computepgmhi != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmcs[2] = {regs->computepgmlo, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B830_COMPUTE_PGM_LO, pgmcs, uasize(pgmcs)
	);

	const uint32_t rsrc1 = modifier == 0
	    ? regs->computepgmrsrc1
	    : ((regs->computepgmrsrc1 & 0xfffffc3f) | modifier);
	const uint32_t pgmrsrc[2] = {rsrc1, regs->computepgmrsrc2};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B848_COMPUTE_PGM_RSRC1, pgmrsrc, uasize(pgmrsrc)
	);

	const uint32_t threads[3] = {
	    regs->computenumthreadx, regs->computenumthready,
	    regs->computenumthreadz};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B81C_COMPUTE_NUM_THREAD_X, threads, uasize(threads)
	);

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmSetEsShader(uint32_t* cmdbuf, uint32_t size,
                                       const uint32_t* es_regs,
                                       uint32_t shader_modifier) {
	const uint32_t maxdwords = 20;
	const GnmEsStageRegisters* regs = (const GnmEsStageRegisters*)es_regs;
	if (!cmdbuf || size < maxdwords || !es_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (shader_modifier & 0xfcfffc3f) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->spishaderpgmhies != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmes[2] = {regs->spishaderpgmloes, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B320_SPI_SHADER_PGM_LO_ES, pgmes, uasize(pgmes)
	);

	const uint32_t rsrc1 = shader_modifier == 0
	    ? regs->spishaderpgmrsrc1es
	    : ((regs->spishaderpgmrsrc1es & 0xfcfffc3f) | shader_modifier);
	const uint32_t pgmrsrc[2] = {rsrc1, regs->spishaderpgmrsrc2es};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B328_SPI_SHADER_PGM_RSRC1_ES, pgmrsrc, uasize(pgmrsrc)
	);

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmSetGsShader(uint32_t* cmdbuf, uint32_t size,
                                       const uint32_t* gs_regs) {
	const uint32_t maxdwords = 29;
	const GnmGsStageRegisters* regs = (const GnmGsStageRegisters*)gs_regs;
	if (!cmdbuf || size < maxdwords || !gs_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->spishaderpgmhigs != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmgs[2] = {regs->spishaderpgmlogs, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B220_SPI_SHADER_PGM_LO_GS, pgmgs, uasize(pgmgs)
	);

	const uint32_t pgmrsrc[2] = {
	    regs->spishaderpgmrsrc1gs, regs->spishaderpgmrsrc2gs};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B228_SPI_SHADER_PGM_RSRC1_GS, pgmrsrc, uasize(pgmrsrc)
	);

	cmdbuf += setcontextregister(
	    cmdbuf, R_028B94_VGT_STRMOUT_CONFIG, regs->vgtstrmoutconfig
	);
	cmdbuf += setcontextregister(
	    cmdbuf, R_028A6C_VGT_GS_OUT_PRIM_TYPE, regs->vgtgsoutprimtype
	);
	cmdbuf += setcontextregister(
	    cmdbuf, R_028B90_VGT_GS_INSTANCE_CNT, regs->vgtgsinstancecnt
	);

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmSetHsShader(uint32_t* cmdbuf, uint32_t size,
                                       const uint32_t* hs_regs,
                                       uint32_t param4) {
	const uint32_t maxdwords = 30;
	const GnmHsStageRegisters* regs = (const GnmHsStageRegisters*)hs_regs;
	if (!cmdbuf || size < maxdwords || !hs_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->spishaderpgmhihs != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmhs[2] = {regs->spishaderpgmlohs, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B420_SPI_SHADER_PGM_LO_HS, pgmhs, uasize(pgmhs)
	);

	const uint32_t pgmrsrc[2] = {
	    regs->spishaderpgmrsrc1hs, regs->spishaderpgmrsrc2hs};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B428_SPI_SHADER_PGM_RSRC1_HS, pgmrsrc, uasize(pgmrsrc)
	);

	const uint32_t tess[2] = {
	    regs->vgthosmaxtesslevel, regs->vgthosmintesslevel};
	cmdbuf += setcontextregisterrange(
	    cmdbuf, R_028A18_VGT_HOS_MAX_TESS_LEVEL, tess, uasize(tess)
	);
	cmdbuf += setcontextregister(
	    cmdbuf, R_028B6C_VGT_TF_PARAM, regs->vgttfparam
	);
	cmdbuf += setcontextregister(
	    cmdbuf, R_028B58_VGT_LS_HS_CONFIG, param4
	);

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmSetLsShader(uint32_t* cmdbuf, uint32_t size,
                                       const uint32_t* ls_regs,
                                       uint32_t shader_modifier) {
	const uint32_t maxdwords = 23;
	const GnmLsStageRegisters* regs = (const GnmLsStageRegisters*)ls_regs;
	if (!cmdbuf || size < maxdwords || !ls_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->spishaderpgmhils != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmls[2] = {regs->spishaderpgmlols, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B520_SPI_SHADER_PGM_LO_LS, pgmls, uasize(pgmls)
	);

	cmdbuf += setpersistentregister(
	    cmdbuf, R_00B524_SPI_SHADER_PGM_HI_LS, regs->spishaderpgmrsrc2ls
	);

	const uint32_t rsrc1 = shader_modifier == 0
	    ? regs->spishaderpgmrsrc1ls
	    : ((regs->spishaderpgmrsrc1ls & 0xfcfffc3f) | shader_modifier);
	cmdbuf += setpersistentregister(
	    cmdbuf, R_00B528_SPI_SHADER_PGM_RSRC1_LS, rsrc1
	);

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

/* --- Shader update functions --- */

int32_t PS4_SYSV_ABI sceGnmUpdateGsShader(uint32_t* cmdbuf, uint32_t size,
                                          const uint32_t* gs_regs) {
	/* UpdateGsShader writes PGM_LO + RSRC, then NOP-wrapped context reg updates. */
	const uint32_t maxdwords = 29;
	const GnmGsStageRegisters* regs = (const GnmGsStageRegisters*)gs_regs;
	if (!cmdbuf || size < maxdwords || !gs_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->spishaderpgmhigs != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmgs[2] = {regs->spishaderpgmlogs, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B220_SPI_SHADER_PGM_LO_GS, pgmgs, uasize(pgmgs)
	);

	const uint32_t pgmrsrc[2] = {
	    regs->spishaderpgmrsrc1gs, regs->spishaderpgmrsrc2gs};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B228_SPI_SHADER_PGM_RSRC1_GS, pgmrsrc, uasize(pgmrsrc)
	);

	/* NOP-wrapped context register updates (update path uses NOP packets
	 * with embedded register address, not SET_CONTEXT_REG) */
	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e02e5; /* VGT_STRMOUT_CONFIG tag */
	cmdbuf[2] = regs->vgtstrmoutconfig;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e029b; /* VGT_GS_OUT_PRIM_TYPE tag */
	cmdbuf[2] = regs->vgtgsoutprimtype;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e02e4; /* VGT_GS_INSTANCE_CNT tag */
	cmdbuf[2] = regs->vgtgsinstancecnt;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmUpdateHsShader(uint32_t* cmdbuf, uint32_t size,
                                      const uint32_t* hs_regs,
                                      uint32_t ls_hs_config) {
	/* UpdateHsShader writes PGM_LO + RSRC, then NOP-wrapped context reg updates. */
	const uint32_t maxdwords = 30;
	const GnmHsStageRegisters* regs = (const GnmHsStageRegisters*)hs_regs;
	if (!cmdbuf || size < maxdwords || !hs_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->spishaderpgmhihs != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmhs[2] = {regs->spishaderpgmlohs, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B420_SPI_SHADER_PGM_LO_HS, pgmhs, uasize(pgmhs)
	);

	const uint32_t pgmrsrc[2] = {
	    regs->spishaderpgmrsrc1hs, regs->spishaderpgmrsrc2hs};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B428_SPI_SHADER_PGM_RSRC1_HS, pgmrsrc, uasize(pgmrsrc)
	);

	/* NOP-wrapped context register updates */
	cmdbuf[0] = PKT3(PKT3_NOP, 3, 0);
	cmdbuf[1] = 0xc01e0286; /* VGT_HOS_MAX/MIN_TESS_LEVEL tag */
	cmdbuf[2] = regs->vgthosmaxtesslevel;
	cmdbuf[3] = regs->vgthosmintesslevel;
	cmdbuf[4] = 0;
	cmdbuf += 5;

	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e02db; /* VGT_TF_PARAM tag */
	cmdbuf[2] = regs->vgttfparam;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e02d6; /* VGT_LS_HS_CONFIG tag */
	cmdbuf[2] = ls_hs_config;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmUpdatePsShader(uint32_t* cmdbuf, uint32_t size,
                                          const uint32_t* ps_regs) {
	return sceGnmDriverSetPsShader(cmdbuf, size, ps_regs);
}

int32_t PS4_SYSV_ABI sceGnmUpdatePsShader350(uint32_t* cmdbuf, uint32_t size,
                                             const uint32_t* ps_regs) {
	return sceGnmDriverSetPsShader350(cmdbuf, size, ps_regs);
}

int32_t PS4_SYSV_ABI sceGnmUpdateVsShader(uint32_t* cmdbuf, uint32_t size,
                                          const uint32_t* vs_regs,
                                          uint32_t shader_modifier) {
	/* UpdateVsShader writes PGM_LO + RSRC, then NOP-wrapped context reg updates.
	 * Unlike SetVsShader, it uses NOP packets for context registers. */
	const uint32_t maxdwords = 29;
	const GnmVsStageRegisters* regs = (const GnmVsStageRegisters*)vs_regs;

	if (!cmdbuf || size < maxdwords || !vs_regs) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (shader_modifier & 0xfcfffc3f) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (regs->spishaderpgmhivs != 0) {
		return GNM_ERROR_CMD_FAILED;
	}

	uint32_t* startcmd = cmdbuf;

	const uint32_t pgmvs[2] = {regs->spishaderpgmlovs, 0};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B120_SPI_SHADER_PGM_LO_VS, pgmvs, uasize(pgmvs)
	);

	const uint32_t rsrc1 = shader_modifier == 0
	    ? regs->spishaderpgmrsrc1vs
	    : ((regs->spishaderpgmrsrc1vs & 0xfcfffc3f) | shader_modifier);
	const uint32_t pgmrsrc[2] = {rsrc1, regs->spishaderpgmrsrc2vs};
	cmdbuf += setpersistentregisterrange(
	    cmdbuf, R_00B128_SPI_SHADER_PGM_RSRC1_VS, pgmrsrc, uasize(pgmrsrc)
	);

	/* NOP-wrapped context register updates */
	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e0207; /* PA_CL_VS_OUT_CNTL tag */
	cmdbuf[2] = regs->paclvsoutcntl;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e01b1; /* SPI_VS_OUT_CONFIG tag */
	cmdbuf[2] = regs->spivsoutconfig;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0xc01e01c3; /* SPI_SHADER_POS_FORMAT tag */
	cmdbuf[2] = regs->spishaderposformat;
	cmdbuf[3] = 0;
	cmdbuf += 4;

	const uint32_t remainingdwords = maxdwords - (cmdbuf - startcmd);
	if (remainingdwords) {
		cmdbuf[0] = PKT3(PKT3_NOP, remainingdwords - 2, 0);
		for (uint32_t i = 1; i < remainingdwords; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return GNM_ERROR_OK;
}

/* --- Init / default hardware state --- */

/* Helper: build a 256-dword hardware init packet from a firmware sequence */
static uint32_t build_hwinit_packet(
    uint32_t* cmdbuf, uint32_t size,
    const uint32_t* seq, size_t seqlen
) {
	if (!cmdbuf || size < HW_INIT_PACKET_SIZE) {
		return 0;
	}

	uint32_t* start = cmdbuf;

	/* ClearContextState preamble (12 dwords) */
	memcpy(cmdbuf, CLEAR_STATE_SEQUENCE, CLEAR_STATE_SEQUENCE_LEN * 4);
	cmdbuf += CLEAR_STATE_SEQUENCE_LEN;

	/* Copy init sequence, skipping first 2 dwords (CLEAR_STATE
	 * already emitted by ClearContextState) */
	const size_t copylen = seqlen - 2;
	memcpy(cmdbuf, &seq[2], copylen * 4);
	cmdbuf += copylen;

	/* Pad remaining with NOP */
	const uint32_t remaining =
	    HW_INIT_PACKET_SIZE - (uint32_t)(cmdbuf - start);
	if (remaining >= 2) {
		cmdbuf[0] = PKT3(PKT3_NOP, remaining - 2, 0);
		for (uint32_t i = 1; i < remaining; i += 1) {
			cmdbuf[i] = 0;
		}
	}

	return HW_INIT_PACKET_SIZE;
}

uint32_t PS4_SYSV_ABI sceGnmDrawInitDefaultHardwareState(uint32_t* cmdbuf,
                                                         uint32_t size) {
	return build_hwinit_packet(cmdbuf, size, INIT_SEQUENCE, INIT_SEQUENCE_LEN);
}

uint32_t PS4_SYSV_ABI sceGnmDrawInitDefaultHardwareState175(
    uint32_t* cmdbuf, uint32_t size
) {
	return build_hwinit_packet(
	    cmdbuf, size, INIT_SEQUENCE_175, INIT_SEQUENCE_175_LEN
	);
}

uint32_t PS4_SYSV_ABI sceGnmDrawInitDefaultHardwareState200(
    uint32_t* cmdbuf, uint32_t size
) {
	const GnmGpuMode gpumode = sceGnmGpuMode();
	if (gpumode == GNM_GPU_NEO) {
		return build_hwinit_packet(
		    cmdbuf, size,
		    INIT_SEQUENCE_200_NEO, INIT_SEQUENCE_200_NEO_LEN
		);
	}
	return build_hwinit_packet(
	    cmdbuf, size, INIT_SEQUENCE_200, INIT_SEQUENCE_200_LEN
	);
}

uint32_t PS4_SYSV_ABI sceGnmDrawInitDefaultHardwareState350(
    uint32_t* cmdbuf, uint32_t size
) {
	return sceGnmDriverDrawInitDefaultHardwareState350(cmdbuf, size);
}

uint32_t PS4_SYSV_ABI sceGnmDispatchInitDefaultHardwareState(uint32_t* cmdbuf,
                                                             uint32_t size) {
	/* Simplified dispatch init — just clear state + NOP pad */
	if (!cmdbuf || size < 4) {
		return 0;
	}
	cmdbuf[0] = PKT3(PKT3_CLEAR_STATE, 0, 0);
	cmdbuf[1] = 0;
	cmdbuf[2] = PKT3(PKT3_NOP, 0, 0);
	cmdbuf[3] = 0;
	return 4;
}

uint32_t PS4_SYSV_ABI sceGnmDrawInitToDefaultContextState(uint32_t* cmdbuf,
                                                          uint32_t size) {
	/* FW-captured context init sequence */
	const uint32_t pktsize = 0x20;
	if (!cmdbuf || size < pktsize) {
		return 0;
	}
	const GnmGpuMode gpumode = sceGnmGpuMode();
	const uint32_t* seq;
	size_t seqlen;
	if (gpumode == GNM_GPU_NEO) {
		seq = CTX_INIT_SEQUENCE_NEO;
		seqlen = CTX_INIT_SEQUENCE_NEO_LEN;
	} else {
		seq = CTX_INIT_SEQUENCE;
		seqlen = CTX_INIT_SEQUENCE_LEN;
	}
	memcpy(cmdbuf, seq, seqlen * 4);
	/* Pad remaining with NOPs */
	for (size_t i = seqlen; i < pktsize; i += 1) {
		cmdbuf[i] = 0;
	}
	return pktsize;
}

uint32_t PS4_SYSV_ABI sceGnmDrawInitToDefaultContextState400(uint32_t* cmdbuf,
                                                             uint32_t size) {
	const uint32_t pktsize = 0x100;
	if (!cmdbuf || size < pktsize) {
		return 0;
	}
	const GnmGpuMode gpumode = sceGnmGpuMode();
	const uint32_t* seq;
	size_t seqlen;
	if (gpumode == GNM_GPU_NEO) {
		seq = CTX_INIT_SEQUENCE_400_NEO;
		seqlen = CTX_INIT_SEQUENCE_400_NEO_LEN;
	} else {
		seq = CTX_INIT_SEQUENCE_400;
		seqlen = CTX_INIT_SEQUENCE_400_LEN;
	}
	memcpy(cmdbuf, seq, seqlen * 4);
	/* Pad remaining with NOPs */
	for (size_t i = seqlen; i < pktsize; i += 1) {
		cmdbuf[i] = 0;
	}
	return pktsize;
}

int PS4_SYSV_ABI sceGnmDrawInitToDefaultContextStateInternalCommand(
    uint32_t* cmdbuf, uint32_t size) {
	if (!cmdbuf || size < 2) {
		return GNM_ERROR_CMD_FAILED;
	}
	cmdbuf[0] = PKT3(PKT3_CLEAR_STATE, 0, 0);
	cmdbuf[1] = 0;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmDrawInitToDefaultContextStateInternalSize(void) {
	return 2;
}

/* --- Dispatch functions --- */

int32_t PS4_SYSV_ABI sceGnmDispatchDirect(uint32_t* cmdbuf, uint32_t size,
                                          uint32_t threads_x,
                                          uint32_t threads_y,
                                          uint32_t threads_z,
                                          uint32_t flags) {
	if (!cmdbuf || size < 12) {
		return GNM_ERROR_CMD_FAILED;
	}
	(void)flags;

	cmdbuf[0] = PKT3(PKT3_SET_SH_REG, 3, 0);
	cmdbuf[1] = (R_00B81C_COMPUTE_NUM_THREAD_X - SI_SH_REG_OFFSET) >> 2;
	cmdbuf[2] = threads_x;
	cmdbuf[3] = threads_y;
	cmdbuf[4] = threads_z;
	cmdbuf += 5;

	cmdbuf[0] = PKT3(PKT3_DISPATCH_DIRECT, 2, 0);
	cmdbuf[1] = threads_x;
	cmdbuf[2] = threads_y;
	cmdbuf[3] = threads_z;
	cmdbuf[4] = 0;
	cmdbuf += 5;

	cmdbuf[0] = PKT3(PKT3_NOP, 1, 0);
	cmdbuf[1] = 0;
	cmdbuf[2] = 0;

	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmDispatchIndirect(uint32_t* cmdbuf, uint32_t size,
                                            uint32_t data_offset,
                                            uint32_t flags) {
	if (!cmdbuf || size < 7) {
		return GNM_ERROR_CMD_FAILED;
	}
	(void)flags;

	cmdbuf[0] = PKT3(PKT3_DISPATCH_INDIRECT, 2, 0);
	cmdbuf[1] = data_offset;
	cmdbuf[2] = 0;
	cmdbuf[3] = 0;
	cmdbuf[4] = 0;
	cmdbuf += 5;

	cmdbuf[0] = PKT3(PKT3_NOP, 1, 0);
	cmdbuf[1] = 0;
	cmdbuf[2] = 0;

	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmDispatchIndirectOnMec(uint32_t* cmdbuf,
                                                 uint32_t size,
                                                 uintptr_t args,
                                                 uint32_t modifier) {
	(void)cmdbuf;
	(void)size;
	(void)args;
	(void)modifier;
	return GNM_ERROR_UNSUPPORTED;
}

/* --- Submit functions (no real hardware on generic) --- */

int32_t PS4_SYSV_ABI sceGnmSubmitCommandBuffers(
    uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes) {
	(void)count;
	(void)dcb_gpu_addrs;
	(void)dcb_sizes_in_bytes;
	(void)ccb_gpu_addrs;
	(void)ccb_sizes_in_bytes;
	return GNM_ERROR_OK; /* no-op on generic */
}

int PS4_SYSV_ABI sceGnmSubmitAndFlipCommandBuffers(
    uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes, uint32_t vo_handle, uint32_t buf_idx,
    uint32_t flip_mode, int64_t flip_arg) {
	(void)count; (void)dcb_gpu_addrs; (void)dcb_sizes_in_bytes;
	(void)ccb_gpu_addrs; (void)ccb_sizes_in_bytes;
	(void)vo_handle; (void)buf_idx; (void)flip_mode; (void)flip_arg;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSubmitCommandBuffersForWorkload(
    uint32_t workload, uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes) {
	(void)workload; (void)count; (void)dcb_gpu_addrs;
	(void)dcb_sizes_in_bytes; (void)ccb_gpu_addrs; (void)ccb_sizes_in_bytes;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSubmitAndFlipCommandBuffersForWorkload(
    uint32_t workload, uint32_t count, void* const dcb_gpu_addrs[],
    uint32_t* dcb_sizes_in_bytes, void* const ccb_gpu_addrs[],
    uint32_t* ccb_sizes_in_bytes, uint32_t vo_handle, uint32_t buf_idx,
    uint32_t flip_mode, int64_t flip_arg) {
	(void)workload; (void)count; (void)dcb_gpu_addrs;
	(void)dcb_sizes_in_bytes; (void)ccb_gpu_addrs; (void)ccb_sizes_in_bytes;
	(void)vo_handle; (void)buf_idx; (void)flip_mode; (void)flip_arg;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSubmitDone(void) {
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmAreSubmitsAllowed(void) {
	return 1;
}

int PS4_SYSV_ABI sceGnmRequestFlipAndSubmitDone(void) {
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmRequestFlipAndSubmitDoneForWorkload(void) {
	return GNM_ERROR_OK;
}

/* --- Compute queue management (no real hardware) --- */

int32_t PS4_SYSV_ABI sceGnmMapComputeQueue(uint32_t pipe_id, uint32_t queue_id,
                                       uintptr_t ring_base_addr,
                                       uint32_t ring_size_dw,
                                       uint32_t* read_ptr_addr) {
	(void)pipe_id; (void)queue_id; (void)ring_base_addr;
	(void)ring_size_dw; (void)read_ptr_addr;
	return GNM_ERROR_UNSUPPORTED;
}

int PS4_SYSV_ABI sceGnmMapComputeQueueWithPriority(
    uint32_t pipe_id, uint32_t queue_id, uintptr_t ring_base_addr,
    uint32_t ring_size_dw, uint32_t* read_ptr_addr, uint32_t pipePriority) {
	(void)pipe_id; (void)queue_id; (void)ring_base_addr;
	(void)ring_size_dw; (void)read_ptr_addr; (void)pipePriority;
	return GNM_ERROR_UNSUPPORTED;
}

int PS4_SYSV_ABI sceGnmUnmapComputeQueue(uint32_t vqid) {
	(void)vqid;
	return GNM_ERROR_OK;
}

void PS4_SYSV_ABI sceGnmDingDong(uint32_t gnm_vqid, uint32_t next_offs_dw) {
	(void)gnm_vqid;
	(void)next_offs_dw;
}

void PS4_SYSV_ABI sceGnmDingDongForWorkload(uint32_t gnm_vqid,
                                            uint32_t next_offs_dw,
                                            uint64_t workload_id) {
	(void)gnm_vqid;
	(void)next_offs_dw;
	(void)workload_id;
}

int32_t PS4_SYSV_ABI sceGnmComputeWaitOnAddress(uint32_t* cmdbuf,
                                                uint32_t size, uintptr_t addr,
                                                uint32_t mask, uint32_t cmp_func,
                                                uint32_t ref) {
	if (!cmdbuf || size < 7 || cmp_func > GNM_WAIT_REG_MEM_FUNC_GREATER) {
		return GNM_ERROR_CMD_FAILED;
	}
	const uint64_t gpuaddr = (uint64_t)addr;
	const uint32_t highaddr = (uint32_t)(gpuaddr >> 32);
	if (highaddr > 0xffff) {
		return GNM_ERROR_CMD_FAILED;
	}
	cmdbuf[0] = PKT3(PKT3_WAIT_REG_MEM, 5, 0);
	cmdbuf[1] = cmp_func | WAIT_REG_MEM_MEM_SPACE(1);
	cmdbuf[2] = (uint32_t)(gpuaddr & 0xffffffff);
	cmdbuf[3] = highaddr;
	cmdbuf[4] = ref;
	cmdbuf[5] = mask;
	cmdbuf[6] = 10;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmComputeWaitSemaphore(void) {
	return GNM_ERROR_UNSUPPORTED;
}

/* --- VGT / wave control --- */

int32_t PS4_SYSV_ABI sceGnmResetVgtControl(uint32_t* cmdbuf, uint32_t size) {
	/* Writes IA_MULTI_VGT_PARAM register to default value 0xFF.
	 * 3 dwords: PKT3 header + reg offset + value. */
	if (!cmdbuf || size != 3) {
		return GNM_ERROR_CMD_FAILED;
	}
	cmdbuf[0] = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
	cmdbuf[1] = (R_028AA8_IA_MULTI_VGT_PARAM - SI_CONTEXT_REG_OFFSET) >> 2;
	cmdbuf[2] = 0xff;
	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmSetVgtControl(uint32_t* cmdbuf, uint32_t size,
                                         uint32_t prim_group_sz_minus_one,
                                         uint32_t partial_vs_wave_mode,
                                         uint32_t wd_switch_only_on_eop_mode) {
	/* Writes IA_MULTI_VGT_PARAM register.
	 * 3 dwords: PKT3 header + reg offset + value. */
	if (!cmdbuf || size != 3 || prim_group_sz_minus_one >= 0x100 ||
	    (wd_switch_only_on_eop_mode | partial_vs_wave_mode) >= 2) {
		return GNM_ERROR_CMD_FAILED;
	}
	if (sceGnmGpuMode() == GNM_GPU_NEO) {
		const uint32_t wd_switch_on_eop =
		    (wd_switch_only_on_eop_mode != 0) << 20;
		const uint32_t switch_on_eoi =
		    (wd_switch_only_on_eop_mode == 0) << 19;
		const uint32_t vgtparam = wd_switch_only_on_eop_mode != 0
		    ? ((partial_vs_wave_mode & 1) << 16) |
		          prim_group_sz_minus_one | wd_switch_on_eop |
		          switch_on_eoi | 0x40000
		    : (prim_group_sz_minus_one & 0x1cffff) |
		          wd_switch_on_eop | switch_on_eoi | 0x50000;

		cmdbuf[0] = PKT3(PKT3_SET_UCONFIG_REG, 1, 0);
		cmdbuf[1] = 0x40000258;  /* IA_MULTI_VGT_PARAM, Neo path */
		cmdbuf[2] = vgtparam | 0x600000;
		return GNM_ERROR_OK;
	}
	const uint32_t vgtparam =
	    ((partial_vs_wave_mode & 1) << 16) |
	    (prim_group_sz_minus_one & 0xffff);
	cmdbuf[0] = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
	cmdbuf[1] = (R_028AA8_IA_MULTI_VGT_PARAM - SI_CONTEXT_REG_OFFSET) >> 2;
	cmdbuf[2] = vgtparam;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSetGsRingSizes(void) {
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSetWaveLimitMultiplier(void) {
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSetWaveLimitMultipliers(void) {
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSetSpiEnableSqCounters(void) {
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmSetSpiEnableSqCountersForUnitInstance(void) {
	return GNM_ERROR_OK;
}

/* --- Markers (emit NOP packets) --- */

int32_t PS4_SYSV_ABI sceGnmInsertDingDongMarker(uint32_t* cmdbuf, uint32_t size) {
	if (!cmdbuf || size < 4) {
		return GNM_ERROR_CMD_FAILED;
	}
	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0;
	cmdbuf[2] = 0;
	cmdbuf[3] = 0;
	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmInsertPopMarker(uint32_t* cmdbuf, uint32_t size) {
	if (!cmdbuf || size < 2) {
		return GNM_ERROR_CMD_FAILED;
	}
	cmdbuf[0] = PKT3(PKT3_NOP, 0, 0);
	cmdbuf[1] = 0;
	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmInsertPushColorMarker(uint32_t* cmdbuf, uint32_t size,
                                                 const char* marker,
                                                 uint32_t color) {
	if (!cmdbuf || size < 4) {
		return GNM_ERROR_CMD_FAILED;
	}
	(void)marker;
	(void)color;
	cmdbuf[0] = PKT3(PKT3_NOP, 2, 0);
	cmdbuf[1] = 0;
	cmdbuf[2] = 0;
	cmdbuf[3] = 0;
	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmInsertPushMarker(uint32_t* cmdbuf, uint32_t size,
                                            const char* marker) {
	if (!cmdbuf || size < 2) {
		return GNM_ERROR_CMD_FAILED;
	}
	(void)marker;
	cmdbuf[0] = PKT3(PKT3_NOP, 0, 0);
	cmdbuf[1] = 0;
	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmInsertSetMarker(uint32_t* cmdbuf, uint32_t size,
                                           const char* marker) {
	if (!cmdbuf || size < 2) {
		return GNM_ERROR_CMD_FAILED;
	}
	(void)marker;
	cmdbuf[0] = PKT3(PKT3_NOP, 0, 0);
	cmdbuf[1] = 0;
	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmInsertWaitFlipDone(uint32_t* cmdbuf, uint32_t size,
                                              int32_t vo_handle,
                                              uint32_t buf_idx) {
	return sceGnmDriverInsertWaitFlipDone(cmdbuf, size, vo_handle, buf_idx);
}

/* --- Misc getters --- */

uintptr_t PS4_SYSV_ABI sceGnmGetTheTessellationFactorRingBufferBaseAddress(void) {
	return 0; /* NULL on generic */
}

int32_t PS4_SYSV_ABI sceGnmGetOffChipTessellationBufferSize(void) {
	return 0;
}

uint32_t PS4_SYSV_ABI sceGnmGetGpuCoreClockFrequency(void) {
	return 0;
}

void PS4_SYSV_ABI sceGnmFlushGarlic(void) {
	/* no-op on generic */
}

/* --- Event queue (no real hardware) --- */

int32_t PS4_SYSV_ABI sceGnmAddEqEvent(void* eq, uint64_t id, void* udata) {
	(void)eq; (void)id; (void)udata;
	return GNM_ERROR_UNSUPPORTED;
}

int32_t PS4_SYSV_ABI sceGnmDeleteEqEvent(void* eq, uint64_t id) {
	(void)eq; (void)id;
	return GNM_ERROR_OK;
}

int32_t PS4_SYSV_ABI sceGnmGetEqEventType(const void* ev) {
	(void)ev;
	return 0;
}

int32_t PS4_SYSV_ABI sceGnmGetEqTimeStamp(void) {
	return 0;
}

/* --- Workload management (no real hardware) --- */

int PS4_SYSV_ABI sceGnmBeginWorkload(uint32_t workload_stream, uint64_t* workload) {
	if (workload) {
		*workload = 0;
	}
	(void)workload_stream;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmEndWorkload(uint64_t workload) {
	(void)workload;
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmCreateWorkloadStream(uint64_t param1,
                                            uint32_t* workload_stream) {
	(void)param1;
	if (workload_stream) {
		*workload_stream = 0;
	}
	return GNM_ERROR_OK;
}

int PS4_SYSV_ABI sceGnmDestroyWorkloadStream(void) {
	return GNM_ERROR_OK;
}
/* Part 3: Stub functions (same return values as orbis backend). */
/* --- SDMA --- */
int PS4_SYSV_ABI sceGnmSdmaOpen(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSdmaClose(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSdmaConstFill(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSdmaCopyLinear(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSdmaCopyTiled(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSdmaCopyWindow(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSdmaFlush(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSdmaGetMinCmdSize(void) { return ORBIS_GNM_ERROR_FAILURE; }

/* --- Resource registration --- */
int32_t PS4_SYSV_ABI sceGnmFindResourcesPublic(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmFindResources(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetResourceBaseAddressAndSizeInBytes(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetResourceName(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetResourceRegistrationBuffers(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetResourceShaderGuid(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetResourceType(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetResourceUserData(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmQueryResourceRegistrationUserMemoryRequirements(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmRegisterGdsResource(void) { return ORBIS_GNM_ERROR_FAILURE; }
int32_t PS4_SYSV_ABI sceGnmRegisterOwner(void* handle, const char* name) {
	(void)handle; (void)name;
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmRegisterOwnerForSystem(void) { return ORBIS_GNM_ERROR_FAILURE; }
int32_t PS4_SYSV_ABI sceGnmRegisterResource(void* res_handle, void* owner_handle,
                                            const void* addr, size_t size,
                                            const char* name, int res_type,
                                            uint64_t user_data) {
	(void)res_handle; (void)owner_handle; (void)addr; (void)size;
	(void)name; (void)res_type; (void)user_data;
	return ORBIS_GNM_ERROR_FAILURE;
}
int PS4_SYSV_ABI sceGnmSetResourceRegistrationUserMemory(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSetResourceUserData(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmUnregisterAllResourcesForOwner(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmUnregisterOwnerAndResources(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmUnregisterResource(void) { return ORBIS_GNM_ERROR_FAILURE; }

/* --- Sqtt --- */
int PS4_SYSV_ABI sceGnmInsertThreadTraceMarker(void) { return 0; }
int PS4_SYSV_ABI sceGnmSqttFini(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttFinishTrace(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetBcInfo(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetGpuClocks(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetHiWater(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetStatus(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetTraceCounter(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetTraceWptr(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetWrapCounts(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetWrapCounts2(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttGetWritebackLabels(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttInit(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSelectMode(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSelectTarget(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSelectTokens(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSetCuPerfMask(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSetDceEventWrite(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSetHiWater(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSetTraceBuffer2(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSetTraceBuffers(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSetUserData(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSetUserdataTimer(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttStartTrace(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttStopTrace(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSwitchTraceBuffer(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttSwitchTraceBuffer2(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSqttWaitForEvent(void) { return ORBIS_GNM_ERROR_FAILURE; }

/* --- Spm --- */
int PS4_SYSV_ABI sceGnmSpmEndSpm(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmInit(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmInit2(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmSetDelay(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmSetMuxRam(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmSetMuxRam2(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmSetSelectCounter(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmSetSpmSelects(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmSetSpmSelects2(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmSpmStartSpm(void) { return ORBIS_GNM_ERROR_FAILURE; }

/* --- Debugger --- */
int PS4_SYSV_ABI sceGnmDebugHardwareStatus(void) { return 0; }
int PS4_SYSV_ABI sceGnmDebugModuleReset(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebugReset(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerGetAddressWatch(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerHaltWavefront(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerReadGds(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerReadSqIndirectRegister(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerResumeWavefront(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerResumeWavefrontCreation(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerSetAddressWatch(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerWriteGds(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDebuggerWriteSqIndirectRegister(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetDbgGcHandle(void) { return -1; }

/* --- Razor --- */
int PS4_SYSV_ABI sceRazorCaptureCommandBuffersOnlyImmediate(void) { return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL; }
int PS4_SYSV_ABI sceRazorCaptureCommandBuffersOnlySinceLastFlip(void) { return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL; }
int PS4_SYSV_ABI sceRazorCaptureImmediate(void) { return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL; }
int PS4_SYSV_ABI sceRazorCaptureSinceLastFlip(void) { return ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL; }
bool PS4_SYSV_ABI sceRazorIsLoaded(void) { return false; }

/* --- Markers stub --- */
int PS4_SYSV_ABI sceGnmInsertSetColorMarker(void) { return 0; }

/* --- Coredump / misc --- */
int PS4_SYSV_ABI sceGnmGetCoredumpAddress(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetCoredumpMode(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetCoredumpProtectionFaultTimestamp(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetDebugTimestamp(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetGpuBlockStatus(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetGpuInfoStatus(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetLastWaitedAddress(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetNumTcaUnits(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetOwnerName(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmGetPhysicalCounterFromVirtualized(void) { return ORBIS_GNM_ERROR_FAILURE; }
uint32_t PS4_SYSV_ABI sceGnmGetProtectionFaultTimeStamp(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetShaderProgramBaseAddress(void) { return 0; }
int PS4_SYSV_ABI sceGnmGetShaderStatus(void) { return 0; }
int PS4_SYSV_ABI sceGnmIsCoredumpValid(void) { return 0; }
int PS4_SYSV_ABI sceGnmRaiseUserExceptionEvent(void) { return 0; }

/* --- Driver internal --- */
bool PS4_SYSV_ABI sceGnmDriverCaptureInProgress(void) { return false; }
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterface(void) { return 0x80000000; }
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterfaceForGpuDebugger(void) { return 0x80000000; }
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterfaceForGpuException(void) { return 0x80000000; }
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterfaceForHDRScopes(void) { return 0x80000000; }
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterfaceForReplay(void) { return 0x80000000; }
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterfaceForResourceRegistration(void) { return 0x80000000; }
uint32_t PS4_SYSV_ABI sceGnmDriverInternalRetrieveGnmInterfaceForValidation(void) { return 0x80000000; }
int PS4_SYSV_ABI sceGnmDriverInternalVirtualQuery(void) { return 0; }
bool PS4_SYSV_ABI sceGnmDriverTraceInProgress(void) { return false; }
int PS4_SYSV_ABI sceGnmDriverTriggerCapture(void) { return ORBIS_GNM_ERROR_CAPTURE_RAZOR_NOT_LOADED; }
void PS4_SYSV_ABI sceGnmRegisterGnmLiveCallbackConfig(void) { }

/* --- Logical CU / GPU PA --- */
void PS4_SYSV_ABI sceGnmGpuPaDebugEnter(void) { }
void PS4_SYSV_ABI sceGnmGpuPaDebugLeave(void) { }
bool PS4_SYSV_ABI sceGnmIsUserPaEnabled(void) { return false; }
int PS4_SYSV_ABI sceGnmLogicalCuIndexToPhysicalCuIndex(void) { return 0; }
int32_t PS4_SYSV_ABI sceGnmLogicalCuMaskToPhysicalCuMask(int64_t a, int32_t logical_cu_mask) {
	(void)a;
	return logical_cu_mask;
}
int PS4_SYSV_ABI sceGnmLogicalTcaUnitToPhysical(void) { return 0; }
int PS4_SYSV_ABI sceGnmPaDisableFlipCallbacks(void) { return 0; }
int PS4_SYSV_ABI sceGnmPaEnableFlipCallbacks(void) { return 0; }
int PS4_SYSV_ABI sceGnmPaHeartbeat(void) { return 0; }

/* --- Mip stats --- */
int PS4_SYSV_ABI sceGnmDisableMipStatsReport(void) { return 0; }
int PS4_SYSV_ABI sceGnmRequestMipStatsReportAndReset(void) { return 0; }
int PS4_SYSV_ABI sceGnmSetupMipStatsReport(void) { return 0; }

/* --- Draw void-param stubs --- */
int PS4_SYSV_ABI sceGnmDrawIndexMultiInstanced(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDrawIndirectCountMulti(void) { return ORBIS_GNM_ERROR_FAILURE; }
int PS4_SYSV_ABI sceGnmDrawOpaqueAuto(void) { return ORBIS_GNM_ERROR_FAILURE; }

/* --- Unnamed NID-only exports --- */
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
/* Part 4: Validate stubs (return 0, same as orbis). */
int32_t PS4_SYSV_ABI sceGnmValidateCommandBuffers(void) { return 0; }
int PS4_SYSV_ABI sceGnmValidateDisableDiagnostics(void) { return 0; }
int PS4_SYSV_ABI sceGnmValidateDisableDiagnostics2(void) { return 0; }
int32_t PS4_SYSV_ABI sceGnmValidateDispatchCommandBuffers(void) { return 0; }
int32_t PS4_SYSV_ABI sceGnmValidateDrawCommandBuffers(void) { return 0; }
int PS4_SYSV_ABI sceGnmValidateGetDiagnosticInfo(void) { return 0; }
int PS4_SYSV_ABI sceGnmValidateGetDiagnostics(void) { return 0; }
int PS4_SYSV_ABI sceGnmValidateGetVersion(void) { return 0; }
bool PS4_SYSV_ABI sceGnmValidateOnSubmitEnabled(void) { return false; }
int PS4_SYSV_ABI sceGnmValidateResetState(void) { return 0; }
int PS4_SYSV_ABI sceGnmValidationRegisterMemoryCheckCallback(void) { return 0; }
