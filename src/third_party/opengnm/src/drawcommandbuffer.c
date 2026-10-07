#include "gnm_drawcommandbuffer.h"

#include "gnmdriver.h"
#include "platform.h"
#include "pm4/pm4_ps4.h"
#include "pm4/sid.h"

#include "u/utility.h"

static inline bool cmdcanfit(GnmCommandBuffer* cmd, uint32_t sizedwords) {
	uint32_t remainingdwords = cmd->endptr - cmd->cmdptr;
	if (sizedwords > remainingdwords) {
		if (!cmd->callback.func(
			cmd, sizedwords, cmd->callback.userdata
		    )) {
			sceGnmWriteMsg(
			    GNM_MSGSEV_ERR, "Command buffer resizing failed"
			);
			return false;
		}
		remainingdwords = cmd->endptr - cmd->cmdptr;
		if (sizedwords > remainingdwords) {
			sceGnmWriteMsg(
			    GNM_MSGSEV_ERR, "Command buffer resizing was too small"
			);
			return false;
		}
	}

	return true;
}

static void setcontextregisterrange(
    GnmCommandBuffer* cmd, uint32_t regaddr, const uint32_t* regvalues,
    uint32_t numvalues
) {
	if (regaddr < SI_CONTEXT_REG_OFFSET ||
	    numvalues > (SI_CONTEXT_REG_END - regaddr) / sizeof(uint32_t)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Invalid context register 0x%x used",
		    regaddr
		);
		return;
	}

	const uint32_t numdwords = 2 + numvalues;
	if (cmd->cmdptr + numdwords > cmd->endptr) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Command is too large. Dwords: %u",
		    numdwords
		);
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_SET_CONTEXT_REG, numvalues, 0);
	cmd->cmdptr[1] = (regaddr - SI_CONTEXT_REG_OFFSET) >> 2;
	for (uint32_t i = 0; i < numvalues; i += 1) {
		cmd->cmdptr[2 + i] = regvalues[i];
	}

	cmd->cmdptr += numdwords;
}
static inline void setcontextregister(
    GnmCommandBuffer* cmd, uint32_t regaddr, uint32_t regvalue
) {
	setcontextregisterrange(cmd, regaddr, &regvalue, 1);
}

static void setpersistentregisterrange(
    GnmCommandBuffer* cmd, uint32_t regaddr, const uint32_t* regvalues,
    uint32_t numvalues
) {
	if (regaddr < SI_SH_REG_OFFSET ||
	    numvalues > (SI_SH_REG_END - regaddr) / sizeof(uint32_t)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Invalid persistent register 0x%x used",
		    regaddr
		);
		return;
	}

	const uint32_t numdwords = 2 + numvalues;
	if (cmd->cmdptr + numdwords > cmd->endptr) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Command is too large. Dwords: %u",
		    numdwords
		);
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_SET_SH_REG, numvalues, 0);
	cmd->cmdptr[1] = (regaddr - SI_SH_REG_OFFSET) >> 2;
	for (uint32_t i = 0; i < numvalues; i += 1) {
		cmd->cmdptr[2 + i] = regvalues[i];
	}

	cmd->cmdptr += numdwords;
}
static inline void setpersistentregister(
    GnmCommandBuffer* cmd, uint32_t regaddr, uint32_t regvalue
) {
	setpersistentregisterrange(cmd, regaddr, &regvalue, 1);
}

static void setuserregisterrange(
    GnmCommandBuffer* cmd, uint32_t regaddr, const uint32_t* regvalues,
    uint32_t numvalues
) {
	if (regaddr < CIK_UCONFIG_REG_OFFSET ||
	    numvalues > (CIK_UCONFIG_REG_END - regaddr) / sizeof(uint32_t)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Invalid user config register 0x%x used",
		    regaddr
		);
		return;
	}

	const uint32_t numdwords = 2 + numvalues;
	if (cmd->cmdptr + numdwords > cmd->endptr) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Command is too large. Dwords: %u",
		    numdwords
		);
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_SET_UCONFIG_REG, numvalues, 0);
	cmd->cmdptr[1] = (regaddr - CIK_UCONFIG_REG_OFFSET) >> 2;
	for (uint32_t i = 0; i < numvalues; i += 1) {
		cmd->cmdptr[2 + i] = regvalues[i];
	}

	cmd->cmdptr += numdwords;
}
static inline void setuserregister(
    GnmCommandBuffer* cmd, uint32_t regaddr, uint32_t regvalue
) {
	setuserregisterrange(cmd, regaddr, &regvalue, 1);
}

void sceGnmDrawCmdInitDefaultHardwareState(GnmCommandBuffer* cmd) {
	const uint32_t cmddwords = 256;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	int32_t res =
	    sceGnmDrawInitDefaultHardwareState350(cmd->cmdptr, cmddwords);
	if (res > 0) {
		cmd->cmdptr += res;
	} else {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "DrawInitDefaultHardwareState350 failed"
		);
	}
}

void sceGnmDrawCmdDrawIndex(
    GnmCommandBuffer* cmd, uint32_t indexcount, const void* indexaddr
) {
	const GnmDrawModifier mod = {0};
	sceGnmDrawCmdDrawIndex2(cmd, indexcount, indexaddr, mod);
}
void sceGnmDrawCmdDrawIndex2(
    GnmCommandBuffer* cmd, uint32_t indexcount, const void* indexaddr,
    GnmDrawModifier modifier
) {
	const uint32_t cmddwords = 10;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	    .rendertargetsliceoffset = modifier.rendertargetsliceoffset,
	};
	int32_t res = sceGnmDriverDrawIndex(
	    cmd->cmdptr, cmddwords, indexcount, indexaddr, flags
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	}
}

void sceGnmDrawCmdDrawIndexAuto(GnmCommandBuffer* cmd, uint32_t indexcount) {
	GnmDrawModifier modifier = {0};
	sceGnmDrawCmdDrawIndexAuto2(cmd, indexcount, modifier);
}

void sceGnmDrawCmdDrawIndexAuto2(
    GnmCommandBuffer* cmd, uint32_t indexcount, GnmDrawModifier modifier
) {
	const uint32_t cmddwords = 7;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	    .rendertargetsliceoffset = modifier.rendertargetsliceoffset,
	};
	int32_t res =
	    sceGnmDriverDrawIndexAuto(cmd->cmdptr, cmddwords, indexcount, flags);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	}
}

void sceGnmDrawCmdDrawIndexIndirect(
    GnmCommandBuffer* cmd, uint32_t dataoffset, GnmShaderStage stage,
    uint8_t vertexoffusgpr, uint8_t instanceoffusgpr
) {
	const GnmDrawModifier mod = {0};
	sceGnmDrawCmdDrawIndexIndirect2(
	    cmd, dataoffset, stage, vertexoffusgpr, instanceoffusgpr, mod
	);
}
void sceGnmDrawCmdDrawIndexIndirect2(
    GnmCommandBuffer* cmd, uint32_t dataoffset, GnmShaderStage stage,
    uint8_t vertexoffusgpr, uint8_t instanceoffusgpr, GnmDrawModifier mod
) {
	const uint32_t cmddwords = 9;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (vertexoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirect: vertexoffsetusgpr is %u too large",
		    vertexoffusgpr
		);
	}
	if (instanceoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirect: instanceoffusgpr is %u too large",
		    instanceoffusgpr
		);
	}
	if (stage < GNM_STAGE_CS || stage > GNM_STAGE_LS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "DrawIndexIndirect: stage %u is invalid",
		    stage
		);
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	    .rendertargetsliceoffset = mod.rendertargetsliceoffset,
	};
	int32_t res = sceGnmDriverDrawIndexIndirect(
	    cmd->cmdptr, cmddwords, dataoffset, stage, vertexoffusgpr,
	    instanceoffusgpr, flags
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirect: driver call failed with 0x%x", res
		);
	}
}

void sceGnmDrawCmdDrawIndirect(
    GnmCommandBuffer* cmd, uint32_t dataoffset, GnmShaderStage stage,
    uint8_t vertexoffusgpr, uint8_t instanceoffusgpr
) {
	const GnmDrawModifier mod = {0};
	sceGnmDrawCmdDrawIndirect2(
	    cmd, dataoffset, stage, vertexoffusgpr, instanceoffusgpr, mod
	);
}
void sceGnmDrawCmdDrawIndirect2(
    GnmCommandBuffer* cmd, uint32_t dataoffset, GnmShaderStage stage,
    uint8_t vertexoffusgpr, uint8_t instanceoffusgpr, GnmDrawModifier mod
) {
	const uint32_t cmddwords = 9;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (vertexoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndirect: vertexoffsetusgpr is %u too large",
		    vertexoffusgpr
		);
	}
	if (instanceoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndirect: instanceoffusgpr is %u too large",
		    instanceoffusgpr
		);
	}
	if (stage < GNM_STAGE_CS || stage > GNM_STAGE_LS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "DrawIndirect: stage %u is invalid", stage
		);
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	    .rendertargetsliceoffset = mod.rendertargetsliceoffset,
	};
	int32_t res = sceGnmDriverDrawIndirect(
	    cmd->cmdptr, cmddwords, dataoffset, stage, vertexoffusgpr,
	    instanceoffusgpr, flags
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndirect: driver call failed with 0x%x", res
		);
	}
}

