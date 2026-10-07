#include "gcn.h"

#include "u/utility.h"

enum {
	NUM_SGPRS = GCN_OPFIELD_SGPR_103 - GCN_OPFIELD_SGPR_0 + 1,
};

typedef struct {
	int8_t index;
	uint16_t offset;
} SgprRef;

typedef struct {
	SgprRef lastsgpresources[NUM_SGPRS];
	uint32_t usedresources;
} Context;

static inline GcnError appendorupdate(
    Context* ctx, GcnResource* newres, const GcnInstruction* instr,
    GcnResource* outresources, uint32_t maxoutresources
) {
	const SgprRef* sgpr = &ctx->lastsgpresources[newres->index];
	if (sgpr->index != -1) {
		// find parent in outresources
		for (uint32_t i = 0; i < ctx->usedresources; i += 1) {
			const GcnResource* curp = &outresources[i];
			if (curp->index == sgpr->index) {
				newres->parentindex = sgpr->index;
				newres->parentoff = sgpr->offset;
				break;
			}
		}
		// error out if somehow we don't have it
		if (newres->parentindex == -1) {
			return GCN_ERR_INTERNAL_ERROR;
		}
	} else {
		newres->parentoff = newres->index * sizeof(uint32_t);
	}

	// update references
	// NOTE: referencing instruction operands directly here is a design
	// trade-off — it couples the analyzer to instruction internals.
	// Acceptable for the fetch-shader-only use case; revisit if the
	// analyzer is used more broadly.
	const GcnOperandField basefield = instr->dsts[0].field;
	const GcnOperandFieldInfo* src1 = &instr->srcs[1];
	const uint32_t dstdwords = instr->dsts[0].numbits / 32;
	for (uint32_t i = 0; i < dstdwords; i += 1) {
		const GcnOperandField curfield = basefield + i;
		if (curfield >= GCN_OPFIELD_SGPR_0 &&
		    curfield <= GCN_OPFIELD_SGPR_103) {
			ctx->lastsgpresources[curfield] = (SgprRef){
			    .index = newres->index,
			    .offset =
				src1->field == GCN_OPFIELD_LITERAL_CONST
				    ? src1->constant + (i * sizeof(uint32_t))
				    : i * sizeof(uint32_t),
			};
		}
	}

	// check if the resource is already in the list:
	// update it if it's a buffer or pointer
	const uint32_t numres = ctx->usedresources;
	for (uint32_t i = 0; i < numres; i += 1) {
		GcnResource* cur = &outresources[i];
		if (newres->index == cur->index &&
		    newres->parentindex == cur->parentindex &&
		    newres->parentoff == cur->parentoff) {
			if (cur->type == GCN_RES_POINTER) {
				if (newres->ptr.referredlen >
				    cur->ptr.referredlen) {
					*cur = *newres;
				}
			} else if (cur->type == GCN_RES_BUFFER) {
				if (newres->buf.length > cur->buf.length) {
					*cur = *newres;
				}
			}
			return GCN_ERR_OK;
		}
	}

	// this is a new resource
	if (numres >= maxoutresources) {
		return GCN_ERR_OVERFLOW;
	}

	// save offset when resource info is loaded
	newres->assignoff = instr->offset;

	outresources[numres] = *newres;
	ctx->usedresources += 1;

	return GCN_ERR_OK;
}

static void parse_exp(const GcnInstruction* instr, GcnAnalysis* out) {
	if (instr->exp.tgt <= GCN_EXP_TARGET_MRT_7) {
		const uint32_t mrtidx = instr->exp.tgt - GCN_EXP_TARGET_MRT_0;
		out->mrtbits |= 1 << mrtidx;
	} else if (instr->exp.tgt >= GCN_EXP_TARGET_POS_0 &&
		   instr->exp.tgt <= GCN_EXP_TARGET_POS_3) {
		const uint32_t posidx = instr->exp.tgt - GCN_EXP_TARGET_POS_0;
		out->posbits |= 1 << posidx;
	} else if (instr->exp.tgt >= GCN_EXP_TARGET_PARAM_0 &&
		   instr->exp.tgt <= GCN_EXP_TARGET_PARAM_31) {
		const uint32_t paramidx =
		    instr->exp.tgt - GCN_EXP_TARGET_PARAM_0;
		out->parambits |= 1 << paramidx;
	}
}

