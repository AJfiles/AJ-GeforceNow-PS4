#include "gcn.h"

#include "u/utility.h"

#include "microcode.h"

static inline const char* instrname(const GcnInstruction* instr) {
	switch (instr->microcode) {
	case GCN_MICROCODE_INVALID:
		return "Invalid instruction";
	case GCN_MICROCODE_EXP:
		return "exp";
	case GCN_MICROCODE_MIMG:
		return gcnStrOpcodeMIMG(instr->mimg.opcode);
	case GCN_MICROCODE_MTBUF:
		return gcnStrOpcodeMTBUF(instr->mtbuf.opcode);
	case GCN_MICROCODE_MUBUF:
		return gcnStrOpcodeMUBUF(instr->mubuf.opcode);
	case GCN_MICROCODE_SMRD:
		return gcnStrOpcodeSMRD(instr->smrd.opcode);
	case GCN_MICROCODE_SOP1:
		return gcnStrOpcodeSOP1(instr->sop1.opcode);
	case GCN_MICROCODE_SOP2:
		return gcnStrOpcodeSOP2(instr->sop2.opcode);
	case GCN_MICROCODE_SOPC:
		return gcnStrOpcodeSOPC(instr->sopc.opcode);
	case GCN_MICROCODE_SOPK:
		return gcnStrOpcodeSOPK(instr->sopk.opcode);
	case GCN_MICROCODE_SOPP:
		return gcnStrOpcodeSOPP(instr->sopp.opcode);
	case GCN_MICROCODE_VOP1:
		return gcnStrOpcodeVOP1(instr->vop1.opcode);
	case GCN_MICROCODE_VOP2:
		return gcnStrOpcodeVOP2(instr->vop2.opcode);
	case GCN_MICROCODE_VOP3:
		return gcnStrOpcodeVOP3(instr->vop3.opcode);
	case GCN_MICROCODE_VOPC:
		return gcnStrOpcodeVOPC(instr->vopc.opcode);
	case GCN_MICROCODE_VINTRP:
		return gcnStrOpcodeVINTRP(instr->vintrp.opcode);
	case GCN_MICROCODE_DS:
		return gcnStrOpcodeDS(instr->ds.opcode);
	default:
		return NULL;
	}
}

static inline bool isvalu(const GcnInstruction* instr) {
	return instr->microcode == GCN_MICROCODE_VOP1 ||
	       instr->microcode == GCN_MICROCODE_VOP2 ||
	       instr->microcode == GCN_MICROCODE_VOP3 ||
	       instr->microcode == GCN_MICROCODE_VOPC ||
	       instr->microcode == GCN_MICROCODE_VINTRP;
}
static inline bool issalu(const GcnInstruction* instr) {
	return instr->microcode == GCN_MICROCODE_SOP1 ||
	       instr->microcode == GCN_MICROCODE_SOP2 ||
	       instr->microcode == GCN_MICROCODE_SOPC ||
	       instr->microcode == GCN_MICROCODE_SOPK ||
	       instr->microcode == GCN_MICROCODE_SOPP;
}

typedef struct {
	GcnDataType types[8];
	uint16_t bits[8];
	uint8_t numtypes;
} InstrTypes;

static InstrTypes getinstrtypes(const GcnInstruction* instr) {
	InstrTypes out = {0};
	_Static_assert(uasize(out.types) == uasize(out.bits), "");

	// other instruction types' don't matter
	if (!isvalu(instr) && !issalu(instr)) {
		return out;
	}

	const char* name = instrname(instr);
	if (!name) {
		return out;
	}

	char buf[32] = {0};
	strncpy(buf, name, sizeof(buf));

	char* e64bit = strstr(buf, "_e64");
	if (e64bit) {
		memset(e64bit, 0, 4);
	}

	char* str = buf;
	char* saveptr = NULL;
	char* curtok = NULL;
	do {
		curtok = u_strtok_r(str, "_", &saveptr);
		str = NULL;

		if (curtok) {
			if (out.numtypes >= uasize(out.types)) {
				break;
			}

			// untyped
			if (!strcmp(curtok, "b32")) {
				out.types[out.numtypes] = GCN_DT_BIN;
				out.bits[out.numtypes] = 32;
			} else if (!strcmp(curtok, "b64")) {
				out.types[out.numtypes] = GCN_DT_BIN;
				out.bits[out.numtypes] = 64;
			}
			// floats
			else if (!strcmp(curtok, "f16")) {
				out.types[out.numtypes] = GCN_DT_FLOAT;
				out.bits[out.numtypes] = 16;
			} else if (!strcmp(curtok, "f32")) {
				out.types[out.numtypes] = GCN_DT_FLOAT;
				out.bits[out.numtypes] = 32;
			} else if (!strcmp(curtok, "f64")) {
				out.types[out.numtypes] = GCN_DT_FLOAT;
				out.bits[out.numtypes] = 64;
			}
			// sints
			else if (!strcmp(curtok, "i4")) {
				out.types[out.numtypes] = GCN_DT_SINT;
				out.bits[out.numtypes] = 4;
			} else if (!strcmp(curtok, "i8")) {
				out.types[out.numtypes] = GCN_DT_SINT;
				out.bits[out.numtypes] = 8;
			} else if (!strcmp(curtok, "i16")) {
				out.types[out.numtypes] = GCN_DT_SINT;
				out.bits[out.numtypes] = 16;
			} else if (!strcmp(curtok, "i24")) {
				out.types[out.numtypes] = GCN_DT_SINT;
				out.bits[out.numtypes] = 24;
			} else if (!strcmp(curtok, "i32")) {
				out.types[out.numtypes] = GCN_DT_SINT;
				out.bits[out.numtypes] = 32;
			} else if (!strcmp(curtok, "i64")) {
				out.types[out.numtypes] = GCN_DT_SINT;
				out.bits[out.numtypes] = 64;
			}
			// uints
			else if (!strcmp(curtok, "u8")) {
				out.types[out.numtypes] = GCN_DT_UINT;
				out.bits[out.numtypes] = 8;
			} else if (!strcmp(curtok, "u16")) {
				out.types[out.numtypes] = GCN_DT_UINT;
				out.bits[out.numtypes] = 16;
			} else if (!strcmp(curtok, "u24")) {
				out.types[out.numtypes] = GCN_DT_UINT;
				out.bits[out.numtypes] = 24;
			} else if (!strcmp(curtok, "u32")) {
				out.types[out.numtypes] = GCN_DT_UINT;
				out.bits[out.numtypes] = 32;
			} else if (!strcmp(curtok, "u64")) {
				out.types[out.numtypes] = GCN_DT_UINT;
				out.bits[out.numtypes] = 64;
			} else {
				continue;
			}

			out.numtypes += 1;
		}
	} while (curtok);

	if (!out.numtypes) {
		out.types[out.numtypes] = GCN_DT_BIN;
		out.bits[out.numtypes] = 32;
		out.numtypes += 1;
	}

	return out;
}