void sceGnmDrawCmdDrawIndexIndirectMulti(
    GnmCommandBuffer* cmd, uint32_t dataoffset, uint32_t maxcount,
    GnmShaderStage stage, uint8_t vertexoffusgpr, uint8_t instanceoffusgpr
) {
	const uint32_t cmddwords = 11;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (vertexoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectMulti: vertexoffsetusgpr is %u too large",
		    vertexoffusgpr
		);
	}
	if (instanceoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectMulti: instanceoffusgpr is %u too large",
		    instanceoffusgpr
		);
	}
	if (stage != GNM_STAGE_VS && stage != GNM_STAGE_ES &&
	    stage != GNM_STAGE_LS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectMulti: stage %u is invalid (must be "
		    "VS/ES/LS)",
		    stage
		);
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	};
	int32_t res = sceGnmDriverDrawIndexIndirectMulti(
	    cmd->cmdptr, cmddwords, dataoffset, maxcount, stage,
	    vertexoffusgpr, instanceoffusgpr, flags
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectMulti: driver call failed with 0x%x",
		    res
		);
	}
}

void sceGnmDrawCmdDrawIndirectMulti(
    GnmCommandBuffer* cmd, uint32_t dataoffset, uint32_t maxcount,
    GnmShaderStage stage, uint8_t vertexoffusgpr, uint8_t instanceoffusgpr
) {
	const uint32_t cmddwords = 11;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (vertexoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndirectMulti: vertexoffsetusgpr is %u too large",
		    vertexoffusgpr
		);
	}
	if (instanceoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndirectMulti: instanceoffusgpr is %u too large",
		    instanceoffusgpr
		);
	}
	if (stage < GNM_STAGE_CS || stage > GNM_STAGE_LS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndirectMulti: stage %u is invalid", stage
		);
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	};
	int32_t res = sceGnmDriverDrawIndirectMulti(
	    cmd->cmdptr, cmddwords, dataoffset, maxcount, stage,
	    vertexoffusgpr, instanceoffusgpr, flags
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndirectMulti: driver call failed with 0x%x", res
		);
	}
}

void sceGnmDrawCmdDrawIndexIndirectCountMulti(
    GnmCommandBuffer* cmd, uint32_t dataoffset, uint32_t maxcount,
    uint64_t countaddr, GnmShaderStage stage, uint8_t vertexoffusgpr,
    uint8_t instanceoffusgpr
) {
	const uint32_t cmddwords = 16;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (vertexoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectCountMulti: vertexoffsetusgpr is %u "
		    "too large",
		    vertexoffusgpr
		);
	}
	if (instanceoffusgpr > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectCountMulti: instanceoffusgpr is %u "
		    "too large",
		    instanceoffusgpr
		);
	}
	if (stage != GNM_STAGE_VS && stage != GNM_STAGE_ES &&
	    stage != GNM_STAGE_LS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectCountMulti: stage %u is invalid (must "
		    "be VS/ES/LS)",
		    stage
		);
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	};
	int32_t res = sceGnmDriverDrawIndexIndirectCountMulti(
	    cmd->cmdptr, cmddwords, dataoffset, maxcount, countaddr, stage,
	    vertexoffusgpr, instanceoffusgpr, flags
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "DrawIndexIndirectCountMulti: driver call failed with "
		    "0x%x",
		    res
		);
	}
}

void sceGnmDrawCmdSetDepthClearValue(GnmCommandBuffer* cmd, float clearvalue) {
	if (clearvalue < 0.0 || clearvalue > 1.0) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "DepthClear: invalid value %f", clearvalue
		);
	}

	setcontextregister(cmd, R_02802C_DB_DEPTH_CLEAR, fui(clearvalue));
}

void sceGnmDrawCmdSetDepthRenderTarget(
    GnmCommandBuffer* cmd, const GnmDepthRenderTarget* depthtarget
) {
	void* zread = 0;
	void* stencilread = 0;
	if (depthtarget) {
		zread = sceGnmDrtGetZReadAddress(depthtarget);
		stencilread = sceGnmDrtGetStencilReadAddress(depthtarget);
	}

	if (zread || stencilread) {
		void* htile = sceGnmDrtGetHtileAddress(depthtarget);

		if (depthtarget->zinfo.tilesurfaceenable && !htile) {
			sceGnmWriteMsgf(
			    GNM_MSGSEV_ERR,
			    "SetDepthRenderTarget: htile acceleration is set "
			    "without a htile address"
			);
		}
		if (zread && depthtarget->zinfo.format == GNM_Z_INVALID) {
			sceGnmWriteMsgf(
			    GNM_MSGSEV_ERR,
			    "SetDepthRenderTarget: Z format is invalid"
			);
			return;
		}
		if (stencilread &&
		    depthtarget->stencilinfo.format == GNM_STENCIL_INVALID) {
			sceGnmWriteMsgf(
			    GNM_MSGSEV_ERR,
			    "SetDepthRenderTarget: stencil format is invalid"
			);
			return;
		}

		_Static_assert(8 * 4 <= sizeof(GnmDepthRenderTarget), "");
		setcontextregisterrange(
		    cmd, R_028040_DB_Z_INFO, (const uint32_t*)depthtarget, 8
		);
		setcontextregister(
		    cmd, R_02803C_DB_DEPTH_INFO, depthtarget->depthinfo.asuint
		);
		setcontextregister(
		    cmd, R_028008_DB_DEPTH_VIEW, depthtarget->depthview.asuint
		);
		setcontextregister(
		    cmd, R_028014_DB_HTILE_DATA_BASE,
		    depthtarget->htiledatabase256b
		);
		setcontextregister(
		    cmd, R_028ABC_DB_HTILE_SURFACE,
		    depthtarget->htilesurface.asuint
		);

		if (!cmdcanfit(cmd, 2)) {
			return;
		}
		cmd->cmdptr[0] = PKT3(PKT3_NOP, 0, 0);
		cmd->cmdptr[1] = depthtarget->size.asuint;
		cmd->cmdptr += 2;
	} else {
		setcontextregister(cmd, R_028040_DB_Z_INFO, 0);
		setcontextregister(cmd, R_028044_DB_STENCIL_INFO, 0);
	}
}

void sceGnmDrawCmdSetGuardBands(
    GnmCommandBuffer* cmd, float horzclip, float vertclip, float horzdiscard,
    float vertdiscard
) {
	const uint32_t cmddwords = 2 + 4;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (horzclip < 1.0f) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "horzclip %f must be between >= than 1.0",
		    horzclip
		);
	}
	if (vertclip < 1.0f) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "vertclip %f must be between >= than 1.0",
		    vertclip
		);
	}
	if (horzdiscard < 1.0f) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "horzdiscard %f must be between >= than 1.0", horzdiscard
		);
	}
	if (vertdiscard < 1.0f) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "vertdiscard %f must be between >= than 1.0", vertdiscard
		);
	}

	const float newvalues[4] = {
	    vertclip, vertdiscard, horzclip, horzdiscard};
	setcontextregisterrange(
	    cmd, R_028BE8_PA_CL_GB_VERT_CLIP_ADJ, (const uint32_t*)newvalues, 4
	);
}

void sceGnmDrawCmdSetHwScreenOffset(
    GnmCommandBuffer* cmd, uint32_t offsetx, uint32_t offsety
) {
	if (offsetx > 508) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "offsetx %u must be between 0 and 508",
		    offsetx
		);
	}
	if (offsety > 508) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "offsety %u must be between 0 and 508",
		    offsety
		);
	}
	if (offsetx & 3) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "offsetx %u must be a multiple of 4",
		    offsetx
		);
	}
	if (offsety & 3) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "offsety %u must be a multiple of 4",
		    offsety
		);
	}

	const uint32_t newval = S_028234_HW_SCREEN_OFFSET_X(offsetx) |
				S_028234_HW_SCREEN_OFFSET_Y(offsety);
	setcontextregister(cmd, R_028234_PA_SU_HARDWARE_SCREEN_OFFSET, newval);
}

