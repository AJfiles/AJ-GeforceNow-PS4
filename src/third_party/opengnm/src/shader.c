#include "gnm_shader.h"

#include <stdbool.h>
#include <string.h>

#include "gcn/gcn.h"
#include "platform.h"
#include "pm4/amdgfxregs.h"

#include "u/utility.h"

static inline uint32_t getfirstfreesgpr(const GnmVsStageRegisters* regs) {
	const uint32_t rsrc2 = regs->spishaderpgmrsrc2vs;
	return G_00B12C_SCRATCH_EN(rsrc2) + G_00B12C_USER_SGPR(rsrc2) +
	       G_00B12C_OC_LDS_EN(rsrc2) + G_00B12C_SO_BASE0_EN(rsrc2) +
	       G_00B12C_SO_BASE1_EN(rsrc2) + G_00B12C_SO_BASE2_EN(rsrc2) +
	       G_00B12C_SO_BASE3_EN(rsrc2) + G_00B12C_SO_EN(rsrc2);
}

GnmError sceGnmFetchShaderCalcSize(
    uint32_t* outsize, const GnmFetchShaderCreateInfo* ci
) {
	if (!outsize || !ci) {
		return GNM_ERROR_INVALID_ARGS;
	}

	const uint32_t firstfreesgpr = getfirstfreesgpr(ci->regs);
	const uint32_t sgprs = G_00B128_SGPRS(ci->regs->spishaderpgmrsrc1vs);
	const uint32_t maxsgprlimit =
	    sgprs * GNM_NUM_SHADER_STAGES + GNM_NUM_SHADER_STAGES;

	// 104 is the max number of SGPRs
	if (maxsgprlimit > 104) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (firstfreesgpr > 100 || firstfreesgpr > maxsgprlimit) {
		return GNM_ERROR_INVALID_ARGS;
	}

	if (ci->vertexbaseusgpr >= 16) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (ci->instancebaseusgpr >= 16) {
		return GNM_ERROR_INVALID_ARGS;
	}

	const uint32_t alignedfirstfreesgpr = (firstfreesgpr + 3) & (-4);
	const uint32_t sgproffset = maxsgprlimit - alignedfirstfreesgpr;
	const uint32_t numbufferslots =
	    (sgproffset >> 2) + 2 * (sgproffset == 0);

	uint32_t totaldwords = 0;

	// vertex and instance bases v_add_i32 instructions
	if (ci->vertexbaseusgpr != 0) {
		totaldwords += 1;
	}
	if (ci->instancebaseusgpr != 0) {
		totaldwords += 1;
	}

	// s_load_dwordx4 for each GnmBuffer
	totaldwords += (ci->numvtxinputs + numbufferslots - 1) / numbufferslots;

	// s_waitcnt for each input's all s_load_dwordx4
	totaldwords += ci->numvtxinputs;

	// buffer_load_format_* for each attribute
	// these are 2 DWORDS long each
	totaldwords += ci->numvtxinputs * 2;

	// s_waitcnt for all buffer_load_format_*
	totaldwords += 1;

	// ending s_setpc_b64
	totaldwords += 1;

	// input count
	totaldwords += 1;

	*outsize = totaldwords * sizeof(uint32_t);
	return GNM_ERROR_OK;
}

typedef struct {
	uint8_t* buf;
	uint32_t size;
	uint32_t off;
} WriterContext;

static GcnError fetchshwrite(
    const void* data, uint32_t datasize, void* userdata
) {
	WriterContext* w = userdata;
	if (w->off + datasize > w->size) {
		return GCN_ERR_OVERFLOW;
	}
	memcpy(&w->buf[w->off], data, datasize);
	w->off += datasize;
	return GCN_ERR_OK;
}

static inline int findremap(
    const GnmFetchShaderCreateInfo* ci, uint32_t inputidx
) {
	for (uint8_t i = 0; i < ci->remaptablecount; i += 1) {
		const uint32_t curemap = ci->remaptable[i];
		if (ci->vtxinputs[inputidx].semantic == curemap) {
			return i;
		}
	}
	return -1;
}