GcnError gcnDecoderInit(
    GcnDecoderContext* ctx, const void* code, uint32_t codesize
) {
	if (!ctx || !code || !codesize) {
		return GCN_ERR_INVALID_ARG;
	}
	if (codesize < sizeof(uint32_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}
	if (codesize & 3) {
		return GCN_ERR_SIZE_NOT_A_MULTIPLE_FOUR;
	}

	ctx->codestart = code;
	ctx->curinstr = code;
	ctx->codesize = codesize;
	return GCN_ERR_OK;
}

GcnError gcnDecoderReset(GcnDecoderContext* ctx) {
	if (!ctx) {
		return GCN_ERR_INVALID_ARG;
	}
	ctx->curinstr = ctx->codestart;
	return GCN_ERR_OK;
}

static inline GcnError parse_vgpr_field(
    GcnOperandFieldInfo* fi, uint8_t field
) {
	if (field <= GCN_OPFIELD_SGPR_103) {
		fi->field = GCN_OPFIELD_VGPR_0 + field;
	} else {
		if (!parseoperandfield(&fi->field, field)) {
			return GCN_ERR_INVALID_SRC_FIELD;
		}
	}
	return GCN_ERR_OK;
}

static GcnError decode_ds(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr,
    uint32_t remainingsize
) {
	if (remainingsize < sizeof(uint64_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}

	const uint64_t instr64 = *(const uint64_t*)ctx->curinstr;

	const uint8_t op = ds_op(instr64);

	if (!parseopcode_ds(&outinstr->ds.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	const uint8_t offset0 = ds_offset0(instr64);
	const uint8_t offset1 = ds_offset1(instr64);
	const uint8_t gds = ds_gds(instr64);
	const uint8_t addr = ds_addr(instr64);
	const uint8_t data0 = ds_data0(instr64);
	const uint8_t data1 = ds_data1(instr64);
	const uint8_t vdst = ds_vdst(instr64);

	//
	// vsrcs
	//
	// write all srcs regardless of actual src count
	for (unsigned i = 0; i < umin(3, uasize(outinstr->srcs)); i += 1) {
		outinstr->srcs[i] = (GcnOperandFieldInfo){
		    .type = GCN_DT_UINT,
		    .numbits = 32,
		};
	}

	GcnError err = parse_vgpr_field(&outinstr->srcs[0], addr);
	if (err != GCN_ERR_OK) {
		return err;
	}
	err = parse_vgpr_field(&outinstr->srcs[1], data0);
	if (err != GCN_ERR_OK) {
		return err;
	}
	err = parse_vgpr_field(&outinstr->srcs[2], data1);
	if (err != GCN_ERR_OK) {
		return err;
	}

	// count srcs
	outinstr->numsrcs = 0;
	// set vaddr on most instructions
	switch (outinstr->ds.opcode) {
	case GCN_DS_APPEND:
	case GCN_DS_CONSUME:
	case GCN_DS_GWS_SEMA_P:
	case GCN_DS_GWS_SEMA_RELEASE_ALL:
	case GCN_DS_GWS_SEMA_V:
	case GCN_DS_NOP:
		break;
	default:
		outinstr->numsrcs += 1;
		break;
	}

	// set vdata0
	switch (outinstr->ds.opcode) {
	case GCN_DS_ADD_RTN_U32:
	case GCN_DS_ADD_RTN_U64:
	case GCN_DS_ADD_U32:
	case GCN_DS_ADD_U64:
	case GCN_DS_AND_B32:
	case GCN_DS_AND_B64:
	case GCN_DS_AND_RTN_B32:
	case GCN_DS_AND_RTN_B64:
	case GCN_DS_CMPST_B32:
	case GCN_DS_CMPST_B64:
	case GCN_DS_CMPST_F32:
	case GCN_DS_CMPST_F64:
	case GCN_DS_CMPST_RTN_B32:
	case GCN_DS_CMPST_RTN_B64:
	case GCN_DS_CMPST_RTN_F32:
	case GCN_DS_CMPST_RTN_F64:
	case GCN_DS_CONDXCHG32_RTN_B128:
	case GCN_DS_CONDXCHG32_RTN_B64:
	case GCN_DS_DEC_RTN_U32:
	case GCN_DS_DEC_RTN_U64:
	case GCN_DS_DEC_U32:
	case GCN_DS_DEC_U64:
	case GCN_DS_INC_RTN_U32:
	case GCN_DS_INC_RTN_U64:
	case GCN_DS_INC_U32:
	case GCN_DS_INC_U64:
	case GCN_DS_MAX_F32:
	case GCN_DS_MAX_F64:
	case GCN_DS_MAX_I32:
	case GCN_DS_MAX_I64:
	case GCN_DS_MAX_RTN_F32:
	case GCN_DS_MAX_RTN_F64:
	case GCN_DS_MAX_RTN_I32:
	case GCN_DS_MAX_RTN_I64:
	case GCN_DS_MAX_RTN_U32:
	case GCN_DS_MAX_RTN_U64:
	case GCN_DS_MAX_U32:
	case GCN_DS_MAX_U64:
	case GCN_DS_MIN_F32:
	case GCN_DS_MIN_F64:
	case GCN_DS_MIN_I32:
	case GCN_DS_MIN_I64:
	case GCN_DS_MIN_RTN_F32:
	case GCN_DS_MIN_RTN_F64:
	case GCN_DS_MIN_RTN_I32:
	case GCN_DS_MIN_RTN_I64:
	case GCN_DS_MIN_RTN_U32:
	case GCN_DS_MIN_RTN_U64:
	case GCN_DS_MIN_U32:
	case GCN_DS_MIN_U64:
	case GCN_DS_MSKOR_B32:
	case GCN_DS_MSKOR_B64:
	case GCN_DS_MSKOR_RTN_B32:
	case GCN_DS_MSKOR_RTN_B64:
	case GCN_DS_OR_B32:
	case GCN_DS_OR_B64:
	case GCN_DS_OR_RTN_B32:
	case GCN_DS_OR_RTN_B64:
	case GCN_DS_RSUB_RTN_U32:
	case GCN_DS_RSUB_RTN_U64:
	case GCN_DS_RSUB_U32:
	case GCN_DS_RSUB_U64:
	case GCN_DS_SUB_RTN_U32:
	case GCN_DS_SUB_RTN_U64:
	case GCN_DS_SUB_U32:
	case GCN_DS_SUB_U64:
	case GCN_DS_WRAP_RTN_B32:
	case GCN_DS_WRITE2ST64_B32:
	case GCN_DS_WRITE2ST64_B64:
	case GCN_DS_WRITE2_B32:
	case GCN_DS_WRITE2_B64:
	case GCN_DS_WRITE_B128:
	case GCN_DS_WRITE_B16:
	case GCN_DS_WRITE_B32:
	case GCN_DS_WRITE_B64:
	case GCN_DS_WRITE_B8:
	case GCN_DS_WRITE_B96:
	case GCN_DS_WRXCHG2ST64_RTN_B32:
	case GCN_DS_WRXCHG2ST64_RTN_B64:
	case GCN_DS_WRXCHG2_RTN_B32:
	case GCN_DS_WRXCHG2_RTN_B64:
	case GCN_DS_WRXCHG_RTN_B32:
	case GCN_DS_WRXCHG_RTN_B64:
	case GCN_DS_XOR_B32:
	case GCN_DS_XOR_B64:
	case GCN_DS_XOR_RTN_B32:
	case GCN_DS_XOR_RTN_B64:
		outinstr->numsrcs += 1;
		break;
	default:
		break;
	}

	// set vdata1
	switch (outinstr->ds.opcode) {
	case GCN_DS_CMPST_B32:
	case GCN_DS_CMPST_B64:
	case GCN_DS_CMPST_F32:
	case GCN_DS_CMPST_F64:
	case GCN_DS_CMPST_RTN_B32:
	case GCN_DS_CMPST_RTN_B64:
	case GCN_DS_CMPST_RTN_F32:
	case GCN_DS_CMPST_RTN_F64:
	case GCN_DS_MSKOR_B32:
	case GCN_DS_MSKOR_B64:
	case GCN_DS_MSKOR_RTN_B32:
	case GCN_DS_MSKOR_RTN_B64:
	case GCN_DS_WRAP_RTN_B32:
	case GCN_DS_WRITE2ST64_B32:
	case GCN_DS_WRITE2ST64_B64:
	case GCN_DS_WRITE2_B32:
	case GCN_DS_WRITE2_B64:
	case GCN_DS_WRXCHG2ST64_RTN_B32:
	case GCN_DS_WRXCHG2ST64_RTN_B64:
	case GCN_DS_WRXCHG2_RTN_B32:
	case GCN_DS_WRXCHG2_RTN_B64:
		outinstr->numsrcs += 1;
		break;
	default:
		break;
	}

	//
	// vdst
	//
	// write dst regardless if it's used or not
	if (vdst <= GCN_OPFIELD_SGPR_103) {
		outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdst;
	} else {
		if (!parseoperandfield(&outinstr->dsts[0].field, vdst)) {
			return GCN_ERR_INVALID_SRC_FIELD;
		}
	}

	// count vdst for specific instructions
	switch (outinstr->ds.opcode) {
	case GCN_DS_ADD_RTN_U32:
	case GCN_DS_ADD_RTN_U64:
	case GCN_DS_AND_RTN_B32:
	case GCN_DS_AND_RTN_B64:
	case GCN_DS_APPEND:
	case GCN_DS_CMPST_RTN_B32:
	case GCN_DS_CMPST_RTN_B64:
	case GCN_DS_CMPST_RTN_F32:
	case GCN_DS_CMPST_RTN_F64:
	case GCN_DS_CONDXCHG32_RTN_B128:
	case GCN_DS_CONDXCHG32_RTN_B64:
	case GCN_DS_CONSUME:
	case GCN_DS_DEC_RTN_U32:
	case GCN_DS_DEC_RTN_U64:
	case GCN_DS_INC_RTN_U32:
	case GCN_DS_INC_RTN_U64:
	case GCN_DS_MAX_RTN_F32:
	case GCN_DS_MAX_RTN_F64:
	case GCN_DS_MAX_RTN_I32:
	case GCN_DS_MAX_RTN_I64:
	case GCN_DS_MAX_RTN_U32:
	case GCN_DS_MAX_RTN_U64:
	case GCN_DS_MIN_RTN_F32:
	case GCN_DS_MIN_RTN_F64:
	case GCN_DS_MIN_RTN_I32:
	case GCN_DS_MIN_RTN_I64:
	case GCN_DS_MIN_RTN_U32:
	case GCN_DS_MIN_RTN_U64:
	case GCN_DS_MSKOR_RTN_B32:
	case GCN_DS_MSKOR_RTN_B64:
	case GCN_DS_OR_RTN_B32:
	case GCN_DS_OR_RTN_B64:
	case GCN_DS_ORDERED_COUNT:
	case GCN_DS_READ2ST64_B32:
	case GCN_DS_READ2ST64_B64:
	case GCN_DS_READ2_B32:
	case GCN_DS_READ2_B64:
	case GCN_DS_READ_B128:
	case GCN_DS_READ_B32:
	case GCN_DS_READ_B64:
	case GCN_DS_READ_B96:
	case GCN_DS_READ_I16:
	case GCN_DS_READ_I8:
	case GCN_DS_READ_U16:
	case GCN_DS_READ_U8:
	case GCN_DS_RSUB_RTN_U32:
	case GCN_DS_RSUB_RTN_U64:
	case GCN_DS_SUB_RTN_U32:
	case GCN_DS_SUB_RTN_U64:
	case GCN_DS_SWIZZLE_B32:
	case GCN_DS_WRAP_RTN_B32:
	case GCN_DS_WRXCHG2ST64_RTN_B32:
	case GCN_DS_WRXCHG2ST64_RTN_B64:
	case GCN_DS_WRXCHG2_RTN_B32:
	case GCN_DS_WRXCHG2_RTN_B64:
	case GCN_DS_WRXCHG_RTN_B32:
	case GCN_DS_WRXCHG_RTN_B64:
	case GCN_DS_XOR_RTN_B32:
	case GCN_DS_XOR_RTN_B64:
		outinstr->numdsts = 1;
		break;
	default:
		break;
	}

	//
	// calculate types for all fields
	//
	const InstrTypes types = getinstrtypes(outinstr);
	switch (types.numtypes) {
	case 0:
		// none found, that's fine for this microcode.
		// default them to b32
		for (unsigned i = 0; i < outinstr->numsrcs; i += 1) {
			outinstr->srcs[i].numbits = 32;
			outinstr->srcs[i].type = GCN_DT_BIN;
		}
		outinstr->dsts[0].numbits = 32;
		outinstr->dsts[0].type = GCN_DT_BIN;
		break;
	case 1:
		for (unsigned i = 0; i < outinstr->numsrcs; i += 1) {
			outinstr->srcs[i].numbits = types.bits[0];
			outinstr->srcs[i].type = types.types[0];
		}
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->dsts[0].type = types.types[0];
		break;
	default:
		// unexpected number of types
		return GCN_ERR_INTERNAL_ERROR;
	}

	// extra data
	outinstr->ds.offsets[0] = offset0;
	outinstr->ds.offsets[1] = offset1;
	outinstr->ds.gds = gds != 0;

	return GCN_ERR_OK;
}

static GcnError decodeexp(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr,
    uint32_t remainingsize
) {
	if (remainingsize < sizeof(uint64_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}

	const uint64_t instr64 = *(const uint64_t*)ctx->curinstr;

	const uint8_t vsrcs[4] = {
	    exp_vsrc0(instr64),
	    exp_vsrc1(instr64),
	    exp_vsrc2(instr64),
	    exp_vsrc3(instr64),
	};
	const uint8_t en = exp_en(instr64);
	const uint8_t target = exp_target(instr64);
	const uint8_t compr = exp_compr(instr64);
	const uint8_t done = exp_done(instr64);
	const uint8_t vm = exp_vm(instr64);

	outinstr->numsrcs = compr ? 2 : 4;
	outinstr->numdsts = 1;

	for (uint32_t i = 0; i < outinstr->numsrcs; i += 1) {
		if (vsrcs[i] <= GCN_OPFIELD_SGPR_103) {
			outinstr->srcs[i].field = GCN_OPFIELD_VGPR_0 + vsrcs[i];
		} else {
			if (!parseoperandfield(
				&outinstr->srcs[i].field, vsrcs[i]
			    )) {
				return GCN_ERR_INVALID_SRC_FIELD;
			}
		}
	}
	if (!parseexptarget(&outinstr->exp.tgt, target)) {
		return GCN_ERR_INVALID_DST_FIELD;
	}

	for (uint32_t i = 0; i < outinstr->numsrcs; i += 1) {
		outinstr->srcs[i].numbits = 32;
		outinstr->srcs[i].type = GCN_DT_FLOAT;
	}
	for (uint32_t i = 0; i < outinstr->numdsts; i += 1) {
		outinstr->dsts[i].numbits = 32;
		outinstr->dsts[i].type = GCN_DT_FLOAT;
	}

	outinstr->exp.compressed = compr ? true : false;
	outinstr->exp.done = done ? true : false;
	outinstr->exp.en = en;
	outinstr->exp.validmask = vm ? true : false;

	return GCN_ERR_OK;
}

static GcnError decodemimg(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr,
    uint32_t remainingsize
) {
	if (remainingsize < sizeof(uint64_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}

	const uint64_t instr64 = *(const uint64_t*)ctx->curinstr;

	const uint8_t op = mimg_op(instr64);
	const uint8_t vaddr = mimg_vaddr(instr64);
	const uint8_t vdata = mimg_vdata(instr64);
	const uint8_t srsrc = mimg_srsrc(instr64);
	const uint8_t ssamp = mimg_ssamp(instr64);

	const uint8_t dmask = mimg_dmask(instr64);
	const uint8_t unrm = mimg_unrm(instr64);
	const uint8_t glc = mimg_glc(instr64);
	const uint8_t da = mimg_da(instr64);
	const uint8_t r128 = mimg_r128(instr64);
	const uint8_t tfe = mimg_tfe(instr64);
	const uint8_t lwe = mimg_lwe(instr64);
	const uint8_t slc = mimg_slc(instr64);

	if (!parseopcode_mimg(&outinstr->mimg.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	uint8_t numdstdwords = 0;
	if (dmask & 0x1) {
		numdstdwords += 1;
	}
	if (dmask & 0x2) {
		numdstdwords += 1;
	}
	if (dmask & 0x4) {
		numdstdwords += 1;
	}
	if (dmask & 0x8) {
		numdstdwords += 1;
	}

	outinstr->numsrcs = 3;
	outinstr->numdsts = 1;

	outinstr->srcs[0].numbits = 32;
	outinstr->srcs[1].numbits = 256;
	outinstr->srcs[2].numbits = 128;
	outinstr->dsts[0].numbits = numdstdwords * 32;

	outinstr->srcs[0].type = GCN_DT_BIN;
	outinstr->srcs[1].type = GCN_DT_BIN;
	outinstr->srcs[2].type = GCN_DT_BIN;
	outinstr->dsts[0].type = GCN_DT_FLOAT;

	switch (outinstr->mimg.opcode) {
	case GCN_IMAGE_GET_RESINFO:
	case GCN_IMAGE_LOAD:
	case GCN_IMAGE_LOAD_MIP:
	case GCN_IMAGE_LOAD_MIP_PCK:
	case GCN_IMAGE_LOAD_MIP_PCK_SGN:
	case GCN_IMAGE_LOAD_PCK:
	case GCN_IMAGE_LOAD_PCK_SGN:
		outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdata;
		outinstr->srcs[0].field = GCN_OPFIELD_VGPR_0 + vaddr;
		outinstr->srcs[1].field = GCN_OPFIELD_SGPR_0 + srsrc;
		outinstr->srcs[1].numbits = r128 ? 128 : 256;
		outinstr->numsrcs = 2;
		outinstr->numdsts = 1;
		break;
	case GCN_IMAGE_ATOMIC_ADD:
	case GCN_IMAGE_ATOMIC_AND:
	case GCN_IMAGE_ATOMIC_CMPSWAP:
	case GCN_IMAGE_ATOMIC_DEC:
	case GCN_IMAGE_ATOMIC_INC:
	case GCN_IMAGE_ATOMIC_OR:
	case GCN_IMAGE_ATOMIC_SMAX:
	case GCN_IMAGE_ATOMIC_SMIN:
	case GCN_IMAGE_ATOMIC_SUB:
	case GCN_IMAGE_ATOMIC_SWAP:
	case GCN_IMAGE_ATOMIC_UMAX:
	case GCN_IMAGE_ATOMIC_UMIN:
	case GCN_IMAGE_ATOMIC_XOR:
	case GCN_IMAGE_STORE:
	case GCN_IMAGE_STORE_MIP:
	case GCN_IMAGE_STORE_MIP_PCK:
	case GCN_IMAGE_STORE_PCK:
		outinstr->srcs[0].field = GCN_OPFIELD_VGPR_0 + vdata;
		outinstr->srcs[0].numbits = numdstdwords * 32;
		outinstr->srcs[1].field = GCN_OPFIELD_VGPR_0 + vaddr;
		outinstr->srcs[2].field = GCN_OPFIELD_SGPR_0 + srsrc;
		outinstr->srcs[2].numbits = r128 ? 128 : 256;
		outinstr->numsrcs = 3;
		outinstr->numdsts = 0;
		break;
	default:
		outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdata;
		outinstr->srcs[0].field = GCN_OPFIELD_VGPR_0 + vaddr;
		outinstr->srcs[1].field = GCN_OPFIELD_SGPR_0 + srsrc;
		outinstr->srcs[2].field = GCN_OPFIELD_SGPR_0 + ssamp;
		outinstr->srcs[1].numbits = r128 ? 128 : 256;
		outinstr->srcs[0].type = GCN_DT_FLOAT;
		break;
	}

	outinstr->mimg.dmask = dmask;
	outinstr->mimg.unnormalized = unrm ? true : false;
	outinstr->mimg.globalcoherency = glc ? true : false;
	outinstr->mimg.declarearray = da ? true : false;
	outinstr->mimg.resource128 = r128 ? true : false;
	outinstr->mimg.texturefail = tfe ? true : false;
	outinstr->mimg.lodwarning = lwe ? true : false;
	outinstr->mimg.syslevelcoherent = slc ? true : false;

	return GCN_ERR_OK;
}

static GcnError decodemtbuf(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr,
    uint32_t remainingsize
) {
	if (remainingsize < sizeof(uint64_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}

	const uint64_t instr64 = *(const uint64_t*)ctx->curinstr;

	const uint8_t op = mtbuf_op(instr64);
	const uint8_t vaddr = mtbuf_vaddr(instr64);
	const uint8_t vdata = mtbuf_vdata(instr64);
	const uint8_t srsrc = mtbuf_srsrc(instr64);
	const uint8_t soffset = mtbuf_soffset(instr64);

	const uint16_t offset = mtbuf_offset(instr64);
	const uint8_t dfmt = mtbuf_dfmt(instr64);
	const uint8_t nfmt = mtbuf_nfmt(instr64);
	const uint8_t offen = mtbuf_offen(instr64);
	const uint8_t idxen = mtbuf_idxen(instr64);
	const uint8_t glc = mtbuf_glc(instr64);
	const uint8_t slc = mtbuf_slc(instr64);
	const uint8_t tfe = mtbuf_tfe(instr64);

	if (!isvalidmtbuf(op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->mtbuf.opcode = op;

	outinstr->numsrcs = 4;
	outinstr->numdsts = 0;

	outinstr->srcs[0].numbits = 32;
	outinstr->srcs[1].numbits = 32;
	outinstr->srcs[2].numbits = 128;
	outinstr->srcs[3].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_UINT;
	outinstr->srcs[1].type = GCN_DT_UINT;
	outinstr->srcs[2].type = GCN_DT_UINT;
	outinstr->srcs[3].type = GCN_DT_UINT;
	outinstr->dsts[0].type = GCN_DT_UINT;

	bool isload = false;
	switch (outinstr->mtbuf.opcode) {
	case GCN_BUFFER_LOAD_FORMAT_X:
		outinstr->dsts[0].numbits = 32;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_FORMAT_XY:
		outinstr->dsts[0].numbits = 64;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_FORMAT_XYZ:
		outinstr->dsts[0].numbits = 96;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_FORMAT_XYZW:
		outinstr->dsts[0].numbits = 128;
		isload = true;
		break;
	case GCN_BUFFER_STORE_FORMAT_X:
		outinstr->srcs[0].numbits = 32;
		isload = true;
		break;
	case GCN_BUFFER_STORE_FORMAT_XY:
		outinstr->srcs[0].numbits = 64;
		isload = true;
		break;
	case GCN_BUFFER_STORE_FORMAT_XYZ:
		outinstr->srcs[0].numbits = 96;
		isload = true;
		break;
	case GCN_BUFFER_STORE_FORMAT_XYZW:
		outinstr->srcs[0].numbits = 128;
		isload = true;
		break;
	default:
		break;
	}

	uint8_t nextsrc = 0;
	if (isload) {
		outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdata;
		outinstr->srcs[1].numbits = 128;
		outinstr->srcs[2].numbits = 32;
		outinstr->numsrcs = 3;
		outinstr->numdsts = 1;
	} else {
		outinstr->srcs[nextsrc].field = GCN_OPFIELD_VGPR_0 + vdata;
		nextsrc += 1;
	}

	outinstr->srcs[nextsrc].field = GCN_OPFIELD_VGPR_0 + vaddr;
	nextsrc += 1;
	outinstr->srcs[nextsrc].field = GCN_OPFIELD_SGPR_0 + srsrc;
	nextsrc += 1;
	outinstr->srcs[nextsrc].field = GCN_OPFIELD_SGPR_0 + soffset;
	nextsrc += 1;

	outinstr->mtbuf.offset = offset;
	outinstr->mtbuf.dfmt = dfmt;
	outinstr->mtbuf.nfmt = nfmt;
	outinstr->mtbuf.hasoffset = offen ? true : false;
	outinstr->mtbuf.hasindex = idxen ? true : false;
	outinstr->mtbuf.globalcoherency = glc ? true : false;
	outinstr->mtbuf.syslevelcoherent = slc ? true : false;
	outinstr->mtbuf.texturefail = tfe ? true : false;

	return GCN_ERR_OK;
}

static GcnError decodemubuf(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr,
    uint32_t remainingsize
) {
	if (remainingsize < sizeof(uint64_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}

	const uint64_t instr64 = *(const uint64_t*)ctx->curinstr;

	const uint8_t op = mubuf_op(instr64);
	const uint8_t vaddr = mubuf_vaddr(instr64);
	const uint8_t vdata = mubuf_vdata(instr64);
	const uint8_t srsrc = mubuf_srsrc(instr64);
	const uint8_t soffset = mubuf_soffset(instr64);

	const uint16_t offset = mubuf_offset(instr64);
	const uint8_t offen = mubuf_offen(instr64);
	const uint8_t idxen = mubuf_idxen(instr64);
	const uint8_t glc = mubuf_glc(instr64);
	const uint8_t lds = mubuf_lds(instr64);
	const uint8_t slc = mubuf_slc(instr64);
	const uint8_t tfe = mubuf_tfe(instr64);

	if (!isvalidmubuf(op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->mubuf.opcode = op;

	outinstr->numsrcs = 4;
	outinstr->numdsts = 0;

	outinstr->srcs[0].numbits = 32;
	outinstr->srcs[1].numbits = 32;
	outinstr->srcs[2].numbits = 128;
	outinstr->srcs[3].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_UINT;
	outinstr->srcs[1].type = GCN_DT_UINT;
	outinstr->srcs[2].type = GCN_DT_UINT;
	outinstr->srcs[3].type = GCN_DT_UINT;
	outinstr->dsts[0].type = GCN_DT_UINT;

	bool isload = false;
	switch (outinstr->mubuf.opcode) {
	case GCN_BUFFER_LOAD_SBYTE:
	case GCN_BUFFER_LOAD_UBYTE:
		outinstr->dsts[0].numbits = 8;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_SSHORT:
	case GCN_BUFFER_LOAD_USHORT:
		outinstr->dsts[0].numbits = 16;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_DWORD:
	case GCN_BUFFER_LOAD_FORMAT_X:
		outinstr->dsts[0].numbits = 32;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_DWORDX2:
	case GCN_BUFFER_LOAD_FORMAT_XY:
		outinstr->dsts[0].numbits = 64;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_DWORDX3:
	case GCN_BUFFER_LOAD_FORMAT_XYZ:
		outinstr->dsts[0].numbits = 96;
		isload = true;
		break;
	case GCN_BUFFER_LOAD_DWORDX4:
	case GCN_BUFFER_LOAD_FORMAT_XYZW:
		outinstr->dsts[0].numbits = 128;
		isload = true;
		break;
	default:
		break;
	}

	uint8_t nextsrc = 0;
	if (isload) {
		outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdata;
		outinstr->srcs[1].numbits = 128;
		outinstr->srcs[2].numbits = 32;
		outinstr->numsrcs = 3;
		outinstr->numdsts = 1;
	} else {
		outinstr->srcs[nextsrc].field = GCN_OPFIELD_VGPR_0 + vdata;
		nextsrc += 1;
	}

	outinstr->srcs[nextsrc].field = GCN_OPFIELD_VGPR_0 + vaddr;
	nextsrc += 1;
	outinstr->srcs[nextsrc].field = GCN_OPFIELD_SGPR_0 + srsrc;
	nextsrc += 1;
	outinstr->srcs[nextsrc].field = GCN_OPFIELD_SGPR_0 + soffset;
	nextsrc += 1;

	outinstr->mubuf.offset = offset;
	outinstr->mubuf.hasoffset = offen ? true : false;
	outinstr->mubuf.hasindex = idxen ? true : false;
	outinstr->mubuf.globalcoherency = glc ? true : false;
	outinstr->mubuf.lds = lds ? true : false;
	outinstr->mubuf.syslevelcoherent = slc ? true : false;
	outinstr->mubuf.texturefail = tfe ? true : false;

	return GCN_ERR_OK;
}

static GcnError decodesmrd(GcnInstruction* outinstr, uint32_t instr) {
	const uint8_t op = smrd_op(instr);
	const uint8_t imm = smrd_imm(instr);
	const uint8_t offset = smrd_offset(instr);
	const uint8_t sbase = smrd_sbase(instr);
	const uint8_t sdst = smrd_sdst(instr);

	if (!isvalidsmrd(op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->smrd.opcode = op;

	// in SMRDs, srcs is calculated from sbase and its size from the opcode
	if (!parseoperandfield(&outinstr->srcs[0].field, sbase * 2)) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	if (!parseoperandfield(&outinstr->dsts[0].field, sdst)) {
		return GCN_ERR_INVALID_DST_FIELD;
	}

	outinstr->numsrcs = 2;
	outinstr->numdsts = 1;

	outinstr->srcs[0].numbits = 128;
	outinstr->srcs[1].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_UINT;
	outinstr->srcs[1].type = GCN_DT_UINT;
	outinstr->dsts[0].type = GCN_DT_UINT;

	outinstr->smrd.imm = imm;

	if (imm) {
		outinstr->srcs[1].field = GCN_OPFIELD_LITERAL_CONST;
		outinstr->srcs[1].constant = offset << 2;
	} else {
		if (!parseoperandfield(&outinstr->srcs[1].field, offset)) {
			return GCN_ERR_INVALID_SRC_FIELD;
		}
	}

	switch (outinstr->smrd.opcode) {
	case GCN_S_LOAD_DWORD:
		outinstr->srcs[0].numbits = 64;
		// fallthrough
	case GCN_S_BUFFER_LOAD_DWORD:
		outinstr->dsts[0].numbits = 32;
		break;
	case GCN_S_LOAD_DWORDX2:
		outinstr->srcs[0].numbits = 64;
		// fallthrough
	case GCN_S_BUFFER_LOAD_DWORDX2:
		outinstr->dsts[0].numbits = 64;
		break;
	case GCN_S_LOAD_DWORDX4:
		outinstr->srcs[0].numbits = 64;
		// fallthrough
	case GCN_S_BUFFER_LOAD_DWORDX4:
		outinstr->dsts[0].numbits = 128;
		break;
	case GCN_S_LOAD_DWORDX8:
		outinstr->srcs[0].numbits = 64;
		// fallthrough
	case GCN_S_BUFFER_LOAD_DWORDX8:
		outinstr->dsts[0].numbits = 256;
		break;
	case GCN_S_LOAD_DWORDX16:
		outinstr->srcs[0].numbits = 64;
		// fallthrough
	case GCN_S_BUFFER_LOAD_DWORDX16:
		outinstr->dsts[0].numbits = 512;
		break;
	case GCN_S_DCACHE_INV:
	case GCN_S_DCACHE_INV_VOL:
		outinstr->numdsts = 0;
		break;
	case GCN_S_MEMTIME:
		outinstr->numsrcs = 0;
		break;
	default:
		return GCN_ERR_INTERNAL_ERROR;
	}

	return GCN_ERR_OK;
}

static GcnError decodesop1(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr, uint32_t instr,
    uint32_t remainingsize
) {
	const uint8_t op = sop1_op(instr);
	const uint8_t ssrc0 = sop1_ssrc0(instr);
	const uint8_t sdst = sop1_sdst(instr);

	if (!parseopcode_sop1(&outinstr->sop1.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	if (!parseoperandfield(&outinstr->srcs[0].field, ssrc0)) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	if (!parseoperandfield(&outinstr->dsts[0].field, sdst)) {
		return GCN_ERR_INVALID_DST_FIELD;
	}

	outinstr->numsrcs = 1;
	outinstr->numdsts = 1;

	switch (outinstr->sop1.opcode) {
	case GCN_S_GETPC_B64:
		outinstr->numsrcs = 0;
		break;
	case GCN_S_CBRANCH_JOIN:
	case GCN_S_RFE_B64:
	case GCN_S_SETPC_B64:
		outinstr->numdsts = 0;
		break;
	default:
		break;
	}

	outinstr->srcs[0].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_BIN;
	outinstr->dsts[0].type = GCN_DT_BIN;

	const InstrTypes types = getinstrtypes(outinstr);
	if (types.numtypes == 1) {
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[0];
		outinstr->dsts[0].type = types.types[0];
	} else if (types.numtypes == 2) {
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[0];
		outinstr->dsts[0].type = types.types[0];
	} else {
		// unexpected number of types
		return GCN_ERR_INTERNAL_ERROR;
	}

	if (outinstr->srcs[0].field == GCN_OPFIELD_LITERAL_CONST) {
		if (remainingsize < sizeof(uint32_t) * 2) {
			return GCN_ERR_CODE_TOO_SMALL;
		}

		outinstr->srcs[0].constant =
		    *(const uint32_t*)(ctx->curinstr + sizeof(uint32_t));

		outinstr->length += sizeof(uint32_t);
	}

	return GCN_ERR_OK;
}

static GcnError decodesop2(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr, uint32_t instr,
    uint32_t remainingsize
) {
	const uint8_t op = sop2_op(instr);
	const uint8_t ssrc0 = sop2_ssrc0(instr);
	const uint8_t ssrc1 = sop2_ssrc1(instr);
	const uint8_t sdst = sop2_sdst(instr);

	if (!parseopcode_sop2(&outinstr->sop2.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	if (!parseoperandfield(&outinstr->srcs[0].field, ssrc0)) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	if (!parseoperandfield(&outinstr->srcs[1].field, ssrc1)) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	if (!parseoperandfield(&outinstr->dsts[0].field, sdst)) {
		return GCN_ERR_INVALID_DST_FIELD;
	}

	outinstr->numsrcs = 2;
	outinstr->numdsts = 1;

	if (outinstr->sop2.opcode == GCN_S_CBRANCH_G_FORK) {
		outinstr->numdsts = 0;
	}

	outinstr->srcs[0].numbits = 128;
	outinstr->srcs[1].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_BIN;
	outinstr->srcs[1].type = GCN_DT_BIN;
	outinstr->dsts[0].type = GCN_DT_BIN;

	const InstrTypes types = getinstrtypes(outinstr);
	if (types.numtypes == 1) {
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->srcs[1].numbits = types.bits[0];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[0];
		outinstr->srcs[1].type = types.types[0];
		outinstr->dsts[0].type = types.types[0];
	} else {
		// unexpected number of types
		return GCN_ERR_INTERNAL_ERROR;
	}

	for (uint8_t i = 0; i < outinstr->numsrcs; i += 1) {
		GcnOperandFieldInfo* fi = &outinstr->srcs[i];
		if (fi->field == GCN_OPFIELD_LITERAL_CONST) {
			if (remainingsize < sizeof(uint32_t) * 2) {
				return GCN_ERR_CODE_TOO_SMALL;
			}

			fi->constant = *(const uint32_t*)(ctx->curinstr +
							  sizeof(uint32_t));
			outinstr->length += sizeof(uint32_t);
		}
	}

	return GCN_ERR_OK;
}

static GcnError decodesopc(GcnInstruction* outinstr, uint32_t instr) {
	const uint8_t op = sopc_op(instr);
	const uint8_t ssrc0 = sopc_ssrc0(instr);
	const uint8_t ssrc1 = sopc_ssrc1(instr);

	if (!isvalidsopc(op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->sopp.opcode = op;
	outinstr->numsrcs = 2;
	outinstr->numdsts = 0;
	outinstr->srcs[0].field = GCN_OPFIELD_SGPR_0 + ssrc0;
	outinstr->srcs[1].field = GCN_OPFIELD_SGPR_0 + ssrc1;

	const InstrTypes types = getinstrtypes(outinstr);

	if (outinstr->sopc.opcode == GCN_S_SETVSKIP) {
		outinstr->srcs[0].numbits = 32;
		outinstr->srcs[0].type = GCN_DT_BIN;
		outinstr->srcs[1].numbits = 32;
		outinstr->srcs[1].type = GCN_DT_BIN;
	} else {
		outinstr->srcs[0].type = types.types[0];
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->srcs[1].type = types.types[0];
		outinstr->srcs[1].numbits = types.bits[0];
	}

	return GCN_ERR_OK;
}

static GcnError decodesopk(GcnInstruction* outinstr, uint32_t instr) {
	const uint8_t op = sopk_op(instr);
	const uint8_t sdst = sopk_sdst(instr);
	const uint16_t simm = sopk_simm(instr);

	if (!isvalidsopk(op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->sopk.opcode = op;
	outinstr->numsrcs = 2;
	outinstr->numdsts = 0;

	const GcnOperandFieldInfo sfield = {
	    .field = GCN_OPFIELD_SGPR_0 + sdst,
	    .type = GCN_DT_BIN,
	    .numbits = 32,
	};
	const GcnOperandFieldInfo sconst = {
	    .field = GCN_OPFIELD_LITERAL_CONST,
	    .type = GCN_DT_BIN,
	    .numbits = 32,
	    .constant = simm,
	};

	switch (outinstr->sopk.opcode) {
	case GCN_S_ADDK_I32:
	case GCN_S_CMOVK_I32:
	case GCN_S_MOVK_I32:
	case GCN_S_MULK_I32:
		outinstr->dsts[0] = sfield;
		outinstr->dsts[0].type = GCN_DT_SINT;
		outinstr->srcs[0] = sconst;
		outinstr->srcs[0].type = GCN_DT_SINT;
		outinstr->numsrcs = 1;
		outinstr->numdsts = 1;
		break;
	case GCN_S_CBRANCH_I_FORK:
		outinstr->srcs[0] = sfield;
		outinstr->srcs[1] = sconst;
		outinstr->numsrcs = 2;
		outinstr->numdsts = 0;
		break;
	case GCN_S_CMPK_EQ_I32:
	case GCN_S_CMPK_GE_I32:
	case GCN_S_CMPK_GT_I32:
	case GCN_S_CMPK_LE_I32:
	case GCN_S_CMPK_LG_I32:
	case GCN_S_CMPK_LT_I32:
		outinstr->srcs[0] = sfield;
		outinstr->srcs[0].type = GCN_DT_SINT;
		outinstr->srcs[1] = sconst;
		outinstr->srcs[1].type = GCN_DT_SINT;
		outinstr->numsrcs = 2;
		outinstr->numdsts = 0;
		break;
	case GCN_S_CMPK_EQ_U32:
	case GCN_S_CMPK_GE_U32:
	case GCN_S_CMPK_GT_U32:
	case GCN_S_CMPK_LE_U32:
	case GCN_S_CMPK_LG_U32:
	case GCN_S_CMPK_LT_U32:
		outinstr->srcs[0] = sfield;
		outinstr->srcs[0].type = GCN_DT_UINT;
		outinstr->srcs[1] = sconst;
		outinstr->srcs[1].type = GCN_DT_UINT;
		outinstr->numsrcs = 2;
		outinstr->numdsts = 0;
		break;
	case GCN_S_SETREG_B32:
	case GCN_S_SETREG_IMM32_B32:
		outinstr->dsts[0] = sfield;
		outinstr->srcs[0] = sconst;
		outinstr->numsrcs = 1;
		outinstr->numdsts = 1;
	default:
		break;
	}

	return GCN_ERR_OK;
}

static GcnError decodesopp(GcnInstruction* outinstr, uint32_t instr) {
	const uint8_t op = sopp_op(instr);
	const uint16_t simm16 = sopp_simm16(instr);

	if (!isvalidsopp(op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->sopp.opcode = op;
	outinstr->sopp.constant = simm16;

	outinstr->numsrcs = 0;
	outinstr->numdsts = 0;

	switch (outinstr->sopp.opcode) {
	case GCN_S_BRANCH:
	case GCN_S_CBRANCH_CDBGSYS:
	case GCN_S_CBRANCH_CDBGSYS_AND_USER:
	case GCN_S_CBRANCH_CDBGSYS_OR_USER:
	case GCN_S_CBRANCH_CDBGUSER:
	case GCN_S_CBRANCH_EXECNZ:
	case GCN_S_CBRANCH_EXECZ:
	case GCN_S_CBRANCH_SCC0:
	case GCN_S_CBRANCH_SCC1:
	case GCN_S_CBRANCH_VCCNZ:
	case GCN_S_CBRANCH_VCCZ:
		outinstr->srcs[0] = (GcnOperandFieldInfo){
		    .field = GCN_OPFIELD_LITERAL_CONST,
		    .numbits = 32,
		    .constant = simm16 << 2,
		};
		outinstr->numsrcs = 1;
		break;
	case GCN_S_WAITCNT:
		outinstr->sopp.waitcnt.vm = simm16 & 0xf;
		outinstr->sopp.waitcnt.exp = (simm16 >> 4) & 0x7;
		outinstr->sopp.waitcnt.lgkm = (simm16 >> 8) & 0xf;

		if (outinstr->sopp.waitcnt.vm == 0xf) {
			outinstr->sopp.waitcnt.vm = 0xff;
		}
		if (outinstr->sopp.waitcnt.exp == 0x7) {
			outinstr->sopp.waitcnt.exp = 0xff;
		}
		if (outinstr->sopp.waitcnt.lgkm == 0xf) {
			outinstr->sopp.waitcnt.lgkm = 0xff;
		}
	default:
		break;
	}

	outinstr->srcs[0].numbits = 32;
	outinstr->srcs[0].type = GCN_DT_UINT;

	return GCN_ERR_OK;
}

static GcnError decodevintrp(GcnInstruction* outinstr, uint32_t instr) {
	const uint8_t op = vintrp_op(instr);
	const uint16_t vsrc = vintrp_vsrc(instr);
	const uint8_t vdst = vintrp_vdst(instr);
	const uint8_t attr = vintrp_attr(instr);
	const uint8_t attrchan = vintrp_attrchan(instr);

	if (!parseopcode_vintrp(&outinstr->vintrp.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->srcs[0].field = GCN_OPFIELD_VGPR_0 + vsrc;
	outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdst;

	outinstr->numsrcs = 1;
	outinstr->numdsts = 1;

	outinstr->srcs[0].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_FLOAT;
	outinstr->dsts[0].type = GCN_DT_FLOAT;

	outinstr->vintrp.attr = attr;
	outinstr->vintrp.attrchan = attrchan;

	return GCN_ERR_OK;
}

static GcnError decodevop1(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr, uint32_t instr,
    uint32_t remainingsize
) {
	const uint8_t op = vop1_op(instr);
	const uint16_t src0 = vop1_src0(instr);
	const uint8_t vdst = vop1_vdst(instr);

	if (!parseopcode_vop1(&outinstr->vop1.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	if (!parseoperandfield(&outinstr->srcs[0].field, src0)) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdst;

	outinstr->numsrcs = 1;
	outinstr->numdsts = 1;

	if (outinstr->vop1.opcode == GCN_V_CLREXCP ||
	    outinstr->vop1.opcode == GCN_V_NOP) {
		outinstr->numsrcs = 0;
		outinstr->numdsts = 0;
	}

	outinstr->srcs[0].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_BIN;
	outinstr->dsts[0].type = GCN_DT_BIN;

	const InstrTypes types = getinstrtypes(outinstr);
	if (types.numtypes == 1) {
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[0];
		outinstr->dsts[0].type = types.types[0];
	} else if (types.numtypes == 2) {
		outinstr->srcs[0].numbits = types.bits[1];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[1];
		outinstr->dsts[0].type = types.types[0];
	} else {
		// unexpected number of types
		return GCN_ERR_INTERNAL_ERROR;
	}

	if (outinstr->vop1.opcode == GCN_V_CVT_OFF_F32_I4) {
	}

	if (outinstr->srcs[0].field == GCN_OPFIELD_LITERAL_CONST) {
		if (remainingsize < sizeof(uint32_t) * 2) {
			return GCN_ERR_CODE_TOO_SMALL;
		}

		outinstr->srcs[0].constant =
		    *(const uint32_t*)(ctx->curinstr + sizeof(uint32_t));

		outinstr->length += sizeof(uint32_t);
	}

	return GCN_ERR_OK;
}

static GcnError decodevop2(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr, uint32_t instr,
    uint32_t remainingsize
) {
	const uint8_t op = vop2_op(instr);
	const uint16_t src0 = vop2_src0(instr);
	const uint8_t vsrc1 = vop2_vsrc1(instr);
	const uint8_t vdst = vop2_vdst(instr);

	if (!isvalidvop2(op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	outinstr->vop2.opcode = op;

	if (outinstr->vop2.opcode == GCN_V_WRITELANE_B32) {
		outinstr->srcs[0].field = GCN_OPFIELD_SGPR_0 + src0;
	} else {
		outinstr->srcs[1].field = GCN_OPFIELD_VGPR_0 + vsrc1;
		if (!parseoperandfield(&outinstr->srcs[0].field, src0)) {
			return GCN_ERR_INVALID_SRC_FIELD;
		}
	}
	outinstr->srcs[1].field = GCN_OPFIELD_VGPR_0 + vsrc1;
	if (outinstr->vop2.opcode == GCN_V_READLANE_B32) {
		outinstr->dsts[0].field = GCN_OPFIELD_SGPR_0 + vdst;
	} else {
		outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdst;
	}

	outinstr->numsrcs = 2;
	outinstr->numdsts = 1;

	outinstr->srcs[0].numbits = 32;
	outinstr->srcs[1].numbits = 32;
	outinstr->dsts[0].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_BIN;
	outinstr->srcs[1].type = GCN_DT_BIN;
	outinstr->dsts[0].type = GCN_DT_BIN;

	const InstrTypes types = getinstrtypes(outinstr);
	if (types.numtypes == 1) {
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->srcs[1].numbits = types.bits[0];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[0];
		outinstr->srcs[1].type = types.types[0];
		outinstr->dsts[0].type = types.types[0];
	} else if (types.numtypes == 2) {
		outinstr->srcs[0].numbits = types.bits[1];
		outinstr->srcs[1].numbits = types.bits[1];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[1];
		outinstr->srcs[1].type = types.types[1];
		outinstr->dsts[0].type = types.types[0];
	} else {
		// unexpected number of types
		return GCN_ERR_INTERNAL_ERROR;
	}

	switch (outinstr->vop2.opcode) {
	case GCN_V_ADDC_U32:
	case GCN_V_CNDMASK_B32:
	case GCN_V_SUBB_U32:
	case GCN_V_SUBBREV_U32:
		outinstr->srcs[2].field = GCN_OPFIELD_VCC_LO;
		outinstr->srcs[2].numbits = 64;
		outinstr->srcs[2].type = GCN_DT_BIN;
		outinstr->numsrcs = 3;
		break;
	case GCN_V_MADAK_F32:
		outinstr->srcs[2].field = GCN_OPFIELD_LITERAL_CONST;
		outinstr->srcs[2].numbits = 32;
		outinstr->srcs[2].type = GCN_DT_FLOAT;
		outinstr->numsrcs = 3;
		break;
	case GCN_V_MADMK_F32:
		outinstr->srcs[2] = outinstr->srcs[1];
		// actual constant gets written at the end
		outinstr->srcs[1].field = GCN_OPFIELD_LITERAL_CONST;
		outinstr->srcs[1].numbits = 32;
		outinstr->srcs[1].type = GCN_DT_FLOAT;
		outinstr->numsrcs = 3;
		break;
	case GCN_V_CVT_PKRTZ_F16_F32:
		// workaround for emulator, and this is probably a more accurate
		// representation of this data
		outinstr->dsts[0].type = GCN_DT_BIN;
		break;
	default:
		break;
	}
	switch (outinstr->vop2.opcode) {
	case GCN_V_ADD_I32:
	case GCN_V_ADDC_U32:
	case GCN_V_SUB_I32:
	case GCN_V_SUBB_U32:
	case GCN_V_SUBBREV_U32:
	case GCN_V_SUBREV_I32:
		outinstr->dsts[1].field = GCN_OPFIELD_VCC_LO;
		outinstr->dsts[1].numbits = 64;
		outinstr->dsts[1].type = GCN_DT_BIN;
		outinstr->numdsts = 2;
		break;
	default:
		break;
	}

	for (uint8_t i = 0; i < uasize(outinstr->srcs); i += 1) {
		GcnOperandFieldInfo* fi = &outinstr->srcs[i];
		if (fi->field == GCN_OPFIELD_LITERAL_CONST) {
			if (remainingsize < sizeof(uint32_t) * 2) {
				return GCN_ERR_CODE_TOO_SMALL;
			}

			fi->constant = *(const uint32_t*)(ctx->curinstr +
							  sizeof(uint32_t));
			outinstr->length += sizeof(uint32_t);
		}
	}

	return GCN_ERR_OK;
}

static GcnError decodevop3(
    const GcnDecoderContext* ctx, GcnInstruction* outinstr,
    uint32_t remainingsize
) {
	if (remainingsize < sizeof(uint64_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}

	const uint64_t instr64 = *(const uint64_t*)ctx->curinstr;

	const uint16_t op = vop3_op(instr64);
	const uint16_t src0 = vop3_src0(instr64);
	const uint16_t src1 = vop3_src1(instr64);
	const uint16_t src2 = vop3_src2(instr64);
	const uint8_t vdst = vop3_vdst(instr64);
	const uint8_t omod = vop3_omod(instr64);
	const uint8_t neg = vop3_neg(instr64);

	if (!parseopcode_vop3(&outinstr->vop3.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	if (outinstr->vop3.opcode == GCN_V_MOVRELS_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MOVRELSD_B32_E64) {
		outinstr->srcs[0].field = GCN_OPFIELD_VGPR_0 + src0;
	} else {
		if (!parseoperandfield(&outinstr->srcs[0].field, src0)) {
			return GCN_ERR_INVALID_SRC_FIELD;
		}
	}
	if (!parseoperandfield(&outinstr->srcs[1].field, src1)) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	if (outinstr->vop3.opcode == GCN_V_ADDC_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CNDMASK_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBB_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBBREV_U32_E64) {
		outinstr->srcs[2].field = GCN_OPFIELD_SGPR_0 + src2;
	} else {
		if (!parseoperandfield(&outinstr->srcs[2].field, src2)) {
			return GCN_ERR_INVALID_SRC_FIELD;
		}
	}

	if (outinstr->vop3.opcode <= GCN_V_CMPX_TRU_U64_E64) {
		if (!parseoperandfield(&outinstr->dsts[0].field, vdst)) {
			return GCN_ERR_INVALID_DST_FIELD;
		}
	} else {
		outinstr->dsts[0].field = GCN_OPFIELD_VGPR_0 + vdst;
	}

	if (outinstr->vop3.opcode == GCN_V_DIV_SCALE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_SCALE_F64_E64) {
		outinstr->dsts[1].field = GCN_OPFIELD_VCC_LO;
	}

	outinstr->numsrcs = 2;
	outinstr->numdsts = 1;

	outinstr->srcs[0].numbits = 32;
	outinstr->srcs[1].numbits = 32;
	outinstr->srcs[2].numbits = 32;
	outinstr->dsts[0].numbits = 32;
	outinstr->dsts[1].numbits = 32;

	outinstr->srcs[0].type = GCN_DT_BIN;
	outinstr->srcs[1].type = GCN_DT_BIN;
	outinstr->srcs[2].type = GCN_DT_BIN;
	outinstr->dsts[0].type = GCN_DT_BIN;
	outinstr->dsts[1].type = GCN_DT_BIN;

	const InstrTypes types = getinstrtypes(outinstr);
	if (types.numtypes == 1) {
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->srcs[1].numbits = types.bits[0];
		outinstr->srcs[2].numbits = types.bits[0];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->dsts[1].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[0];
		outinstr->srcs[1].type = types.types[0];
		outinstr->srcs[2].type = types.types[0];
		outinstr->dsts[0].type = types.types[0];
		outinstr->dsts[1].type = types.types[0];
	} else if (types.numtypes == 2) {
		outinstr->srcs[0].numbits = types.bits[1];
		outinstr->srcs[1].numbits = types.bits[1];
		outinstr->srcs[2].numbits = types.bits[1];
		outinstr->dsts[0].numbits = types.bits[0];
		outinstr->dsts[1].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[1];
		outinstr->srcs[1].type = types.types[1];
		outinstr->srcs[2].type = types.types[1];
		outinstr->dsts[0].type = types.types[0];
		outinstr->dsts[1].type = types.types[0];
	} else {
		// unexpected number of types
		return GCN_ERR_INTERNAL_ERROR;
	}

	if (outinstr->vop3.opcode == GCN_V_CLREXCP_E64 ||
	    outinstr->vop3.opcode == GCN_V_NOP_E64) {
		outinstr->numsrcs = 0;
		outinstr->numdsts = 0;
	}

	if (outinstr->vop3.opcode == GCN_V_CVT_PKRTZ_F16_F32_E64) {
		// same workaround found in VOP2's variant
		outinstr->dsts[0].type = GCN_DT_BIN;
	}

	if (outinstr->vop3.opcode == GCN_V_BFREV_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CEIL_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CEIL_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_COS_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F16_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_F16_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_UBYTE0_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_UBYTE1_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_UBYTE2_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F32_UBYTE3_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F64_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F64_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F64_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_FLR_I32_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_I32_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_I32_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_OFF_F32_I4_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_RPI_I32_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_U32_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_U32_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_EXP_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_EXP_LEGACY_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FFBH_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FFBH_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FFBL_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FLOOR_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FLOOR_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_FRACT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FRACT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_FREXP_EXP_I32_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FREXP_EXP_I32_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_FREXP_MANT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FREXP_MANT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_LOG_CLAMP_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_LOG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_LOG_LEGACY_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MOV_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MOV_FED_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MOVRELD_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_NOT_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_CLAMP_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_CLAMP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_IFLAG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_LEGACY_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RNDNE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RNDNE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RSQ_CLAMP_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RSQ_CLAMP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RSQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_RSQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RSQ_LEGACY_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SIN_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SQRT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SQRT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_TRUNC_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_TRUNC_F64_E64) {
		outinstr->numsrcs = 1;
	}
	if (outinstr->vop3.opcode == GCN_V_ADDC_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CNDMASK_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBB_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBBREV_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_ALIGNBIT_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_ALIGNBYTE_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_BFE_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_BFE_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_BFI_B32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CUBEID_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CUBEMA_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CUBESC_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CUBETC_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_PK_U8_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_FIXUP_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_FIXUP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_FMAS_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_FMAS_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_FMA_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_FMA_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_LERP_U8_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAD_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAD_I32_I24_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAD_I64_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAD_LEGACY_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAD_U32_U24_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAD_U64_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAX3_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAX3_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAX3_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MED3_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MED3_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MED3_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MIN3_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MIN3_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MIN3_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MQSAD_PK_U16_U8_E64 ||
	    outinstr->vop3.opcode == GCN_V_MQSAD_U32_U8_E64 ||
	    outinstr->vop3.opcode == GCN_V_MSAD_U8_E64 ||
	    outinstr->vop3.opcode == GCN_V_MULLIT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_QSAD_PK_U16_U8_E64 ||
	    outinstr->vop3.opcode == GCN_V_SAD_HI_U8_E64 ||
	    outinstr->vop3.opcode == GCN_V_SAD_U16_E64 ||
	    outinstr->vop3.opcode == GCN_V_SAD_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SAD_U8_E64) {
		outinstr->numsrcs = 3;
	}
	if (outinstr->vop3.opcode == GCN_V_ADD_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUB_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBREV_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_ADDC_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBB_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBBREV_U32_E64) {
		outinstr->numdsts = 2;
	}
	if (outinstr->vop3.opcode == GCN_V_DIV_SCALE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_SCALE_F64_E64) {
		outinstr->numsrcs = 3;
		outinstr->numdsts = 2;
	}

	if (outinstr->vop3.opcode == GCN_V_FREXP_EXP_I32_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_I32_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_U32_F64_E64) {
		outinstr->srcs[0].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_CNDMASK_B32_E64) {
		outinstr->srcs[2].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_CMP_CLASS_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_CLASS_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_EQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_F_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NEQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NGE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NGT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NLE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NLG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NLT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_O_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_TRU_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_U_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_EQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_F_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_GE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_GT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_LE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_LG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_LT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NEQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NGE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NGT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NLE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NLG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NLT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_O_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_TRU_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_U_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_EQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_F_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_GE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_GT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_LE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_LG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_LT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NEQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NGE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NGT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NLE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NLG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NLT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_O_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_TRU_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_U_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_EQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_F_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NEQ_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NGE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NGT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NLE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NLG_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NLT_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_O_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_TRU_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_U_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_EQ_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_F_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GE_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GT_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LE_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LG_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LT_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_TRU_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_EQ_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_F_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GE_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GT_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LE_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LG_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LT_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_TRU_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_EQ_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_F_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GE_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GT_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LE_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LG_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LT_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_TRU_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_EQ_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_F_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GE_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GT_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LE_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LG_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LT_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_TRU_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F64_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_CVT_F64_U32_E64) {
		outinstr->dsts[0].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_ADD_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_SCALE_F32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUB_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBREV_I32_E64) {
		outinstr->dsts[1].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_ADDC_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBB_U32_E64 ||
	    outinstr->vop3.opcode == GCN_V_SUBBREV_U32_E64) {
		outinstr->srcs[2].numbits = 64;
		outinstr->dsts[1].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_ASHR_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CEIL_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_CLASS_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_CLASS_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_FLOOR_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_FRACT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_FREXP_MANT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_LDEXP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_LSHL_B64_E64 ||
	    outinstr->vop3.opcode == GCN_V_LSHR_B64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_CLAMP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RCP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RSQ_CLAMP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RSQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_RNDNE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_SQRT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_TRIG_PREOP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_TRUNC_F64_E64) {
		outinstr->srcs[0].numbits = 64;
		outinstr->dsts[0].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_ADD_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_EQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_F_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NEQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NGE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NGT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NLE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NLG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_NLT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_O_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_TRU_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_U_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_EQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_F_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_GE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_GT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_LE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_LG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_LT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NEQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NGE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NGT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NLE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NLG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_NLT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_O_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_TRU_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPS_U_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_EQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_F_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_GE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_GT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_LE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_LG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_LT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NEQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NGE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NGT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NLE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NLG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_NLT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_O_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_TRU_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPSX_U_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_EQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_F_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NEQ_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NGE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NGT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NLE_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NLG_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_NLT_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_O_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_TRU_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_U_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_EQ_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_F_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GE_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GT_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LE_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LG_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LT_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_TRU_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_EQ_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_F_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GE_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GT_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LE_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LG_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LT_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_TRU_I64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_EQ_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_F_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GE_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_GT_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LE_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LG_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_LT_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMP_TRU_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_EQ_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_F_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GE_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_GT_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LE_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LG_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_LT_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_CMPX_TRU_U64_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAX_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_MIN_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_MUL_F64_E64) {
		outinstr->srcs[0].numbits = 64;
		outinstr->srcs[1].numbits = 64;
		outinstr->dsts[0].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_FMA_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_FIXUP_F64_E64 ||
	    outinstr->vop3.opcode == GCN_V_DIV_FMAS_F64_E64) {
		outinstr->srcs[0].numbits = 64;
		outinstr->srcs[1].numbits = 64;
		outinstr->srcs[2].numbits = 64;
		outinstr->dsts[0].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_MQSAD_PK_U16_U8_E64 ||
	    outinstr->vop3.opcode == GCN_V_QSAD_PK_U16_U8_E64) {
		outinstr->srcs[0].numbits = 64;
		outinstr->srcs[2].numbits = 64;
		outinstr->dsts[0].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_MQSAD_U32_U8_E64) {
		outinstr->srcs[0].numbits = 128;
		outinstr->srcs[2].numbits = 128;
		outinstr->dsts[0].numbits = 128;
	}
	if (outinstr->vop3.opcode == GCN_V_MAD_I64_I32_E64 ||
	    outinstr->vop3.opcode == GCN_V_MAD_U64_U32_E64) {
		outinstr->srcs[2].numbits = 64;
		outinstr->dsts[0].numbits = 64;
		outinstr->dsts[1].numbits = 64;
	}
	if (outinstr->vop3.opcode == GCN_V_DIV_SCALE_F64_E64) {
		outinstr->srcs[0].numbits = 64;
		outinstr->srcs[1].numbits = 64;
		outinstr->srcs[2].numbits = 64;
		outinstr->dsts[0].numbits = 64;
		outinstr->dsts[1].numbits = 64;
	}

	if (strstr(gcnStrOpcodeVOP3(outinstr->vop3.opcode), "v_cmp")) {
		outinstr->dsts[0].type = GCN_DT_UINT;
	}

	if (isvop3b(op)) {
		const uint8_t sdst = vop3b_sdst(instr64);
		outinstr->dsts[1].field = GCN_OPFIELD_SGPR_0 + sdst;
	} else {
		const uint8_t abs = vop3a_abs(instr64);
		const uint8_t clmp = vop3a_clmp(instr64);

		for (uint8_t i = 0; i < umin(outinstr->numsrcs, 3); i += 1) {
			outinstr->srcs[i].absolute = ((abs >> i) & 0x1) == 1;
		}
		outinstr->dsts[0].clamp = clmp == 1;
	}

	switch (omod) {
	case 0:
		outinstr->dsts[0].outputmodifier = GCN_OMOD_NONE;
		break;
	case 1:
		outinstr->dsts[0].outputmodifier = GCN_OMOD_MUL_2;
		break;
	case 2:
		outinstr->dsts[0].outputmodifier = GCN_OMOD_MUL_4;
		break;
	case 3:
		outinstr->dsts[0].outputmodifier = GCN_OMOD_MUL_0_5;
		break;
	default:
		return GCN_ERR_INVALID_ARG;
	}

	for (uint8_t i = 0; i < umin(outinstr->numsrcs, 3); i += 1) {
		outinstr->srcs[i].negative = ((neg >> i) & 0x1) == 1;
	}

	return GCN_ERR_OK;
}

static GcnError decodevopc(GcnInstruction* outinstr, uint32_t instr) {
	const uint8_t op = vopc_op(instr);
	const uint16_t src0 = vopc_src0(instr);
	const uint8_t vsrc1 = vopc_vsrc1(instr);

	if (!parseopcode_vopc(&outinstr->vopc.opcode, op)) {
		return GCN_ERR_INVALID_OPCODE;
	}

	if (!parseoperandfield(&outinstr->srcs[0].field, src0)) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	outinstr->srcs[1].field = GCN_OPFIELD_VGPR_0 + vsrc1;
	outinstr->dsts[0].field = GCN_OPFIELD_VCC_LO;

	outinstr->numsrcs = 2;
	outinstr->numdsts = 1;

	outinstr->srcs[0].numbits = 32;
	outinstr->srcs[1].numbits = 32;
	outinstr->dsts[0].numbits = 64;

	outinstr->srcs[0].type = GCN_DT_BIN;
	outinstr->srcs[1].type = GCN_DT_BIN;
	outinstr->dsts[0].type = GCN_DT_BIN;

	const InstrTypes types = getinstrtypes(outinstr);
	if (types.numtypes == 1) {
		outinstr->srcs[0].numbits = types.bits[0];
		outinstr->srcs[1].numbits = types.bits[0];
		outinstr->srcs[0].type = types.types[0];
		outinstr->srcs[1].type = types.types[0];
	} else {
		// unexpected number of types
		return GCN_ERR_INTERNAL_ERROR;
	}

	return GCN_ERR_OK;
}

GcnError gcnDecodeInstruction(
    GcnDecoderContext* ctx, GcnInstruction* outinstr
) {
	if (!ctx || !outinstr) {
		return GCN_ERR_INVALID_ARG;
	}
	if (!ctx->codestart || !ctx->curinstr || !ctx->codesize) {
		return GCN_ERR_UNINITIALIZED_CONTEXT;
	}

	const uint32_t curoffset = ctx->curinstr - ctx->codestart;
	const uint32_t remainingsize = ctx->codesize - curoffset;

	if (!remainingsize) {
		return GCN_ERR_END_OF_CODE;
	}
	if (remainingsize < sizeof(uint32_t)) {
		return GCN_ERR_CODE_TOO_SMALL;
	}

	const uint32_t instr = *(const uint32_t*)ctx->curinstr;
	GcnMicrocode microcode = parsemicrocode(instr);
	if (microcode == GCN_MICROCODE_INVALID) {
		return GCN_ERR_INVALID_MICROCODE;
	}

	uint32_t instrlen = 0;
	if (!getmicrocodelen(&instrlen, microcode)) {
		return GCN_ERR_INTERNAL_ERROR;
	}

	outinstr->microcode = microcode;
	outinstr->offset = curoffset;
	outinstr->length = instrlen;

	GcnError gerr = GCN_ERR_OK;
	switch (microcode) {
	case GCN_MICROCODE_DS:
		gerr = decode_ds(ctx, outinstr, remainingsize);
		break;
	case GCN_MICROCODE_EXP:
		gerr = decodeexp(ctx, outinstr, remainingsize);
		break;
	case GCN_MICROCODE_MIMG:
		gerr = decodemimg(ctx, outinstr, remainingsize);
		break;
	case GCN_MICROCODE_MTBUF:
		gerr = decodemtbuf(ctx, outinstr, remainingsize);
		break;
	case GCN_MICROCODE_MUBUF:
		gerr = decodemubuf(ctx, outinstr, remainingsize);
		break;
	case GCN_MICROCODE_SMRD:
		gerr = decodesmrd(outinstr, instr);
		break;
	case GCN_MICROCODE_SOP1:
		gerr = decodesop1(ctx, outinstr, instr, remainingsize);
		break;
	case GCN_MICROCODE_SOP2:
		gerr = decodesop2(ctx, outinstr, instr, remainingsize);
		break;
	case GCN_MICROCODE_SOPC:
		gerr = decodesopc(outinstr, instr);
		break;
	case GCN_MICROCODE_SOPK:
		gerr = decodesopk(outinstr, instr);
		break;
	case GCN_MICROCODE_SOPP:
		gerr = decodesopp(outinstr, instr);
		break;
	case GCN_MICROCODE_VINTRP:
		gerr = decodevintrp(outinstr, instr);
		break;
	case GCN_MICROCODE_VOP1:
		gerr = decodevop1(ctx, outinstr, instr, remainingsize);
		break;
	case GCN_MICROCODE_VOP2:
		gerr = decodevop2(ctx, outinstr, instr, remainingsize);
		break;
	case GCN_MICROCODE_VOP3:
		gerr = decodevop3(ctx, outinstr, remainingsize);
		break;
	case GCN_MICROCODE_VOPC:
		gerr = decodevopc(outinstr, instr);
		break;
	case GCN_MICROCODE_INVALID:
		gerr = GCN_ERR_INVALID_MICROCODE;
		break;
	default:
		// All known microcodes are handled above. This fires only
		// for future/unknown microcode types.
		gerr = GCN_ERR_UNIMPLEMENTED;
	}

	if (gerr != GCN_ERR_OK) {
		return gerr;
	}

	ctx->curinstr += outinstr->length;
	return GCN_ERR_OK;
}