void sceGnmDrawCmdSetIndexSize(
    GnmCommandBuffer* cmd, GnmIndexSize indexsize, GnmCachePolicy cachepol
) {
	const uint32_t cmddwords = 2;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_INDEX_TYPE, 0, 0);
	cmd->cmdptr[1] =
	    (indexsize & 0x3) |				      // INDEX_TYPE
	    ((cachepol & 0x3) << 6) |			      // RDREQ_POLICY
	    (((cachepol != GNM_POLICY_BYPASS) & 0x1) << 10);  // REQ_PATH
	cmd->cmdptr += cmddwords;
}

void sceGnmDrawCmdSetIndexBuffer(GnmCommandBuffer* cmd, const void* addr) {
	const uint32_t cmddwords = 3;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (!ispow2aligned((uint64_t)addr, 2)) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "SetIndexBuffer: address must be 2 bytes aligned"
		);
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_INDEX_BASE, 1, 0);
	cmd->cmdptr[1] = (uint64_t)addr & 0xfffffffe;
	cmd->cmdptr[2] = ((uint64_t)addr >> 32) & 0xffffffff;
	cmd->cmdptr += cmddwords;
}

void sceGnmDrawCmdSetIndexCount(GnmCommandBuffer* cmd, uint32_t count) {
	const uint32_t cmddwords = 2;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_INDEX_BUFFER_SIZE, 0, 0);
	cmd->cmdptr[1] = count;
	cmd->cmdptr += cmddwords;
}

static inline void setindirectargs(GnmCommandBuffer* cmd, uint64_t addr) {
	const uint32_t cmddwords = 4;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if (!ispow2aligned(addr, 8)) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "SetIndirectArgs: address must be 8 bytes aligned"
		);
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_SET_BASE, 2, 0);
	cmd->cmdptr[1] = 1;
	cmd->cmdptr[2] = addr & 0xfffffff8;
	cmd->cmdptr[3] = (addr >> 32) & 0xffffffff;
	cmd->cmdptr += cmddwords;
}

void sceGnmDrawCmdSetIndirectArgs(
    GnmCommandBuffer* cmd, const GnmDrawIndirectArgs* args
) {
	setindirectargs(cmd, (uint64_t)args);
}

void sceGnmDrawCmdSetIndexedIndirectArgs(
    GnmCommandBuffer* cmd, const GnmDrawIndexedIndirectArgs* args
) {
	setindirectargs(cmd, (uint64_t)args);
}

void sceGnmDrawCmdSetInstanceStepRate(
    GnmCommandBuffer* cmd, uint32_t rate0, uint32_t rate1
) {
	const uint32_t rates[2] = {rate0, rate1};
	setcontextregisterrange(
	    cmd, R_028AA0_VGT_INSTANCE_STEP_RATE_0, rates, uasize(rates)
	);
}

void sceGnmDrawCmdSetNumInstances(GnmCommandBuffer* cmd, uint32_t count) {
	const uint32_t cmddwords = 2;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_NUM_INSTANCES, 0, 0);
	cmd->cmdptr[1] = count;
	cmd->cmdptr += cmddwords;
}

void sceGnmDrawCmdSetPrimitiveType(
    GnmCommandBuffer* cmd, GnmPrimitiveType primtype
) {
	setuserregister(cmd, R_030908_VGT_PRIMITIVE_TYPE, primtype);
}

void sceGnmDrawCmdSetRenderTarget(
    GnmCommandBuffer* cmd, uint32_t rtslot, const GnmRenderTarget* rt
) {
	if (rtslot > 7) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "rtslot %u must be between 0 and 7", rtslot
		);
		return;
	}

	if (rt) {
		setcontextregisterrange(
		    cmd, R_028C60_CB_COLOR0_BASE + rtslot * 0x3c,
		    (const uint32_t*)rt, 14
		);

		if (!cmdcanfit(cmd, 2)) {
			return;
		}
		cmd->cmdptr[0] = PKT3(PKT3_NOP, 0, 0);
		cmd->cmdptr[1] = rt->size.asuint;
		cmd->cmdptr += 2;
	} else {
		setcontextregister(
		    cmd, R_028C70_CB_COLOR0_INFO + rtslot * 0x3c, 0
		);
	}
}

void sceGnmDrawCmdSetRenderTargetMask(GnmCommandBuffer* cmd, uint32_t mask) {
	const uint32_t cmddwords = 2 + 1;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	setcontextregister(cmd, R_028238_CB_TARGET_MASK, mask);
}

void sceGnmDrawCmdSetScreenScissor(
    GnmCommandBuffer* cmd, int32_t left, int32_t top, int32_t right,
    int32_t bottom
) {
	if (left < -32768 || left > 16383) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "left %i must be between -32768 and 16383",
		    left
		);
	}
	if (top < -32768 || top > 16383) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "top %i must be between -32768 and 16383",
		    top
		);
	}
	if (right < -32768 || right > 16384) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "right %i must be between -32768 and 16384",
		    right
		);
	}
	if (bottom < -32768 || bottom > 16384) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "bottom %i must be between -32768 and 16384", bottom
		);
	}

	const int16_t scissor[4] = {left, top, right, bottom};
	setcontextregisterrange(
	    cmd, R_028030_PA_SC_SCREEN_SCISSOR_TL, (const uint32_t*)scissor, 2
	);
}

void sceGnmDrawCmdSetViewport(
    GnmCommandBuffer* cmd, uint32_t viewportid, const GnmSetViewportInfo* vpinfo
) {
	if (!vpinfo) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "vpinfo must not be NULL");
		return;
	}
	if (viewportid > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "viewportid %u must be between 0 and 15",
		    viewportid
		);
		return;
	}

	const float dvalues[2] = {vpinfo->dmin, vpinfo->dmax};
	setcontextregisterrange(
	    cmd, R_0282D0_PA_SC_VPORT_ZMIN_0 + viewportid * 0x8,
	    (const uint32_t*)dvalues, 2
	);

	const float scaleoffsets[6] = {vpinfo->scale[0], vpinfo->offset[0],
				       vpinfo->scale[1], vpinfo->offset[1],
				       vpinfo->scale[2], vpinfo->offset[2]};
	setcontextregisterrange(
	    cmd, R_02843C_PA_CL_VPORT_XSCALE + viewportid * 0x18,
	    (const uint32_t*)scaleoffsets, 6
	);
}

void sceGnmDrawCmdSetPsShader(
    GnmCommandBuffer* cmd, const GnmPsStageRegisters* regs
) {
	const uint32_t cmddwords = 40;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	int32_t res = sceGnmDriverSetPsShader350(cmd->cmdptr, cmddwords, regs);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetPsShader: driver call failed with 0x%x",
		    res
		);
	}
}

void sceGnmDrawCmdSetEmbeddedPsShader(
    GnmCommandBuffer* cmd, GnmEmbeddedPsShader shaderid
) {
	const uint32_t cmddwords = 40;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	int32_t res =
	    sceGnmDriverSetEmbeddedPsShader(cmd->cmdptr, cmddwords, shaderid);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	}
}

void sceGnmDrawCmdSetVsShader(
    GnmCommandBuffer* cmd, const GnmVsStageRegisters* regs,
    uint32_t shadermodifier
) {
	const uint32_t cmddwords = 29;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	int32_t res =
	    sceGnmDriverSetVsShader(cmd->cmdptr, cmddwords, regs, shadermodifier);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetVsShader: driver call failed with 0x%x",
		    res
		);
	}
}

void sceGnmDrawCmdSetEmbeddedVsShader(
    GnmCommandBuffer* cmd, GnmEmbeddedVsShader shaderid, uint32_t shadermodifier
) {
	const uint32_t cmddwords = 29;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	int32_t res = sceGnmDriverSetEmbeddedVsShader(
	    cmd->cmdptr, cmddwords, shaderid, shadermodifier
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "SetEmbeddedVsShader: driver call failed with 0x%x", res
		);
	}
}

// Write a trailing NOP packet with N data dwords
static inline void writetrailingnop(
    GnmCommandBuffer* cmd, uint32_t numdata
) {
	const uint32_t numdwords = 1 + numdata;
	if (!cmdcanfit(cmd, numdwords)) {
		return;
	}
	cmd->cmdptr[0] = PKT3(PKT3_NOP, numdata - 1, 0);
	for (uint32_t i = 0; i < numdata; i += 1) {
		cmd->cmdptr[1 + i] = 0;
	}
	cmd->cmdptr += numdwords;
}