GnmError sceGnmCreateFetchShader(
    void* outdata, uint32_t outdatasize, const GnmFetchShaderCreateInfo* ci,
    GnmFetchShaderResults* r
) {
	if (!outdata || !outdatasize || !ci || !r) {
		return GNM_ERROR_INVALID_ARGS;
	}

	WriterContext wctx = {
	    .buf = outdata,
	    .size = outdatasize,
	};
	GcnAssembler as = {
	    .writer = fetchshwrite,
	    .writeuserdata = &wctx,
	};

	int vbtableregister = -1;
	for (uint32_t i = 0; i < ci->numinputusages; i += 1) {
		const GnmInputUsageSlot* slot = &ci->inputusages[i];

		if (slot->usagetype == GNM_SHINPUTUSAGE_PTR_VERTEXBUFFERTABLE) {
			if (vbtableregister != -1) {
				// more than one vtx buffer table isn't
				// supported
				return GNM_ERROR_UNSUPPORTED;
			}

			vbtableregister = slot->startregister;

			if (slot->registercount) {
				return GNM_ERROR_INVALID_ARGS;
			}
		} else if (slot->usagetype == GNM_SHINPUTUSAGE_IMM_VERTEXBUFFER) {
			return GNM_ERROR_UNSUPPORTED;
		}
	}

	GcnError err = GCN_ERR_OK;

	if (ci->vertexbaseusgpr) {
		if (ci->vertexbaseusgpr >= 16) {
			return GNM_ERROR_INVALID_ARGS;
		}
		// v_add_i32
		err = gcnAsVop2(
		    &as, GCN_V_ADD_I32, GCN_OPFIELD_VGPR_0,
		    GCN_OPFIELD_SGPR_0 + ci->vertexbaseusgpr, GCN_OPFIELD_VGPR_0
		);
		if (err != GCN_ERR_OK) {
			return GNM_ERROR_ASM_FAILED;
		}
	}
	if (ci->instancebaseusgpr) {
		if (ci->instancebaseusgpr >= 16) {
			return GNM_ERROR_INVALID_ARGS;
		}
		// v_add_i32
		err = gcnAsVop2(
		    &as, GCN_V_ADD_I32, GCN_OPFIELD_VGPR_3,
		    GCN_OPFIELD_SGPR_0 + ci->instancebaseusgpr,
		    GCN_OPFIELD_VGPR_3
		);
		if (err != GCN_ERR_OK) {
			return GNM_ERROR_ASM_FAILED;
		}
	}

	const uint32_t firstfreesgpr = getfirstfreesgpr(ci->regs);
	const uint32_t sgprs = G_00B128_SGPRS(ci->regs->spishaderpgmrsrc1vs);
	// GNM_NUM_SHADER_STAGES (8) is used here as a SGPR limit multiplier
	// from reversed firmware code. The value is correct; the name is
	// coincidental — it represents the SGPR allocation granularity, not
	// the number of shader stages.
	const uint32_t maxsgprlimit =
	    sgprs * GNM_NUM_SHADER_STAGES + GNM_NUM_SHADER_STAGES;
	const uint32_t alignedfirstfreesgpr = (firstfreesgpr + 3) & (-4);
	const uint32_t sgproffset = maxsgprlimit - alignedfirstfreesgpr;
	const uint32_t numbufferslots =
	    (sgproffset >> 2) + 2 * (sgproffset == 0);

	uint32_t cursem = 0;
	uint32_t curvtxidx = 0;
	while (cursem < ci->numvtxinputs) {
		for (uint32_t curbuf = 0;
		     curbuf < numbufferslots && curvtxidx < ci->numvtxinputs;
		     curbuf += 1, curvtxidx += 1) {
			int semanticidx = curvtxidx;
			if (ci->remaptable) {
				int semanticidx = findremap(ci, curvtxidx);
				if (semanticidx == -1) {
					return GNM_ERROR_SEMANTIC_NOT_FOUND;
				}
			}

			err = gcnAsSmrd(
			    &as, GCN_S_LOAD_DWORDX4,
			    GCN_OPFIELD_SGPR_0 + alignedfirstfreesgpr +
				4 * curbuf,
			    GCN_OPFIELD_SGPR_0 + vbtableregister,
			    semanticidx * 4
			);
			if (err != GCN_ERR_OK) {
				return GNM_ERROR_ASM_FAILED;
			}
		}

		// s_waitcnt lgkmcnt(0)
		err = gcnAsSopp(&as, GCN_S_WAITCNT, 0x7f);
		if (err != GCN_ERR_OK) {
			return GNM_ERROR_ASM_FAILED;
		}

		if (cursem < curvtxidx) {
			const uint32_t numremapped = curvtxidx - cursem;
			for (uint32_t i = 0; i < numremapped; i += 1) {
				GnmFetchShaderInstancingMode instmode =
				    GNM_FETCH_MODE_VERTEXINDEX;
				if (ci->instancedata) {
					if (i + cursem >= ci->numinstancedata) {
						return GNM_ERROR_INVALID_ARGS;
					} else {
						instmode = ci->instancedata
							       [i + cursem];
					}
				}
				const GnmVertexInputSemantic* inputsem =
				    &ci->vtxinputs[i + cursem];

				if (inputsem->sizeinelements < 1 ||
				    inputsem->sizeinelements > 4) {
					return GNM_ERROR_INVALID_ARGS;
				}

				uint8_t vaddr = 0;
				switch (instmode) {
				case GNM_FETCH_MODE_INSTANCEID:
					vaddr = 3;
					break;
				case GNM_FETCH_MODE_INSTANCEID_OVERSTEPRATE0:
					vaddr =
					    ci->flags &
						    GNM_FETCH_FLAG_SHIFT_OVERSTEP0
						? 2
						: 1;
					break;
				case GNM_FETCH_MODE_INSTANCEID_OVERSTEPRATE1:
					if (ci->flags &
					    GNM_FETCH_FLAG_SHIFT_OVERSTEP0) {
						return GNM_ERROR_UNSUPPORTED;
					}
					vaddr = 2;
					break;
				default:
					break;
				}

				// buffer_load_format_* (2 dwords)
				err = gcnAsMubuf(
				    &as,
				    GCN_BUFFER_LOAD_FORMAT_X +
					inputsem->sizeinelements - 1,
				    GCN_OPFIELD_VGPR_0 + inputsem->vgpr,
				    GCN_OPFIELD_VGPR_0 + vaddr,
				    GCN_OPFIELD_SGPR_0 + alignedfirstfreesgpr +
					4 * i,
				    GCN_OPFIELD_CONST_INT_0,
				    &(GcnAsMubufOptions){.idxen = true}
				);
				if (err != GCN_ERR_OK) {
					return GNM_ERROR_ASM_FAILED;
				}
			}
			cursem = curvtxidx;
		}
	}

	// s_waitcnt vmcnt(0) & expcnt(0) & lgkmcnt(0)
	err = gcnAsSopp(&as, GCN_S_WAITCNT, 0);
	if (err != GCN_ERR_OK) {
		return GNM_ERROR_ASM_FAILED;
	}
	err = gcnAsSop1(
	    &as, GCN_S_SETPC_B64, GCN_OPFIELD_SGPR_0, GCN_OPFIELD_SGPR_0
	);
	if (err != GCN_ERR_OK) {
		return GNM_ERROR_ASM_FAILED;
	}

	uint32_t writtensize = wctx.off;
	if (outdatasize - writtensize < sizeof(uint32_t)) {
		return GNM_ERROR_ASM_FAILED;
	}

	// Trailing dword: number of vertex inputs (from reversed firmware code).
	// The GPU likely uses this to determine fetch count.
	const uint32_t numinputs =
	    ci->remaptablecount ? ci->remaptablecount : ci->numvtxinputs;
	*(uint32_t*)&wctx.buf[wctx.off] = numinputs;
	writtensize += sizeof(uint32_t);

	// Add an extra SGPR when the SGPR offset is zero (from reversed code).
	// This accounts for the fetch shader's own SGPR usage when the vertex
	// shader's SGPR range is full.
	r->sgprs = (sgproffset == 0) + sgprs;
	// VGPR0-3: (VertexID, InstanceID / StepRate0, InstanceID / StepRate1,
	// InstanceID)
	r->vgprcompcnt =
	    ci->instancedata
		? 3
		: G_00B128_VGPR_COMP_CNT(ci->regs->spishaderpgmrsrc1vs);

	return GNM_ERROR_OK;
}

void sceGnmVsRegsSetFetchShaderModifier(
    GnmVsStageRegisters* regs, const GnmFetchShaderResults* r
) {
	regs->spishaderpgmrsrc1vs &= C_00B128_SGPRS;
	regs->spishaderpgmrsrc1vs |= S_00B128_SGPRS(r->sgprs);
	regs->spishaderpgmrsrc1vs &= C_00B128_VGPR_COMP_CNT;
	regs->spishaderpgmrsrc1vs |= S_00B128_VGPR_COMP_CNT(r->vgprcompcnt);
}
