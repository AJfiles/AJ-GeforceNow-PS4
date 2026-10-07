#include "gcn.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gnm_strings.h"

#include "u/utility.h"

static GcnError doformat(
    char* outbuf, size_t outbufsize, const char* fmt, ...
) {
	va_list va = {0};

	va_start(va, fmt);
	const int actualsize = vsnprintf(NULL, 0, fmt, va);
	va_end(va);

	if (actualsize < 0 || (size_t)actualsize + 1 > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	va_start(va, fmt);
	vsnprintf(outbuf, outbufsize, fmt, va);
	va_end(va);

	va_end(va);
	return GCN_ERR_OK;
}

static GcnError stropfield(
    char* outbuf, size_t outbufsize, const GcnOperandFieldInfo* fieldinfo
) {
	const GcnOperandField field = fieldinfo->field;
	const uint32_t numdwords = fieldinfo->numbits / 32;

	if (field >= GCN_OPFIELD_SGPR_0 && field <= GCN_OPFIELD_SGPR_103) {
		const char* fmt = NULL;
		if (numdwords > 1) {
			fmt = "s[%u:%u]";
		} else {
			fmt = "s%u";
		}

		const uint32_t index = field - GCN_OPFIELD_SGPR_0;
		return doformat(
		    outbuf, outbufsize, fmt, index, index + numdwords - 1
		);
	}
	if (field >= GCN_OPFIELD_VGPR_0 && field <= GCN_OPFIELD_VGPR_255) {
		const char* fmt = NULL;
		if (numdwords > 1) {
			fmt = "v[%u:%u]";
		} else {
			fmt = "v%u";
		}

		const uint32_t index = field - GCN_OPFIELD_VGPR_0;
		return doformat(
		    outbuf, outbufsize, fmt, index, index + numdwords - 1
		);
	}
	if (field >= GCN_OPFIELD_TTMP_0 && field <= GCN_OPFIELD_TTMP_11) {
		const uint32_t index = field - GCN_OPFIELD_TTMP_0;
		return doformat(outbuf, outbufsize, "ttmp%u", index);
	}
	if (field >= GCN_OPFIELD_CONST_INT_0 &&
	    field <= GCN_OPFIELD_CONST_INT_64) {
		const uint32_t num = field - GCN_OPFIELD_CONST_INT_0;
		return doformat(outbuf, outbufsize, "%u", num);
	}
	if (field >= GCN_OPFIELD_CONST_INT_NEG_1 &&
	    field <= GCN_OPFIELD_CONST_INT_NEG_16) {
		const int32_t num = GCN_OPFIELD_CONST_INT_64 - field;
		return doformat(outbuf, outbufsize, "%i", num);
	}

	const char* src = NULL;
	switch (field) {
	// Vector Condition Code (VCC) mask
	case GCN_OPFIELD_VCC_LO:
		src = numdwords == 2 ? "vcc" : "vcc_lo";
		break;
	case GCN_OPFIELD_VCC_HI:
		src = "vcc_hi";
		break;
	// Trap handler base address
	case GCN_OPFIELD_TBA_LO:
		src = numdwords == 2 ? "tba" : "tba_lo";
		break;
	case GCN_OPFIELD_TBA_HI:
		src = "tba_hi";
		break;
	// Pointer to data in memory used by trap handler
	case GCN_OPFIELD_TMA_LO:
		src = numdwords == 2 ? "tma" : "tma_lo";
		break;
	case GCN_OPFIELD_TMA_HI:
		src = "tma_hi";
		break;

	// Temporary memory register
	case GCN_OPFIELD_M0:
		src = "m0";
		break;
	// EXEC mask
	case GCN_OPFIELD_EXEC_LO:
		src = numdwords == 2 ? "exec" : "exec_lo";
		break;
	case GCN_OPFIELD_EXEC_HI:
		src = "exec_hi";
		break;

	// Floating point immediates
	case GCN_OPFIELD_CONST_FLOAT_0_5:
		src = "0.5";
		break;
	case GCN_OPFIELD_CONST_FLOAT_NEG_0_5:
		src = "-0.5";
		break;
	case GCN_OPFIELD_CONST_FLOAT_1:
		src = "1.0";
		break;
	case GCN_OPFIELD_CONST_FLOAT_NEG_1:
		src = "-1.0";
		break;
	case GCN_OPFIELD_CONST_FLOAT_2:
		src = "2.0";
		break;
	case GCN_OPFIELD_CONST_FLOAT_NEG_2:
		src = "-2.0";
		break;
	case GCN_OPFIELD_CONST_FLOAT_4:
		src = "4.0";
		break;
	case GCN_OPFIELD_CONST_FLOAT_NEG_4:
		src = "-4.0";
		break;

	case GCN_OPFIELD_VCCZ:
		src = "vccz";
		break;
	case GCN_OPFIELD_EXECZ:
		src = "execz";
		break;
	case GCN_OPFIELD_SCC:
		src = "scc";
		break;

		// 32-bit constant from instruction stream
	case GCN_OPFIELD_LITERAL_CONST:
		return doformat(
		    outbuf, outbufsize, "#0x%08x", fieldinfo->constant
		);
	default:
		return GCN_ERR_INVALID_ARG;
	}

	return doformat(outbuf, outbufsize, "%s", src);
}

static inline GcnError concatoperand(
    char* outbuf, size_t outbufsize, const GcnOperandFieldInfo* info,
    bool commasep
) {
	char strfield[64] = {0};
	GcnError gerr = stropfield(strfield, uasize(strfield), info);
	if (gerr != GCN_ERR_OK) {
		return gerr;
	}

	const char* fmt = NULL;
	if (commasep) {
		if (info->negative && info->absolute) {
			fmt = ", -abs(%s)";
		} else if (info->negative) {
			fmt = ", -%s";
		} else if (info->absolute) {
			fmt = ", abs(%s)";
		} else {
			fmt = ", %s";
		}
	} else {
		if (info->negative && info->absolute) {
			fmt = " -abs(%s)";
		} else if (info->negative) {
			fmt = " -%s";
		} else if (info->absolute) {
			fmt = " abs(%s)";
		} else {
			fmt = " %s";
		}
	}

	char fmtfield[64] = {0};
	snprintf(fmtfield, sizeof(fmtfield), fmt, strfield);

	if (u_strlcat(outbuf, fmtfield, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	return GCN_ERR_OK;
}

static GcnError genericformat(
    const GcnInstruction* instr, const char* strop, char* outbuf,
    size_t outbufsize
) {
	if (instr->numsrcs > uasize(instr->srcs) ||
	    instr->numdsts > uasize(instr->dsts)) {
		return GCN_ERR_INVALID_ARG;
	}

	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	uint8_t curinstr = 0;
	for (uint8_t i = 0; i < instr->numdsts; i += 1, curinstr += 1) {
		const bool usecomma = curinstr > 0;
		GcnError gerr = concatoperand(
		    outbuf, outbufsize, &instr->dsts[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
	}

	bool hasconstsrc = false;
	uint32_t constval = 0;
	for (uint8_t i = 0; i < instr->numsrcs; i += 1, curinstr += 1) {
		const GcnOperandFieldInfo* src = &instr->srcs[i];

		if (src->field == GCN_OPFIELD_LITERAL_CONST) {
			if (hasconstsrc) {
				return GCN_ERR_TOO_MANY_CONSTS;
			}
			hasconstsrc = true;
			constval = src->constant;
		}

		const bool usecomma = curinstr > 0;
		GcnError gerr =
		    concatoperand(outbuf, outbufsize, src, usecomma);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
	}

	if (instr->microcode == GCN_MICROCODE_VOP3) {
		switch (instr->dsts[0].outputmodifier) {
		case GCN_OMOD_NONE:
			// nothing to output
			break;
		case GCN_OMOD_MUL_2:
			if (u_strlcat(outbuf, " mul:2", outbufsize) >
			    outbufsize) {
				return GCN_ERR_BUFFER_TOO_SMALL;
			}
			break;
		case GCN_OMOD_MUL_4:
			if (u_strlcat(outbuf, " mul:4", outbufsize) >
			    outbufsize) {
				return GCN_ERR_BUFFER_TOO_SMALL;
			}
			break;
		case GCN_OMOD_MUL_0_5:
			if (u_strlcat(outbuf, " div:2", outbufsize) >
			    outbufsize) {
				return GCN_ERR_BUFFER_TOO_SMALL;
			}
			break;
		default:
			return GCN_ERR_INTERNAL_ERROR;
		}
	}

	if (instr->dsts[0].clamp) {
		if (u_strlcat(outbuf, " clamp", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	if (hasconstsrc) {
		char conststr[64] = {0};
		snprintf(
		    conststr, sizeof(conststr), " // %f = %i", uif(constval),
		    constval
		);

		if (u_strlcat(outbuf, conststr, outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	return GCN_ERR_OK;
}

static GcnError format_ds(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	const char* strop = gcnStrOpcodeDS(instr->ds.opcode);
	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	bool usecomma = false;
	GcnError gerr = GCN_ERR_OK;
	for (uint8_t i = 0; i < instr->numdsts; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->dsts[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}
	for (uint8_t i = 0; i < instr->numsrcs; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->srcs[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}

	char offbuf[32] = {0};
	snprintf(
	    offbuf, sizeof(offbuf), " offset:0x%04x",
	    instr->ds.offsets[0] | (instr->ds.offsets[1] << 8)
	);
	if (u_strlcat(outbuf, offbuf, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	return GCN_ERR_OK;
}

static GcnError expformat(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	if (u_strlcat(outbuf, "exp ", outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	if (u_strlcat(outbuf, gcnStrExpTarget(instr->exp.tgt), outbufsize) >
	    outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	for (uint8_t i = 0; i < instr->numsrcs; i += 1) {
		GcnError gerr =
		    concatoperand(outbuf, outbufsize, &instr->srcs[i], true);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
	}

	if (instr->exp.compressed) {
		if (u_strlcat(outbuf, " compr", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->exp.validmask) {
		if (u_strlcat(outbuf, " vm", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->exp.done) {
		if (u_strlcat(outbuf, " done", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	return GCN_ERR_OK;
}

static GcnError mimgformat(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	const char* strop = gcnStrOpcodeMIMG(instr->mimg.opcode);
	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	bool usecomma = false;
	GcnError gerr = GCN_ERR_OK;
	for (uint8_t i = 0; i < instr->numdsts; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->dsts[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}
	for (uint8_t i = 0; i < instr->numsrcs; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->srcs[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}

	if (instr->mimg.dmask) {
		char maskbuf[32] = {0};
		snprintf(
		    maskbuf, sizeof(maskbuf), " dmask:0x%01x", instr->mimg.dmask
		);
		if (u_strlcat(outbuf, maskbuf, outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mimg.unnormalized) {
		if (u_strlcat(outbuf, " unorm", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mimg.globalcoherency) {
		if (u_strlcat(outbuf, " glc", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mimg.syslevelcoherent) {
		if (u_strlcat(outbuf, " slc", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mimg.lodwarning) {
		if (u_strlcat(outbuf, " lwe", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mimg.declarearray) {
		if (u_strlcat(outbuf, " da", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	return GCN_ERR_OK;
}

static GcnError formatmtbuf(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	const char* strop = gcnStrOpcodeMTBUF(instr->mtbuf.opcode);
	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	bool usecomma = false;
	GcnError gerr = GCN_ERR_OK;
	for (uint8_t i = 0; i < instr->numdsts; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->dsts[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}
	for (uint8_t i = 0; i < instr->numsrcs; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->srcs[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}

	if (instr->mtbuf.hasoffset) {
		char offbuf[32] = {0};
		snprintf(
		    offbuf, sizeof(offbuf), " offset:0x%04x",
		    instr->mtbuf.offset
		);
		if (u_strlcat(outbuf, offbuf, outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mtbuf.hasoffset) {
		if (u_strlcat(outbuf, " offen", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mtbuf.hasindex) {
		if (u_strlcat(outbuf, " idxen", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mtbuf.syslevelcoherent) {
		if (u_strlcat(outbuf, " slc", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	const char* dfmt = gnmStrSurfaceFormat(instr->mtbuf.dfmt);
	const char* nfmt = gnmStrTexChannelType(instr->mtbuf.nfmt);
	char fmtbuf[64] = {0};
	snprintf(fmtbuf, sizeof(fmtbuf), " format:[%s,%s]", dfmt, nfmt);
	if (u_strlcat(outbuf, fmtbuf, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	return GCN_ERR_OK;
}

static GcnError formatmubuf(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	const char* strop = gcnStrOpcodeMUBUF(instr->mubuf.opcode);
	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	bool usecomma = false;
	GcnError gerr = GCN_ERR_OK;
	for (uint8_t i = 0; i < instr->numdsts; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->dsts[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}
	for (uint8_t i = 0; i < instr->numsrcs; i += 1) {
		gerr = concatoperand(
		    outbuf, outbufsize, &instr->srcs[i], usecomma
		);
		if (gerr != GCN_ERR_OK) {
			return gerr;
		}
		usecomma = true;
	}

	if (instr->mubuf.hasoffset) {
		char offbuf[32] = {0};
		snprintf(
		    offbuf, sizeof(offbuf), " offset:0x%04x",
		    instr->mubuf.offset
		);
		if (u_strlcat(outbuf, offbuf, outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mubuf.hasoffset) {
		if (u_strlcat(outbuf, " offen", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mubuf.hasindex) {
		if (u_strlcat(outbuf, " idxen", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}
	if (instr->mubuf.syslevelcoherent) {
		if (u_strlcat(outbuf, " slc", outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	return GCN_ERR_OK;
}

static GcnError smrdformat(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	const char* strop = gcnStrOpcodeSMRD(instr->smrd.opcode);
	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	char srcbuf[128] = {0};

	if (instr->numdsts == 1) {
		const GcnOperandFieldInfo* dst0 = &instr->dsts[0];
		const uint32_t numdwords = dst0->numbits / 32;
		if (numdwords > 1) {
			snprintf(
			    srcbuf, sizeof(srcbuf), " s[%u:%u]", dst0->field,
			    dst0->field + numdwords - 1
			);
		} else {
			snprintf(srcbuf, sizeof(srcbuf), " s%u", dst0->field);
		}
		if (u_strlcat(outbuf, srcbuf, outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	if (instr->numsrcs == 2) {
		const GcnOperandFieldInfo* src0 = &instr->srcs[0];
		const uint32_t numdwords = src0->numbits / 32;
		if (numdwords > 1) {
			snprintf(
			    srcbuf, sizeof(srcbuf), ", s[%u:%u], 0x%02x",
			    src0->field, src0->field + numdwords - 1,
			    instr->srcs[1].constant
			);
		} else {
			snprintf(
			    srcbuf, sizeof(srcbuf), ", s%u, 0x%02x",
			    src0->field, instr->srcs[1].constant
			);
		}
		if (u_strlcat(outbuf, srcbuf, outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
	}

	return GCN_ERR_OK;
}

static GcnError soppformat(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	const char* strop = gcnStrOpcodeSOPP(instr->sopp.opcode);
	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	char srcbuf[128] = {0};
	bool andsep = false;

	switch (instr->sopp.opcode) {
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
	case GCN_S_DECPERFLEVEL:
	case GCN_S_INCPERFLEVEL:
	case GCN_S_NOP:
	case GCN_S_SENDMSG:
	case GCN_S_SENDMSGHALT:
	case GCN_S_SETHALT:
	case GCN_S_SETKILL:
	case GCN_S_SETPRIO:
	case GCN_S_SLEEP:
	case GCN_S_TRAP:
		snprintf(
		    srcbuf, sizeof(srcbuf), " 0x%08x", instr->sopp.constant
		);
		if (u_strlcat(outbuf, srcbuf, outbufsize) > outbufsize) {
			return GCN_ERR_BUFFER_TOO_SMALL;
		}
		break;
	case GCN_S_WAITCNT:
		if (instr->sopp.waitcnt.vm != 0xff) {
			snprintf(
			    srcbuf, sizeof(srcbuf), " vmcnt(%u)",
			    instr->sopp.waitcnt.vm
			);
			if (u_strlcat(outbuf, srcbuf, outbufsize) >
			    outbufsize) {
				return GCN_ERR_BUFFER_TOO_SMALL;
			}
			andsep = true;
		}
		if (instr->sopp.waitcnt.exp != 0xff) {
			snprintf(
			    srcbuf, sizeof(srcbuf),
			    andsep ? " & expcnt(%u)" : " expcnt(%u)",
			    instr->sopp.waitcnt.exp
			);
			if (u_strlcat(outbuf, srcbuf, outbufsize) >
			    outbufsize) {
				return GCN_ERR_BUFFER_TOO_SMALL;
			}
			andsep = true;
		}
		if (instr->sopp.waitcnt.lgkm != 0xff) {
			snprintf(
			    srcbuf, sizeof(srcbuf),
			    andsep ? " & lgkmcnt(%u)" : " lgkmcnt(%u)",
			    instr->sopp.waitcnt.lgkm
			);
			if (u_strlcat(outbuf, srcbuf, outbufsize) >
			    outbufsize) {
				return GCN_ERR_BUFFER_TOO_SMALL;
			}
		}
		break;
	default:
		break;
	}

	return GCN_ERR_OK;
}

static GcnError vintrpformat(
    const GcnInstruction* instr, char* outbuf, size_t outbufsize
) {
	const char* strop = gcnStrOpcodeVINTRP(instr->vintrp.opcode);
	if (u_strlcat(outbuf, strop, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	GcnError gerr =
	    concatoperand(outbuf, outbufsize, &instr->dsts[0], false);
	if (gerr != GCN_ERR_OK) {
		return gerr;
	}
	gerr = concatoperand(outbuf, outbufsize, &instr->srcs[0], true);
	if (gerr != GCN_ERR_OK) {
		return gerr;
	}

	const char chanmap[4] = {'x', 'y', 'z', 'w'};
	if (instr->vintrp.attrchan >= uasize(chanmap)) {
		return GCN_ERR_INTERNAL_ERROR;
	}

	char srcbuf[128] = {0};
	snprintf(
	    srcbuf, sizeof(srcbuf), ", attr%u.%c", instr->vintrp.attr,
	    chanmap[instr->vintrp.attrchan]
	);
	if (u_strlcat(outbuf, srcbuf, outbufsize) > outbufsize) {
		return GCN_ERR_BUFFER_TOO_SMALL;
	}

	return GCN_ERR_OK;
}

GcnError gcnFormatInstruction(
    const GcnInstruction* instr, char* outbuffer, size_t outbuffersize
) {
	if (!instr || !outbuffer || !outbuffersize) {
		return GCN_ERR_INVALID_ARG;
	}

	outbuffer[0] = 0;

	switch (instr->microcode) {
	case GCN_MICROCODE_DS:
		return format_ds(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_EXP:
		return expformat(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_MIMG:
		return mimgformat(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_MTBUF:
		return formatmtbuf(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_MUBUF:
		return formatmubuf(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_SMRD:
		return smrdformat(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_SOP1:
		return genericformat(
		    instr, gcnStrOpcodeSOP1(instr->sop1.opcode), outbuffer,
		    outbuffersize
		);
	case GCN_MICROCODE_SOP2:
		return genericformat(
		    instr, gcnStrOpcodeSOP2(instr->sop2.opcode), outbuffer,
		    outbuffersize
		);
	case GCN_MICROCODE_SOPC:
		return genericformat(
		    instr, gcnStrOpcodeSOPC(instr->sopc.opcode), outbuffer,
		    outbuffersize
		);
	case GCN_MICROCODE_SOPK:
		return genericformat(
		    instr, gcnStrOpcodeSOPK(instr->sopk.opcode), outbuffer,
		    outbuffersize
		);
	case GCN_MICROCODE_SOPP:
		return soppformat(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_VINTRP:
		return vintrpformat(instr, outbuffer, outbuffersize);
	case GCN_MICROCODE_VOP1:
		return genericformat(
		    instr, gcnStrOpcodeVOP1(instr->vop1.opcode), outbuffer,
		    outbuffersize
		);
	case GCN_MICROCODE_VOP2:
		return genericformat(
		    instr, gcnStrOpcodeVOP2(instr->vop2.opcode), outbuffer,
		    outbuffersize
		);
	case GCN_MICROCODE_VOP3:
		return genericformat(
		    instr, gcnStrOpcodeVOP3(instr->vop3.opcode), outbuffer,
		    outbuffersize
		);
	case GCN_MICROCODE_VOPC:
		return genericformat(
		    instr, gcnStrOpcodeVOPC(instr->vopc.opcode), outbuffer,
		    outbuffersize
		);
	default:
		return GCN_ERR_INVALID_MICROCODE;
	}
}