// Set SH registers with compute shader type (MEC engine)
static void setshregcompute(
    GnmCommandBuffer* cmd, uint32_t regaddr, const uint32_t* regvalues,
    uint32_t numvalues
) {
	if (regaddr < SI_SH_REG_OFFSET ||
	    numvalues > (SI_SH_REG_END - regaddr) / sizeof(uint32_t)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "Invalid SH register 0x%x used", regaddr
		);
		return;
	}

	const uint32_t numdwords = 2 + numvalues;
	if (!cmdcanfit(cmd, numdwords)) {
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_SET_SH_REG, numvalues, 0) |
			 PKT3_SHADER_TYPE_S(1);  // compute engine
	cmd->cmdptr[1] = (regaddr - SI_SH_REG_OFFSET) >> 2;
	for (uint32_t i = 0; i < numvalues; i += 1) {
		cmd->cmdptr[2 + i] = regvalues[i];
	}
	cmd->cmdptr += numdwords;
}

void sceGnmDrawCmdSetCsShader(
    GnmCommandBuffer* cmd, const GnmCsStageRegisters* regs
) {
	sceGnmDrawCmdSetCsShaderWithModifier(cmd, regs, 0);
}

void sceGnmDrawCmdSetCsShaderWithModifier(
    GnmCommandBuffer* cmd, const GnmCsStageRegisters* regs,
    uint32_t shadermodifier
) {
	if (!regs) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "SetCsShader: regs must not be NULL");
		return;
	}
	if (regs->computepgmhi != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "SetCsShader: invalid shader address (hi != 0)"
		);
		return;
	}
	if ((shadermodifier & 0xfffffc3f) != 0) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetCsShader: invalid modifier mask 0x%x",
		    shadermodifier
		);
		return;
	}

	// COMPUTE_PGM_LO / COMPUTE_PGM_HI
	const uint32_t pgmlo[2] = {regs->computepgmlo, 0};
	setshregcompute(cmd, R_00B830_COMPUTE_PGM_LO, pgmlo, 2);

	// COMPUTE_PGM_RSRC1 / COMPUTE_PGM_RSRC2
	const uint32_t rsrc1 = shadermodifier == 0
				   ? regs->computepgmrsrc1
				   : (regs->computepgmrsrc1 & 0xfffffc3f) | shadermodifier;
	const uint32_t rsrc[2] = {rsrc1, regs->computepgmrsrc2};
	setshregcompute(cmd, R_00B848_COMPUTE_PGM_RSRC1, rsrc, 2);

	// COMPUTE_NUM_THREAD_X / Y / Z
	const uint32_t threads[3] = {
	    regs->computenumthreadx, regs->computenumthready,
	    regs->computenumthreadz};
	setshregcompute(cmd, R_00B81C_COMPUTE_NUM_THREAD_X, threads, 3);

	// Trailing NOP (11 data dwords)
	writetrailingnop(cmd, 11);
}

void sceGnmDrawCmdSetGsShader(
    GnmCommandBuffer* cmd, const GnmGsStageRegisters* regs
) {
	if (!regs) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "SetGsShader: regs must not be NULL");
		return;
	}
	if (regs->spishaderpgmhigs != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "SetGsShader: invalid shader address (hi != 0)"
		);
		return;
	}

	// SPI_SHADER_PGM_LO_GS / SPI_SHADER_PGM_HI_GS
	const uint32_t pgmlo[2] = {regs->spishaderpgmlogs, 0};
	setpersistentregisterrange(cmd, R_00B220_SPI_SHADER_PGM_LO_GS, pgmlo, 2);

	// SPI_SHADER_PGM_RSRC1_GS / SPI_SHADER_PGM_RSRC2_GS
	const uint32_t rsrc[2] = {
	    regs->spishaderpgmrsrc1gs, regs->spishaderpgmrsrc2gs};
	setpersistentregisterrange(
	    cmd, R_00B228_SPI_SHADER_PGM_RSRC1_GS, rsrc, 2
	);

	// VGT_STRMOUT_CONFIG
	setcontextregister(
	    cmd, R_028B94_VGT_STRMOUT_CONFIG, regs->vgtstrmoutconfig
	);
	// VGT_GS_OUT_PRIM_TYPE
	setcontextregister(
	    cmd, R_028A6C_VGT_GS_OUT_PRIM_TYPE, regs->vgtgsoutprimtype
	);
	// VGT_GS_INSTANCE_CNT
	setcontextregister(
	    cmd, R_028B90_VGT_GS_INSTANCE_CNT, regs->vgtgsinstancecnt
	);

	// Trailing NOP (11 data dwords)
	writetrailingnop(cmd, 11);
}

void sceGnmDrawCmdSetEsShader(
    GnmCommandBuffer* cmd, const GnmEsStageRegisters* regs,
    uint32_t shadermodifier
) {
	if (!regs) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "SetEsShader: regs must not be NULL");
		return;
	}
	if (regs->spishaderpgmhies != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "SetEsShader: invalid shader address (hi != 0)"
		);
		return;
	}
	if (shadermodifier & 0xfcfffc3f) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetEsShader: invalid modifier mask 0x%x",
		    shadermodifier
		);
		return;
	}

	// SPI_SHADER_PGM_LO_ES / SPI_SHADER_PGM_HI_ES
	const uint32_t pgmlo[2] = {regs->spishaderpgmloes, 0};
	setpersistentregisterrange(cmd, R_00B320_SPI_SHADER_PGM_LO_ES, pgmlo, 2);

	// SPI_SHADER_PGM_RSRC1_ES / SPI_SHADER_PGM_RSRC2_ES
	const uint32_t var = shadermodifier == 0
				 ? regs->spishaderpgmrsrc1es
				 : (regs->spishaderpgmrsrc1es & 0xfcfffc3f) | shadermodifier;
	const uint32_t rsrc[2] = {var, regs->spishaderpgmrsrc2es};
	setpersistentregisterrange(
	    cmd, R_00B328_SPI_SHADER_PGM_RSRC1_ES, rsrc, 2
	);

	// Trailing NOP (11 data dwords)
	writetrailingnop(cmd, 11);
}

void sceGnmDrawCmdSetHsShader(
    GnmCommandBuffer* cmd, const GnmHsStageRegisters* regs,
    uint32_t lshsconfig
) {
	if (!regs) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "SetHsShader: regs must not be NULL");
		return;
	}
	if (regs->spishaderpgmhihs != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "SetHsShader: invalid shader address (hi != 0)"
		);
		return;
	}

	// SPI_SHADER_PGM_LO_HS / SPI_SHADER_PGM_HI_HS
	const uint32_t pgmlo[2] = {regs->spishaderpgmlohs, 0};
	setpersistentregisterrange(cmd, R_00B420_SPI_SHADER_PGM_LO_HS, pgmlo, 2);

	// SPI_SHADER_PGM_RSRC1_HS / SPI_SHADER_PGM_RSRC2_HS
	const uint32_t rsrc[2] = {
	    regs->spishaderpgmrsrc1hs, regs->spishaderpgmrsrc2hs};
	setpersistentregisterrange(
	    cmd, R_00B428_SPI_SHADER_PGM_RSRC1_HS, rsrc, 2
	);

	// VGT_HOS_MAX_TESS_LEVEL / VGT_HOS_MIN_TESS_LEVEL
	const uint32_t tess[2] = {
	    regs->vgthosmaxtesslevel, regs->vgthosmintesslevel};
	setcontextregisterrange(
	    cmd, R_028A18_VGT_HOS_MAX_TESS_LEVEL, tess, 2
	);

	// VGT_TF_PARAM
	setcontextregister(cmd, R_028B6C_VGT_TF_PARAM, regs->vgttfparam);

	// VGT_LS_HS_CONFIG
	setcontextregister(cmd, R_028B58_VGT_LS_HS_CONFIG, lshsconfig);

	// Trailing NOP (11 data dwords)
	writetrailingnop(cmd, 11);
}

void sceGnmDrawCmdSetLsShader(
    GnmCommandBuffer* cmd, const GnmLsStageRegisters* regs,
    uint32_t shadermodifier
) {
	if (!regs) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "SetLsShader: regs must not be NULL");
		return;
	}
	if (regs->spishaderpgmhils != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "SetLsShader: invalid shader address (hi != 0)"
		);
		return;
	}

	const uint32_t modifier_mask =
	    ((shadermodifier & 0xfffffc3f) == 0) ? 0xfffffc3f : 0xfcfffc3f;
	if (shadermodifier & modifier_mask) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetLsShader: invalid modifier mask 0x%x",
		    shadermodifier
		);
		return;
	}

	// SPI_SHADER_PGM_LO_LS / SPI_SHADER_PGM_HI_LS
	const uint32_t pgmlo[2] = {regs->spishaderpgmlols, 0};
	setpersistentregisterrange(cmd, R_00B520_SPI_SHADER_PGM_LO_LS, pgmlo, 2);

	// SPI_SHADER_PGM_RSRC2_LS (written before RSRC1, matching driver RE)
	setpersistentregister(
	    cmd, R_00B52C_SPI_SHADER_PGM_RSRC2_LS, regs->spishaderpgmrsrc2ls
	);

	// SPI_SHADER_PGM_RSRC1_LS / SPI_SHADER_PGM_RSRC2_LS
	const uint32_t var = shadermodifier == 0
				 ? regs->spishaderpgmrsrc1ls
				 : (regs->spishaderpgmrsrc1ls & modifier_mask) | shadermodifier;
	const uint32_t rsrc[2] = {var, regs->spishaderpgmrsrc2ls};
	setpersistentregisterrange(
	    cmd, R_00B528_SPI_SHADER_PGM_RSRC1_LS, rsrc, 2
	);

	// Trailing NOP (11 data dwords)
	writetrailingnop(cmd, 11);
}