static GcnError parsemimg(
    Context* ctx, const GcnInstruction* instr, GcnResource* outresources,
    uint32_t maxoutresources
) {
	GcnError err = GCN_ERR_OK;

	switch (instr->mimg.opcode) {
	case GCN_IMAGE_SAMPLE:
	case GCN_IMAGE_SAMPLE_LZ:
	case GCN_IMAGE_SAMPLE_LZ_O:
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_IMAGE,
			.index = instr->srcs[1].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[1].numbits / 32,
		    },
		    instr, outresources, maxoutresources
		);
		if (err != GCN_ERR_OK) {
			break;
		}
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_SAMPLER,
			.index = instr->srcs[2].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[2].numbits / 32,
		    },
		    instr, outresources, maxoutresources
		);
		break;
	default:
		break;
	}

	return err;
}

static GcnError parsemtbuf(
    Context* ctx, const GcnInstruction* instr, GcnResource* outresources,
    uint32_t maxoutresources
) {
	GcnError err = GCN_ERR_OK;

	switch (instr->mtbuf.opcode) {
	case GCN_TBUFFER_LOAD_FORMAT_X:
	case GCN_TBUFFER_LOAD_FORMAT_XY:
	case GCN_TBUFFER_LOAD_FORMAT_XYZ:
	case GCN_TBUFFER_LOAD_FORMAT_XYZW:
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_BUFFER,
			.index = instr->srcs[1].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[1].numbits / 32,
			.buf =
			    {
				.length = instr->srcs[2].constant +
					  instr->dsts[0].numbits / 8,
				.uniform = false,
			    },
		    },
		    instr, outresources, maxoutresources
		);
		break;
	default:
		break;
	}

	return err;
}

static GcnError parsemubuf(
    Context* ctx, const GcnInstruction* instr, GcnResource* outresources,
    uint32_t maxoutresources
) {
	GcnError err = GCN_ERR_OK;

	switch (instr->mubuf.opcode) {
	case GCN_BUFFER_LOAD_FORMAT_X:
	case GCN_BUFFER_LOAD_FORMAT_XY:
	case GCN_BUFFER_LOAD_FORMAT_XYZ:
	case GCN_BUFFER_LOAD_FORMAT_XYZW:
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_BUFFER,
			.index = instr->srcs[1].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[1].numbits / 32,
			.buf =
			    {
				.length = instr->srcs[2].constant +
					  instr->dsts[0].numbits / 8,
				.uniform = false,
			    },
		    },
		    instr, outresources, maxoutresources
		);
		break;
	case GCN_BUFFER_STORE_FORMAT_X:
	case GCN_BUFFER_STORE_FORMAT_XY:
	case GCN_BUFFER_STORE_FORMAT_XYZ:
	case GCN_BUFFER_STORE_FORMAT_XYZW:
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_BUFFER,
			.index = instr->srcs[2].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[2].numbits / 32,
			.is_written = true,
			.buf =
			    {
				.length = instr->srcs[3].constant +
					  instr->srcs[0].numbits / 8,
				.uniform = false,
			    },
		    },
		    instr, outresources, maxoutresources
		);
		break;
	default:
		break;
	}

	return err;
}

static GcnError parsesmrd(
    Context* ctx, const GcnInstruction* instr, GcnResource* outresources,
    uint32_t maxoutresources
) {
	GcnError err = GCN_ERR_OK;

	switch (instr->smrd.opcode) {
	case GCN_S_BUFFER_LOAD_DWORD:
	case GCN_S_BUFFER_LOAD_DWORDX2:
	case GCN_S_BUFFER_LOAD_DWORDX4:
	case GCN_S_BUFFER_LOAD_DWORDX8:
	case GCN_S_BUFFER_LOAD_DWORDX16:
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_BUFFER,
			.index = instr->srcs[0].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[0].numbits / 32,
			.buf =
			    {
				.length = instr->srcs[1].constant +
					  instr->dsts[0].numbits / 8,
				.uniform = true,
			    },
		    },
		    instr, outresources, maxoutresources
		);
		break;
	case GCN_S_LOAD_DWORD:
	case GCN_S_LOAD_DWORDX2:
	case GCN_S_LOAD_DWORDX4:
	case GCN_S_LOAD_DWORDX8:
	case GCN_S_LOAD_DWORDX16:
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_POINTER,
			.index = instr->srcs[0].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[0].numbits / 32,
			.ptr =
			    {
				.referredlen = instr->srcs[1].constant +
					       instr->dsts[0].numbits / 8,
			    },
		    },
		    instr, outresources, maxoutresources
		);
		break;
	default:
		break;
	}

	return err;
}