void sceGnmDrawCmdDispatchDirect(
    GnmCommandBuffer* cmd, uint32_t threadsx, uint32_t threadsy,
    uint32_t threadsz, uint32_t flags
) {
	const uint32_t cmddwords = 9;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	if ((int32_t)(threadsx | threadsy | threadsz) < 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "DispatchDirect: thread counts must not be negative"
		);
		return;
	}

	// PM4 header: DISPATCH_DIRECT, count=4, compute engine
	const uint32_t predicate = (flags & 1) ? 1 : 0;
	cmd->cmdptr[0] = PKT3(PKT3_DISPATCH_DIRECT, 4, predicate) |
			 PKT3_SHADER_TYPE_S(1);  // compute engine
	cmd->cmdptr[1] = threadsx;
	cmd->cmdptr[2] = threadsy;
	cmd->cmdptr[3] = threadsz;
	cmd->cmdptr[4] = (flags & 0x18) + 1;  // ordered append mode

	// Trailing NOP (3 data dwords)
	cmd->cmdptr[5] = PKT3(PKT3_NOP, 2, 0);
	cmd->cmdptr[6] = 0;
	cmd->cmdptr[7] = 0;
	cmd->cmdptr[8] = 0;

	cmd->cmdptr += cmddwords;
}

void sceGnmDrawCmdDispatchIndirect(
    GnmCommandBuffer* cmd, uint32_t dataoffset, uint32_t flags
) {
	const uint32_t cmddwords = 7;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	// PM4 header: DISPATCH_INDIRECT, count=2, compute engine
	const uint32_t predicate = (flags & 1) ? 1 : 0;
	cmd->cmdptr[0] = PKT3(PKT3_DISPATCH_INDIRECT, 2, predicate) |
			 PKT3_SHADER_TYPE_S(1);  // compute engine
	cmd->cmdptr[1] = dataoffset;
	cmd->cmdptr[2] = (flags & 0x18) + 1;  // ordered append mode

	// Trailing NOP (3 data dwords)
	cmd->cmdptr[3] = PKT3(PKT3_NOP, 2, 0);
	cmd->cmdptr[4] = 0;
	cmd->cmdptr[5] = 0;
	cmd->cmdptr[6] = 0;

	cmd->cmdptr += cmddwords;
}

void sceGnmDrawCmdDrawIndexOffset(
    GnmCommandBuffer* cmd, uint32_t indexoffset, uint32_t indexcount,
    GnmDrawModifier modifier
) {
	const uint32_t cmddwords = 9;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	const SceGnmDrawFlags flags = {
	    .predication = cmd->flags.predication_enabled,
	    .rendertargetsliceoffset = modifier.rendertargetsliceoffset,
	};
	const uint32_t rawflags =
	    (flags.predication ? 1u : 0u) |
	    (flags.rendertargetsliceoffset << 29);
	int32_t res = sceGnmDrawIndexOffset(
	    cmd->cmdptr, cmddwords, indexoffset, indexcount, rawflags
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += cmddwords;
	}
}

void sceGnmDrawCmdSetPsInputUsage(
    GnmCommandBuffer* cmd, const GnmVertexExportSemantic* vstable,
    uint32_t numvstableitems, const GnmPixelInputSemantic* pstable,
    uint32_t numpstableitems
) {
	if (numvstableitems > 0 && !vstable) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "if numvstableitems is larger than 0, vstable must not be "
		    "NULL"
		);
		return;
	}
	if (numpstableitems > 0 && !pstable) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "if numpstableitems is larger than 0, pstable must not be "
		    "NULL"
		);
		return;
	}
	if (numpstableitems > 32) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "numpstableitems must not be larger than 32"
		);
		return;
	}

	uint32_t inputs[32] = {0};

	for (uint32_t i = 0; i < numpstableitems; i += 1) {
		const GnmPixelInputSemantic* psitem = &pstable[i];

		const GnmVertexExportSemantic* matchvsitem = 0;
		for (uint32_t y = 0; y < numvstableitems; y += 1) {
			if (vstable[y].semantic == psitem->semantic) {
				matchvsitem = &vstable[y];
				break;
			}
		}

		uint32_t newitem = 0;

		uint8_t usedbitsf16 = 0;
		if (matchvsitem) {
			newitem |= S_028644_OFFSET(matchvsitem->outindex);
			if (psitem->iscustom) {
				newitem |= S_028644_OFFSET(0x20);
			}
			usedbitsf16 =
			    psitem->interpf16 & matchvsitem->exportf16;
		} else {
			newitem |= S_028644_OFFSET(0x20);
		}

		newitem |= S_028644_DEFAULT_VAL(psitem->defaultvalue);

		if (psitem->isflatshaded || psitem->iscustom) {
			newitem |= S_028644_FLAT_SHADE(1);
		}

		if (sceGnmGpuMode() == GNM_GPU_NEO && psitem->interpf16) {
			newitem |= S_028644_FP16_INTERP_MODE(1);

			if (psitem->interpf16 & 0x1) {
				newitem |= S_028644_ATTR0_VALID(0);
			}
			if (psitem->interpf16 & 0x2) {
				newitem |= S_028644_ATTR1_VALID(1);
			}
			if ((~usedbitsf16) & 0x3) {
				newitem |= S_028644_OFFSET(0x20);
			}
			if (!(usedbitsf16 & 0x1)) {
				newitem |=
				    S_028644_DEFAULT_VAL(psitem->defaultvalue);
			}
			if (!(usedbitsf16 & 0x2)) {
				newitem |= S_028644_DEFAULT_VAL_ATTR1(
				    psitem->defaultvaluehi
				);
			}
		}

		inputs[i] = newitem;
	}

	setcontextregisterrange(
	    cmd, R_028644_SPI_PS_INPUT_CNTL_0, inputs, numpstableitems
	);
}

static inline uint32_t getuserdataslot(
    GnmShaderStage stage, uint32_t startuserdataslot
) {
	static const uint32_t stagebases[GNM_NUM_SHADER_STAGES] = {
	    R_00B900_COMPUTE_USER_DATA_0,
	    R_00B030_SPI_SHADER_USER_DATA_PS_0,
	    R_00B130_SPI_SHADER_USER_DATA_VS_0,
	    R_00B230_SPI_SHADER_USER_DATA_GS_0,
	    R_00B330_SPI_SHADER_USER_DATA_ES_0,
	    R_00B430_SPI_SHADER_USER_DATA_HS_0,
	    R_00B530_SPI_SHADER_USER_DATA_LS_0,
	};
	static const uint32_t stageends[GNM_NUM_SHADER_STAGES] = {
	    R_00B93C_COMPUTE_USER_DATA_15,
	    R_00B06C_SPI_SHADER_USER_DATA_PS_15,
	    R_00B16C_SPI_SHADER_USER_DATA_VS_15,
	    R_00B26C_SPI_SHADER_USER_DATA_GS_15,
	    R_00B36C_SPI_SHADER_USER_DATA_ES_15,
	    R_00B46C_SPI_SHADER_USER_DATA_HS_15,
	    R_00B56C_SPI_SHADER_USER_DATA_LS_15,
	};

	if (stage < GNM_STAGE_CS || stage > GNM_STAGE_LS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "getuserdataslot: stage %i is invalid",
		    stage
		);
		return 0;
	}

	const uint32_t basereg = stagebases[stage];
	const uint32_t endreg = stageends[stage];
	const uint32_t maxregs = endreg - basereg + 4;
	if (startuserdataslot >= maxregs) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "getuserdataslot: startuserdataslot %u for stage %i must "
		    "be between 0 and %u",
		    startuserdataslot, stage, maxregs
		);
		return 0;
	}

	return basereg + (startuserdataslot << 2);
}

void sceGnmDrawCmdSetVsharpUserData(
    GnmCommandBuffer* cmd, GnmShaderStage stage, uint32_t startuserdataslot,
    const GnmBuffer* buf
) {
	if (startuserdataslot > GNM_MAX_VSHARP_USERDATA_SLOTS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetVsharpUserData: slot %i is too large",
		    startuserdataslot
		);
		return;
	}
	if (!buf) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetVsharpUserData: buf must not be null"
		);
		return;
	}

	const uint32_t targetreg = getuserdataslot(stage, startuserdataslot);
	if (!targetreg) {
		return;
	}

	if (!cmdcanfit(cmd, 2)) {
		return;
	}
	cmd->cmdptr[0] = PKT3(PKT3_NOP, 0, 0);
	cmd->cmdptr[1] = GPU_OPHINT_SET_VSHARP_USERDATA;
	cmd->cmdptr += 2;

	const uint32_t texdwords = sizeof(GnmBuffer) / sizeof(uint32_t);
	setpersistentregisterrange(
	    cmd, targetreg, (const uint32_t*)buf, texdwords
	);
}

void sceGnmDrawCmdSetTsharpUserData(
    GnmCommandBuffer* cmd, GnmShaderStage stage, uint32_t startuserdataslot,
    const GnmTexture* tex
) {
	if (startuserdataslot > GNM_MAX_TSHARP_USERDATA_SLOTS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetTsharpUserData: slot %i is too large",
		    startuserdataslot
		);
		return;
	}
	if (!tex) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetTsharpUserData: tex must not be null"
		);
		return;
	}
	if (sceGnmGpuMode() == GNM_GPU_BASE && tex->metadataaddr) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "SetTsharpUserData: on Base mode, metadataaddr must be null"
		);
		return;
	}

	const uint32_t targetreg = getuserdataslot(stage, startuserdataslot);
	if (!targetreg) {
		return;
	}

	if (!cmdcanfit(cmd, 2)) {
		return;
	}
	cmd->cmdptr[0] = PKT3(PKT3_NOP, 0, 0);
	cmd->cmdptr[1] = GPU_OPHINT_SET_TSHARP_USERDATA;
	cmd->cmdptr += 2;

	const uint32_t texdwords = sizeof(GnmTexture) / sizeof(uint32_t);
	setpersistentregisterrange(
	    cmd, targetreg, (const uint32_t*)tex, texdwords
	);
}

void sceGnmDrawCmdSetSsharpUserData(
    GnmCommandBuffer* cmd, GnmShaderStage stage, uint32_t startuserdataslot,
    const GnmSampler* sampler
) {
	if (startuserdataslot > GNM_MAX_SSHARP_USERDATA_SLOTS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetSsharpUserData: slot %i is too large",
		    startuserdataslot
		);
		return;
	}
	if (!sampler) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "SetSsharpUserData: sampler must not be null"
		);
		return;
	}

	const uint32_t targetreg = getuserdataslot(stage, startuserdataslot);
	if (!targetreg) {
		return;
	}

	if (!cmdcanfit(cmd, 2)) {
		return;
	}
	cmd->cmdptr[0] = PKT3(PKT3_NOP, 0, 0);
	cmd->cmdptr[1] = GPU_OPHINT_SET_SSHARP_USERDATA;
	cmd->cmdptr += 2;

	const uint32_t sampdwords = sizeof(GnmSampler) / sizeof(uint32_t);
	setpersistentregisterrange(
	    cmd, targetreg, (const uint32_t*)sampler, sampdwords
	);
}

void sceGnmDrawCmdSetPointerUserData(
    GnmCommandBuffer* cmd, GnmShaderStage stage, uint32_t startuserdataslot,
    void* ptr
) {
	if (startuserdataslot > GNM_MAX_POINTER_USERDATA_SLOTS) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetPointerUserData: slot %i is too large",
		    startuserdataslot
		);
		return;
	}

	const uint32_t targetreg = getuserdataslot(stage, startuserdataslot);
	if (!targetreg) {
		return;
	}

	if (!cmdcanfit(cmd, 2)) {
		return;
	}

	const uint32_t ptrdwords = sizeof(ptr) / sizeof(uint32_t);
	setpersistentregisterrange(
	    cmd, targetreg, (const uint32_t*)&ptr, ptrdwords
	);
}

void sceGnmDrawCmdSetBlendControl(
    GnmCommandBuffer* cmd, uint32_t rtindex, const GnmBlendControl* ctrl
) {
	if (!ctrl) {
		sceGnmWriteMsgf(GNM_MSGSEV_ERR, "SetBlendControl: ctrl is null");
		return;
	}
	if (rtindex > 7) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetBlendControl: rtindex is too large"
		);
		return;
	}

	const uint32_t ctrlflags =
	    S_028780_COLOR_SRCBLEND(ctrl->colorsrcmult) |
	    S_028780_COLOR_COMB_FCN(ctrl->colorfunc) |
	    S_028780_COLOR_DESTBLEND(ctrl->colordstmult) |
	    S_028780_ALPHA_SRCBLEND(ctrl->alphasrcmult) |
	    S_028780_ALPHA_COMB_FCN(ctrl->alphafunc) |
	    S_028780_ALPHA_DESTBLEND(ctrl->alphadstmult) |
	    S_028780_SEPARATE_ALPHA_BLEND(ctrl->separatealphaenable) |
	    S_028780_ENABLE(ctrl->blendenabled);
	setcontextregister(
	    cmd, R_028780_CB_BLEND0_CONTROL + rtindex, ctrlflags
	);
}

void sceGnmDrawCmdSetBlendColor(
    GnmCommandBuffer* cmd, float red, float green, float blue, float alpha
) {
	const float values[4] = {red, green, blue, alpha};
	uint32_t words[4];
	memcpy(words, values, sizeof(words));
	setcontextregisterrange(cmd, R_028414_CB_BLEND_RED, words, 4);
}

void sceGnmDrawCmdSetDepthStencilControl(
    GnmCommandBuffer* cmd, const GnmDepthStencilControl* ctrl
) {
	if (!ctrl) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetDepthStencilControl: ctrl is null"
		);
		return;
	}

	// ENABLE_COLOR_WRITES_ON_DEPTH_FAIL (bit 30) and
	// DISABLE_COLOR_WRITES_ON_DEPTH_PASS (bit 31) are not exposed in
	// GnmDepthStencilControl — they are niche features not used by
	// typical renderers. Add fields to the struct if needed.
	const uint32_t ctrlflags =
	    S_028800_STENCIL_ENABLE(ctrl->stencilenable) |
	    S_028800_Z_ENABLE(ctrl->depthenable) |
	    S_028800_Z_WRITE_ENABLE(ctrl->zwrite) |
	    S_028800_DEPTH_BOUNDS_ENABLE(ctrl->depthboundsenable) |
	    S_028800_ZFUNC(ctrl->zfunc) |
	    S_028800_BACKFACE_ENABLE(ctrl->separatestencilenable) |
	    S_028800_STENCILFUNC(ctrl->stencilfunc) |
	    S_028800_STENCILFUNC_BF(ctrl->stencilbackfunc);
	setcontextregister(cmd, R_028800_DB_DEPTH_CONTROL, ctrlflags);
}

void sceGnmDrawCmdSetDbRenderControl(
    GnmCommandBuffer* cmd, const GnmDbRenderControl* ctrl
) {
	if (!ctrl) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "SetDbRenderControl: ctrl is null"
		);
		return;
	}
	if (ctrl->copysampleindex > 15) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "SetDbRenderControl: copysampleindex must be less than 15"
		);
		return;
	}

	// DEPTH_COPY (bit 2) and STENCIL_COPY (bit 3) are not exposed in
	// GnmDbRenderControl — they conflict with clear/resummarize usage
	// patterns and are not needed by typical renderers.
	const uint32_t ctrlflags =
	    S_028000_DEPTH_CLEAR_ENABLE(ctrl->depthclearenable) |
	    S_028000_STENCIL_CLEAR_ENABLE(ctrl->stencilclearenable) |
	    S_028000_RESUMMARIZE_ENABLE(ctrl->htileresummarizeenable) |
	    S_028000_STENCIL_COMPRESS_DISABLE(ctrl->stencilwritebackpol) |
	    S_028000_DEPTH_COMPRESS_DISABLE(ctrl->depthwritebackpol) |
	    S_028000_COPY_CENTROID(ctrl->copycentroidenable) |
	    S_028000_COPY_SAMPLE(ctrl->copysampleindex) |
	    S_028000_DECOMPRESS_ENABLE(ctrl->forcedepthdecompress);
	setcontextregister(cmd, R_028000_DB_RENDER_CONTROL, ctrlflags);
}