static GcnError parsesop1(
    Context* ctx, const GcnInstruction* instr, GcnResource* outresources,
    uint32_t maxoutresources
) {
	GcnError err = GCN_ERR_OK;

	switch (instr->sop1.opcode) {
	case GCN_S_SWAPPC_B64:
		err = appendorupdate(
		    ctx,
		    &(GcnResource){
			.type = GCN_RES_CODE,
			.index = instr->srcs[0].field - GCN_OPFIELD_SGPR_0,
			.parentindex = -1,
			.numdwords = instr->srcs[0].numbits / 32,
		    },
		    instr, outresources, maxoutresources
		);
		break;
	default:
		break;
	}

	return err;
}

static int rescmpfunc(const void* p1, const void* p2) {
	const GcnResource* castp1 = p1;
	const GcnResource* castp2 = p2;
	if (castp1->parentindex < castp2->parentindex) {
		return -1;
	} else if (castp1->parentindex > castp2->parentindex) {
		return 1;
	}
	if (castp1->index < castp2->index) {
		return -1;
	} else if (castp1->index > castp2->index) {
		return 1;
	}
	return 0;
}

static inline void processfi(GcnAnalysis* out, const GcnOperandFieldInfo* fi) {
	if (fi->field >= GCN_OPFIELD_SGPR_0 &&
	    fi->field <= GCN_OPFIELD_SGPR_103) {
		out->numsgprs = umax(
		    (fi->field - GCN_OPFIELD_SGPR_0) + (fi->numbits / 32),
		    out->numsgprs
		);
	}
	if (fi->field >= GCN_OPFIELD_VGPR_0 &&
	    fi->field <= GCN_OPFIELD_VGPR_255) {
		out->numvgprs = umax(
		    (fi->field - GCN_OPFIELD_VGPR_0) + (fi->numbits / 32),
		    out->numvgprs
		);
	}
}

GcnError gcnAnalyzeShader(
    const void* code, uint32_t codesize, GcnAnalysis* out
) {
	if (!code || !codesize || !out) {
		return GCN_ERR_INVALID_ARG;
	}

	GcnDecoderContext decoder = {0};
	GcnError err = gcnDecoderInit(&decoder, code, codesize);
	if (err != GCN_ERR_OK) {
		return err;
	}

	Context ctx = {0};
	for (size_t i = 0; i < uasize(ctx.lastsgpresources); i += 1) {
		ctx.lastsgpresources[i] = (SgprRef){.index = -1, .offset = 0};
	}

	while (1) {
		GcnInstruction instr = {0};
		err = gcnDecodeInstruction(&decoder, &instr);
		if (err == GCN_ERR_END_OF_CODE ||
		    (instr.microcode == GCN_MICROCODE_SOPP &&
		     instr.sopp.opcode == GCN_S_ENDPGM)) {
			break;
		}
		if (err != GCN_ERR_OK) {
			return err;
		}

		for (uint32_t i = 0; i < instr.numsrcs; i += 1) {
			processfi(out, &instr.srcs[i]);
		}
		for (uint32_t i = 0; i < instr.numdsts; i += 1) {
			processfi(out, &instr.dsts[i]);
		}

		switch (instr.microcode) {
		case GCN_MICROCODE_EXP:
			parse_exp(&instr, out);
			break;
		case GCN_MICROCODE_MIMG:
			err = parsemimg(
			    &ctx, &instr, out->resources, out->maxresources
			);
			break;
		case GCN_MICROCODE_MTBUF:
			err = parsemtbuf(
			    &ctx, &instr, out->resources, out->maxresources
			);
			break;
		case GCN_MICROCODE_MUBUF:
			err = parsemubuf(
			    &ctx, &instr, out->resources, out->maxresources
			);
			break;
		case GCN_MICROCODE_SMRD:
			err = parsesmrd(
			    &ctx, &instr, out->resources, out->maxresources
			);
			break;
		case GCN_MICROCODE_SOP1:
			err = parsesop1(
			    &ctx, &instr, out->resources, out->maxresources
			);
			break;
		default:
			break;
		}
		if (err != GCN_ERR_OK) {
			return err;
		}
	}

	qsort(
	    out->resources, ctx.usedresources, sizeof(GcnResource), rescmpfunc
	);
	out->numresources = ctx.usedresources;

	return GCN_ERR_OK;
}