void sceGnmDrawCmdSetPrimitiveSetup(
    GnmCommandBuffer* cmd, const GnmPrimitiveSetup* ctrl
) {
	if (!ctrl) {
		sceGnmWriteMsgf(GNM_MSGSEV_ERR, "SetPrimitiveSetup: ctrl is null");
		return;
	}

	// Unexposed fields: POLY_OFFSET_PARA_ENABLE (bit 13),
	// MULTI_PRIM_IB_ENA (bit 21) — niche features not in GnmPrimitiveSetup.
	const uint32_t ctrlflags =
	    S_028814_CULL_FRONT(ctrl->cullmode) |
	    S_028814_CULL_BACK(
		(ctrl->cullmode & GNM_CULL_BACK) == GNM_CULL_BACK
	    ) |
	    S_028814_FACE(ctrl->frontface) |
	    S_028814_POLY_MODE(
		ctrl->frontmode != GNM_FILL_SOLID ||
		ctrl->backmode != GNM_FILL_SOLID
	    ) |
	    S_028814_POLYMODE_FRONT_PTYPE(ctrl->frontmode) |
	    S_028814_POLYMODE_BACK_PTYPE(ctrl->backmode) |
	    S_028814_POLY_OFFSET_FRONT_ENABLE(ctrl->frontoffsetmode) |
	    S_028814_POLY_OFFSET_BACK_ENABLE(ctrl->backoffsetmode) |
	    S_028814_VTX_WINDOW_OFFSET_ENABLE(ctrl->vertexwindowoffsetenable) |
	    S_028814_PROVOKING_VTX_LAST(ctrl->provokemode) |
	    S_028814_PERSP_CORR_DIS(ctrl->perspectivecorrectiondisable);
	setcontextregister(cmd, R_028814_PA_SU_SC_MODE_CNTL, ctrlflags);
}

void sceGnmDrawCmdSetViewportTransformControl(
    GnmCommandBuffer* cmd, const GnmViewportTransformControl* ctrl
) {
	const uint32_t cmddwords = 2 + 1;
	if (!cmdcanfit(cmd, cmddwords)) {
		return;
	}

	// PERFCOUNTER_REF (bit 11) not exposed — performance counter
	// reference mode is a profiling feature not in GnmViewportTransformControl.
	const uint32_t ctrlflags =
	    S_028818_VPORT_X_SCALE_ENA(ctrl->scalex) |
	    S_028818_VPORT_X_OFFSET_ENA(ctrl->offsetx) |
	    S_028818_VPORT_Y_SCALE_ENA(ctrl->scaley) |
	    S_028818_VPORT_Y_OFFSET_ENA(ctrl->offsety) |
	    S_028818_VPORT_Z_SCALE_ENA(ctrl->scalez) |
	    S_028818_VPORT_Z_OFFSET_ENA(ctrl->offsetz) |
	    S_028818_VTX_XY_FMT(ctrl->perspectivedividexy) |
	    S_028818_VTX_Z_FMT(ctrl->perspectivedividez) |
	    S_028818_VTX_W0_FMT(ctrl->invertw);
	setcontextregister(cmd, R_028818_PA_CL_VTE_CNTL, ctrlflags);
}

void sceGnmDrawCmdEventWriteEop(
    GnmCommandBuffer* cmd, GnmEventType event, uint64_t gpuaddr,
    GnmEventDataSel datasel, uint64_t immvalue
) {
	if (!cmdcanfit(cmd, 6)) {
		return;
	}

	if (!gpuaddr) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "gpuaddr must not be null");
		return;
	}

	if (event > 0x3f) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "EventWriteEop: event type %u exceeds the 6-bit PM4 field",
		    event
		);
		return;
	}

	if (datasel > GNM_DATA_SEL_SEND_GPU_CLOCK) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "EventWriteEop: data select %u is not supported", datasel
		);
		return;
	}

	const uint32_t highaddr = gpuaddr >> 32;

	// Only 16 bits are available for the high address, more than 16 bits is
	// not supported
	if ((highaddr >> 16) != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "High 16 bits of gpuaddr must be 0"
		);
		return;
	}

	// 32-bit data must be DWORD aligned.
	if ((datasel == GNM_DATA_SEL_SEND_DATA32) &&
	    !ispow2aligned(gpuaddr, 4)) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "Address for 32 bit data must be 4 bytes aligned"
		);
		return;
	}

	// 64-bit data must be QWORD aligned.
	if ((datasel == GNM_DATA_SEL_SEND_DATA64 ||
	     datasel == GNM_DATA_SEL_SEND_SYS_CLOCK ||
	     datasel == GNM_DATA_SEL_SEND_GPU_CLOCK) &&
	    !ispow2aligned(gpuaddr, 8)) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "Address for 64 bit data must be 8 bytes aligned"
		);
		return;
	}

	uint32_t sel = EOP_DST_SEL(EOP_DST_SEL_MEM) | EOP_DATA_SEL(datasel);
	if (datasel != GNM_DATA_SEL_DISCARD)
		sel |= EOP_INT_SEL(EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM);

	cmd->cmdptr[0] = PKT3(PKT3_EVENT_WRITE_EOP, 4, 0);
	cmd->cmdptr[1] =
	    EVENT_TYPE(event) |
	    EVENT_INDEX(event == GNM_CS_DONE || event == GNM_PS_DONE ? 6 : 5);
	cmd->cmdptr[2] = gpuaddr & 0xffffffff;
	cmd->cmdptr[3] = (highaddr & 0xffff) | sel;
	cmd->cmdptr[4] = immvalue & 0xffffffff;
	cmd->cmdptr[5] = (immvalue >> 32) & 0xffffffff;
	cmd->cmdptr += 6;
}

bool sceGnmDrawCmdFillMemory(
    GnmCommandBuffer* cmd, uint64_t gpuaddr, uint32_t sizebytes,
    uint32_t value
) {
	const uint32_t maxchunksize = 0x1ffffc;
	if (!cmd || !gpuaddr || !sizebytes || (gpuaddr & 3) || (sizebytes & 3)) {
		return false;
	}
	const uint32_t packetcount = (sizebytes + maxchunksize - 1) / maxchunksize;
	if (!cmdcanfit(cmd, packetcount * 7)) {
		return false;
	}

	while (sizebytes) {
		const uint32_t chunksize = sizebytes > maxchunksize ? maxchunksize : sizebytes;
		cmd->cmdptr[0] = PKT3(PKT3_DMA_DATA, 5, 0);
		cmd->cmdptr[1] = S_500_SRC_SEL(V_500_DATA) |
		                 S_500_DST_SEL(V_500_DST_ADDR) | S_500_CP_SYNC(1);
		cmd->cmdptr[2] = value;
		cmd->cmdptr[3] = 0;
		cmd->cmdptr[4] = (uint32_t)gpuaddr;
		cmd->cmdptr[5] = (uint32_t)(gpuaddr >> 32);
		cmd->cmdptr[6] = S_415_BYTE_COUNT_GFX6(chunksize);
		cmd->cmdptr += 7;
		gpuaddr += chunksize;
		sizebytes -= chunksize;
	}
	return true;
}

bool sceGnmDrawCmdCopyMemory(
    GnmCommandBuffer* cmd, uint64_t dstaddr, uint64_t srcaddr,
    uint32_t sizebytes
) {
	const uint32_t maxchunksize = 0x1ffffc;
	if (!cmd || !dstaddr || !srcaddr || !sizebytes || (dstaddr & 3) ||
	    (srcaddr & 3) || (sizebytes & 3)) {
		return false;
	}
	const uint32_t packetcount = (sizebytes + maxchunksize - 1) / maxchunksize;
	if (!cmdcanfit(cmd, packetcount * 7)) {
		return false;
	}
	while (sizebytes) {
		const uint32_t chunksize = sizebytes > maxchunksize ? maxchunksize : sizebytes;
		cmd->cmdptr[0] = PKT3(PKT3_DMA_DATA, 5, 0);
		cmd->cmdptr[1] = S_500_SRC_SEL(V_500_SRC_ADDR) |
		                 S_500_DST_SEL(V_500_DST_ADDR) | S_500_CP_SYNC(1);
		cmd->cmdptr[2] = (uint32_t)srcaddr;
		cmd->cmdptr[3] = (uint32_t)(srcaddr >> 32);
		cmd->cmdptr[4] = (uint32_t)dstaddr;
		cmd->cmdptr[5] = (uint32_t)(dstaddr >> 32);
		cmd->cmdptr[6] = S_415_BYTE_COUNT_GFX6(chunksize);
		cmd->cmdptr += 7;
		srcaddr += chunksize;
		dstaddr += chunksize;
		sizebytes -= chunksize;
	}
	return true;
}

void sceGnmDrawCmdWaitGraphicsWrite(
    GnmCommandBuffer* cmd, GnmAcquireTargetFlags targets
) {
	if (!cmdcanfit(cmd, 7)) {
		return;
	}

	uint32_t cpcoherctrl = targets;
	if (targets & 0x00003fc0) {
		cpcoherctrl |= S_0301F0_CB_ACTION_ENA(1);
	}
	if (targets & 0x00004000) {
		cpcoherctrl |= S_0301F0_DB_ACTION_ENA(1);
	}

	cmd->cmdptr[0] = PKT3(PKT3_ACQUIRE_MEM, 5, 0);
	cmd->cmdptr[1] = cpcoherctrl | S_0301F0_TCL1_VOL_ACTION_ENA(1) |
			 S_0301F0_TC_VOL_ACTION_ENA(1) |
			 S_0301F0_TC_WB_ACTION_ENA(1);
	cmd->cmdptr[2] = 0xffffffff;
	cmd->cmdptr[3] = 0x000000ff;
	cmd->cmdptr[4] = 0;
	cmd->cmdptr[5] = 0;
	cmd->cmdptr[6] = 0xa;  // poll interval
	cmd->cmdptr += 7;
}

void sceGnmDrawCmdWaitMem(
    GnmCommandBuffer* cmd, GnmWaitRegMemFunc op, uint64_t gpuaddr,
    uint32_t refval, uint32_t mask
) {
	if (!cmdcanfit(cmd, 7)) {
		return;
	}

	if (!gpuaddr) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "gpuaddr must not be null");
		return;
	}
	if (op > GNM_WAIT_REG_MEM_FUNC_GREATER) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "WaitMem: compare function %u exceeds supported range", op
		);
		return;
	}
	const uint32_t highaddr = gpuaddr >> 32;
	if ((highaddr >> 16) != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "WaitMem: high 16 bits of gpuaddr must be 0"
		);
		return;
	}

	cmd->cmdptr[0] = PKT3(PKT3_WAIT_REG_MEM, 5, 0);
	cmd->cmdptr[1] = op | WAIT_REG_MEM_MEM_SPACE(1);
	cmd->cmdptr[2] = gpuaddr & 0xffffffff;
	cmd->cmdptr[3] = highaddr;
	cmd->cmdptr[4] = refval;
	cmd->cmdptr[5] = mask;
	cmd->cmdptr[6] = 4;  // poll interval
	cmd->cmdptr += 7;
}

void sceGnmDrawCmdWaitUntilSafeForRendering(
    GnmCommandBuffer* cmd, int32_t videohandle, uint32_t displaybufidx
) {
	if (!cmdcanfit(cmd, 7)) {
		return;
	}

	int32_t res = sceGnmDriverInsertWaitFlipDone(
	    cmd->cmdptr, 7, videohandle, displaybufidx
	);
	if (res == GNM_ERROR_OK) {
		cmd->cmdptr += 7;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "WaitUntilSafeForRendering: driver call failed with 0x%x",
		    res
		);
	}
}

/* ==================== Transform Feedback (Stream-Out) ==================== */

void sceGnmDrawCmdSetStreamOutConfig(
    GnmCommandBuffer* cmd, uint32_t streamen, uint32_t raststream,
    uint32_t bufferen
) {
	if (streamen > 0xF) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "StreamOutConfig: streamen 0x%x exceeds 4 bits", streamen
		);
		return;
	}
	if (raststream > 7) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "StreamOutConfig: raststream %u exceeds 3 bits",
		    raststream
		);
		return;
	}
	if (bufferen > 0xffff) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "StreamOutConfig: bufferen 0x%x exceeds 16 bits", bufferen
		);
		return;
	}

	const uint32_t config =
	    (streamen & 0xF) |
	    S_028B94_RAST_STREAM(raststream);
	setcontextregister(cmd, R_028B94_VGT_STRMOUT_CONFIG, config);
	setcontextregister(
	    cmd, R_028B98_VGT_STRMOUT_BUFFER_CONFIG, bufferen
	);
}

void sceGnmDrawCmdSetStreamOutBuffer(
    GnmCommandBuffer* cmd, uint32_t slot, uint64_t gpuaddr, uint32_t size,
    uint32_t stride
) {
	(void)gpuaddr;
	if (slot > 3) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "StreamOutBuffer: slot %u exceeds 3", slot
		);
		return;
	}
	if (stride > 0x3FF) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR,
		    "StreamOutBuffer: stride %u exceeds 10 bits", stride
		);
	}

	/* Each buffer slot has 3 context regs at fixed offsets:
	 * SIZE_0 + slot*4, VTX_STRIDE_0 + slot*4, BUFFER_OFFSET_0 + slot*4
	 * Note: there's a gap (BUFFER_OFFSET is +8 from SIZE, not +4)
	 */
	const uint32_t sizereg =
	    R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + slot * 4;
	const uint32_t stridereg =
	    R_028AD4_VGT_STRMOUT_VTX_STRIDE_0 + slot * 4;
	const uint32_t offsetreg =
	    R_028ADC_VGT_STRMOUT_BUFFER_OFFSET_0 + slot * 4;

	/* SIZE register: buffer size in bytes (stored as-is on gfx8) */
	setcontextregister(cmd, sizereg, size);
	/* VTX_STRIDE register: stride in bytes (10-bit field) */
	setcontextregister(
	    cmd, stridereg, S_028AD4_STRIDE(stride)
	);
	/* BUFFER_OFFSET: start offset (0 = begin at start of buffer) */
	setcontextregister(cmd, offsetreg, 0);
}

/* ==================== Occlusion Queries ==================== */

enum {
	GNM_PM4_EVENT_TYPE_ZPASS_DONE = 0x15,
};

/* ZPASS_DONE occlusion query uses EVENT_WRITE_EOP with:
 * - event_type = 0x15 (ZPASS_DONE on gfx8)
 * - event_index = 1 (ZPASS_DONE index per AMD spec)
 * - data_sel = SEND_DATA32 (write 32-bit count to gpuaddr)
 *
 * BeginQuery:  enable DB_COUNT_CONTROL counter
 * EndQuery:    emit ZPASS_DONE EOP event to write count to gpuaddr
 * ResetQuery:  emit ZPASS_DONE EOP event with data=0 to clear count
 */

static void writeZpassDoneEop(
    GnmCommandBuffer* cmd, uint64_t gpuaddr, uint32_t data
) {
	if (!cmdcanfit(cmd, 6)) {
		return;
	}
	if (!gpuaddr) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "Query: gpuaddr must not be null");
		return;
	}
	if (!ispow2aligned(gpuaddr, 4)) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "Query: gpuaddr must be 4-byte aligned"
		);
		return;
	}

	const uint32_t highaddr = gpuaddr >> 32;
	if ((highaddr >> 16) != 0) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "Query: high 16 bits of gpuaddr must be 0"
		);
		return;
	}

	const uint32_t sel = EOP_DST_SEL(EOP_DST_SEL_MEM) |
	                     EOP_DATA_SEL(GNM_DATA_SEL_SEND_DATA32) |
	                     EOP_INT_SEL(EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM);

	cmd->cmdptr[0] = PKT3(PKT3_EVENT_WRITE_EOP, 4, 0);
	cmd->cmdptr[1] =
	    EVENT_TYPE(GNM_PM4_EVENT_TYPE_ZPASS_DONE) | EVENT_INDEX(1);
	cmd->cmdptr[2] = gpuaddr & 0xffffffff;
	cmd->cmdptr[3] = (highaddr & 0xffff) | sel;
	cmd->cmdptr[4] = data;
	cmd->cmdptr[5] = 0;
	cmd->cmdptr += 6;
}

void sceGnmDrawCmdResetQuery(GnmCommandBuffer* cmd, uint64_t gpuaddr) {
	/* Reset: write 0 to the query address via ZPASS_DONE event */
	writeZpassDoneEop(cmd, gpuaddr, 0);
}

void sceGnmDrawCmdBeginQuery(GnmCommandBuffer* cmd, uint64_t gpuaddr) {
	(void)gpuaddr;
	/* Enable occlusion counter in DB_COUNT_CONTROL.
	 * ZPASS_ENABLE bit 8 = slot 0 enable, PERFECT_ZPASS_COUNTS bit 1
	 * for accurate counting.
	 */
	const uint32_t ctrl =
	    S_028004_PERFECT_ZPASS_COUNTS(1) |
	    S_028004_ZPASS_ENABLE(1);	/* enable slot 0 */
	setcontextregister(cmd, R_028004_DB_COUNT_CONTROL, ctrl);
}

void sceGnmDrawCmdEndQuery(GnmCommandBuffer* cmd, uint64_t gpuaddr) {
	/* Emit ZPASS_DONE EOP event to write occlusion count to gpuaddr */
	writeZpassDoneEop(cmd, gpuaddr, 0);

	/* Disable occlusion counter */
	setcontextregister(cmd, R_028004_DB_COUNT_CONTROL, 0);
}
