#ifndef _GCN_PRIVATE_MICROCODE_H_
#define _GCN_PRIVATE_MICROCODE_H_

#include "gcn_types.h"

static inline GcnMicrocode parsemicrocode(uint32_t instr) {
	// 9 bit long magic constant
	uint32_t mc = instr & (0x000001ff << 23);  // last 9 bits
	switch (mc) {
	case 0xbe800000:  // 0b101111101
		return GCN_MICROCODE_SOP1;
	case 0xbf000000:  // 0b101111110
		return GCN_MICROCODE_SOPC;
	case 0xbf800000:  // 0b101111111
		return GCN_MICROCODE_SOPP;
	default:
		break;
	}

	// 7 bit long magic constant
	mc = instr & (0x0000007f << 25);  // last 7 bits
	switch (mc) {
	case 0x7e000000:  // 0b00111111
		return GCN_MICROCODE_VOP1;
	case 0x7c000000:  // 0b00111110
		return GCN_MICROCODE_VOPC;
	default:
		break;
	}

	// 6 bit long magic constant
	mc = instr & (0x0000003f << 26);  // last 6 bits
	switch (mc) {
	case 0xd0000000:  // 0b00110100
		return GCN_MICROCODE_VOP3;
	case 0xc8000000:  // 0b00110010
		return GCN_MICROCODE_VINTRP;
	case 0xd8000000:  // 0b00110110
		return GCN_MICROCODE_DS;
	case 0xe0000000:  // 0b00111000
		return GCN_MICROCODE_MUBUF;
	case 0xe8000000:  // 0b00111010
		return GCN_MICROCODE_MTBUF;
	case 0xf0000000:  // 0b00111100
		return GCN_MICROCODE_MIMG;
	case 0xf8000000:  // 0b00111110
		return GCN_MICROCODE_EXP;
	default:
		break;
	}

	// 5 bit long magic constant
	mc = instr & (0x0000001f << 27);  // last 5 bits
	switch (mc) {
	case 0xc0000000:  // 0b00011000
		return GCN_MICROCODE_SMRD;
	default:
		break;
	}

	// 4 bit long magic constant
	mc = instr & (0x0000000f << 28);  // last 4 bits
	switch (mc) {
	case 0xb0000000:  // 0b00001011
		return GCN_MICROCODE_SOPK;
	default:
		break;
	}

	// 2 bit long magic constant
	mc = instr & (0x00000003 << 30);  // last 2 bits
	switch (mc) {
	case 0x80000000:  // 0b00000010
		return GCN_MICROCODE_SOP2;
	default:
		break;
	}

	// 1 bit long magic constant
	mc = instr & (0x00000001 << 31);  // last 1 bit
	switch (mc) {
	case 0x00000000:  // 0b00000000
		return GCN_MICROCODE_VOP2;
	default:
		break;
	}

	return GCN_MICROCODE_INVALID;
}

static inline bool getmicrocodelen(uint32_t* outlen, GcnMicrocode fmt) {
	if (!outlen) {
		return false;
	}
	switch (fmt) {
	case GCN_MICROCODE_SOP2:
	case GCN_MICROCODE_SOPK:
	case GCN_MICROCODE_SOP1:
	case GCN_MICROCODE_SOPC:
	case GCN_MICROCODE_SOPP:
	case GCN_MICROCODE_SMRD:
	case GCN_MICROCODE_VOP2:
	case GCN_MICROCODE_VOP1:
	case GCN_MICROCODE_VOPC:
	case GCN_MICROCODE_VINTRP:
		*outlen = sizeof(uint32_t);
		return true;
	case GCN_MICROCODE_VOP3:
	case GCN_MICROCODE_DS:
	case GCN_MICROCODE_MUBUF:
	case GCN_MICROCODE_MTBUF:
	case GCN_MICROCODE_MIMG:
	case GCN_MICROCODE_EXP:
		*outlen = sizeof(uint64_t);
		return true;
	default:
		return false;
	}
}

static inline bool parseoperandfield(
    GcnOperandField* outfield, uint16_t field
) {
	// out of bounds
	if (field > GCN_OPFIELD_VGPR_255) {
		return false;
	}
	// reserved fields
	if (field == 104 || field == 105 || field == 125) {
		return false;
	}
	if (field >= 209 && field <= 239) {
		return false;
	}
	if (field >= 248 && field <= 250) {
		return false;
	}

	*outfield = field;
	return true;
}

static inline bool parseexptarget(GcnExpTarget* outtarget, uint8_t target) {
	// out of bounds
	if (target > GCN_EXP_TARGET_PARAM_31) {
		return false;
	}
	// reserved fields
	if (target == 10 || target == 11) {
		return false;
	}
	if (target >= 16 && target <= 31) {
		return false;
	}

	*outtarget = target;
	return true;
}

//
// ds
//
static inline bool parseopcode_ds(GcnOpcodeDS* outopcode, uint8_t op) {
	if (op > GCN_DS_NOP && op < GCN_DS_GWS_SEMA_RELEASE_ALL) {
		return false;
	}
	if (op > GCN_DS_MAX_F64 && op < GCN_DS_ADD_RTN_U64) {
		return false;
	}
	if (op > GCN_DS_MAX_RTN_F64 && op < GCN_DS_READ_B64) {
		return false;
	}
	if (op > GCN_DS_READ2ST64_B64 && op < GCN_DS_CONDXCHG32_RTN_B64) {
		return false;
	}
	if (op > GCN_DS_CONDXCHG32_RTN_B64 && op < GCN_DS_ADD_SRC2_U32) {
		return false;
	}
	if (op > GCN_DS_XOR_SRC2_B32 && op < GCN_DS_WRITE_SRC2_B32) {
		return false;
	}
	if (op > GCN_DS_WRITE_SRC2_B32 && op < GCN_DS_MIN_SRC2_F32) {
		return false;
	}
	if (op > GCN_DS_MAX_SRC2_F32 && op < GCN_DS_ADD_SRC2_U64) {
		return false;
	}
	if (op > GCN_DS_XOR_SRC2_B64 && op < GCN_DS_WRITE_SRC2_B64) {
		return false;
	}
	if (op > GCN_DS_WRITE_SRC2_B64 && op < GCN_DS_MIN_SRC2_F64) {
		return false;
	}
	if (op > GCN_DS_MAX_SRC2_F64 && op < GCN_DS_WRITE_B96) {
		return false;
	}
	if (op > GCN_DS_WRITE_B128 && op < GCN_DS_CONDXCHG32_RTN_B128) {
		return false;
	}
	*outopcode = op;
	return true;
}

static inline uint8_t ds_offset0(uint64_t instr) {
	return (instr >> 0) & 0xff;
}
static inline uint8_t ds_offset1(uint64_t instr) {
	return (instr >> 8) & 0xff;
}
static inline uint8_t ds_gds(uint64_t instr) {
	return (instr >> 17) & 0x1;
}
static inline uint8_t ds_op(uint64_t instr) {
	return (instr >> 18) & 0xff;
}
static inline uint8_t ds_addr(uint64_t instr) {
	return (instr >> 32) & 0xff;
}
static inline uint8_t ds_data0(uint64_t instr) {
	return (instr >> 40) & 0xff;
}
static inline uint8_t ds_data1(uint64_t instr) {
	return (instr >> 48) & 0xff;
}
static inline uint8_t ds_vdst(uint64_t instr) {
	return (instr >> 56) & 0xff;
}

//
// exp
//
static inline uint8_t exp_en(uint64_t instr) {
	return instr & 0xf;
}
static inline uint8_t exp_target(uint64_t instr) {
	return (instr >> 4) & 0x3f;
}
static inline uint8_t exp_compr(uint64_t instr) {
	return (instr >> 10) & 0x1;
}
static inline uint8_t exp_done(uint64_t instr) {
	return (instr >> 11) & 0x1;
}
static inline uint8_t exp_vm(uint64_t instr) {
	return (instr >> 12) & 0x1;
}
static inline uint8_t exp_vsrc0(uint64_t instr) {
	return (instr >> 32) & 0xff;
}
static inline uint8_t exp_vsrc1(uint64_t instr) {
	return (instr >> 40) & 0xff;
}
static inline uint8_t exp_vsrc2(uint64_t instr) {
	return (instr >> 48) & 0xff;
}
static inline uint8_t exp_vsrc3(uint64_t instr) {
	return (instr >> 56) & 0xff;
}

//
// mimg
//
static inline bool parseopcode_mimg(GcnOpcodeMIMG* outopcode, uint8_t op) {
	if (op > GCN_IMAGE_LOAD_MIP_PCK_SGN && op < GCN_IMAGE_STORE) {
		return false;
	}
	if (op > GCN_IMAGE_STORE_MIP_PCK && op < GCN_IMAGE_GET_RESINFO) {
		return false;
	}
	if (op > GCN_IMAGE_ATOMIC_SUB && op < GCN_IMAGE_ATOMIC_SMIN) {
		return false;
	}
	if (op > GCN_IMAGE_GATHER4_CL && op < GCN_IMAGE_GATHER4_L) {
		return false;
	}
	if (op > GCN_IMAGE_GATHER4_C_CL && op < GCN_IMAGE_GATHER4_C_L) {
		return false;
	}
	if (op > GCN_IMAGE_GATHER4_CL_O && op < GCN_IMAGE_GATHER4_L_O) {
		return false;
	}
	if (op > GCN_IMAGE_GATHER4_C_CL_O && op < GCN_IMAGE_GATHER4_C_L_O) {
		return false;
	}
	if (op > GCN_IMAGE_GET_LOD && op < GCN_IMAGE_SAMPLE_CD) {
		return false;
	}
	if (op > GCN_IMAGE_SAMPLE_C_CD_CL_O) {
		return false;
	}
	*outopcode = op;
	return true;
}

static inline uint8_t mimg_dmask(uint64_t instr) {
	return (instr >> 8) & 0xf;
}
static inline uint8_t mimg_unrm(uint64_t instr) {
	return (instr >> 12) & 0x1;
}
static inline uint8_t mimg_glc(uint64_t instr) {
	return (instr >> 13) & 0x1;
}
static inline uint8_t mimg_da(uint64_t instr) {
	return (instr >> 14) & 0x1;
}
static inline uint8_t mimg_r128(uint64_t instr) {
	return (instr >> 15) & 0x1;
}
static inline uint8_t mimg_tfe(uint64_t instr) {
	return (instr >> 16) & 0x1;
}
static inline uint8_t mimg_lwe(uint64_t instr) {
	return (instr >> 17) & 0x1;
}
static inline uint8_t mimg_op(uint64_t instr) {
	return (instr >> 18) & 0x7f;
}
static inline uint8_t mimg_slc(uint64_t instr) {
	return (instr >> 25) & 0x1;
}
static inline uint8_t mimg_vaddr(uint64_t instr) {
	return (instr >> 32) & 0xff;
}
static inline uint8_t mimg_vdata(uint64_t instr) {
	return (instr >> 40) & 0xff;
}
static inline uint8_t mimg_srsrc(uint64_t instr) {
	return ((instr >> 48) & 0x1f) << 2;
}
static inline uint8_t mimg_ssamp(uint64_t instr) {
	return ((instr >> 53) & 0x1f) << 2;
}

//
// mtbuf
//
static inline bool isvalidmtbuf(uint8_t op) {
	if (op > GCN_TBUFFER_STORE_FORMAT_XYZW) {
		return false;
	}
	return true;
}

static inline uint16_t mtbuf_offset(uint64_t instr) {
	return instr & 0xfff;
}
static inline uint8_t mtbuf_offen(uint64_t instr) {
	return (instr >> 12) & 0x1;
}
static inline uint8_t mtbuf_idxen(uint64_t instr) {
	return (instr >> 13) & 0x1;
}
static inline uint8_t mtbuf_glc(uint64_t instr) {
	return (instr >> 14) & 0x1;
}
static inline uint8_t mtbuf_op(uint64_t instr) {
	return (instr >> 16) & 0x7;
}
static inline uint8_t mtbuf_dfmt(uint64_t instr) {
	return (instr >> 19) & 0xf;
}
static inline uint8_t mtbuf_nfmt(uint64_t instr) {
	return (instr >> 23) & 0x7;
}
static inline uint8_t mtbuf_vaddr(uint64_t instr) {
	return (instr >> 32) & 0xff;
}
static inline uint8_t mtbuf_vdata(uint64_t instr) {
	return (instr >> 40) & 0xff;
}
static inline uint8_t mtbuf_srsrc(uint64_t instr) {
	return ((instr >> 48) & 0x1f) << 2;
}
static inline uint8_t mtbuf_slc(uint64_t instr) {
	return (instr >> 54) & 0x1;
}
static inline uint8_t mtbuf_tfe(uint64_t instr) {
	return (instr >> 55) & 0x1;
}
static inline uint8_t mtbuf_soffset(uint64_t instr) {
	return (instr >> 56) & 0xff;
}

//
// mubuf
//
static inline bool isvalidmubuf(uint8_t op) {
	if (op > GCN_BUFFER_LOAD_DWORDX3 && op < GCN_BUFFER_STORE_BYTE) {
		return false;
	}
	if (op > GCN_BUFFER_STORE_BYTE && op < GCN_BUFFER_STORE_SHORT) {
		return false;
	}
	if (op > GCN_BUFFER_STORE_DWORDX3 && op < GCN_BUFFER_ATOMIC_SWAP) {
		return false;
	}
	if (op > GCN_BUFFER_ATOMIC_FMAX && op < GCN_BUFFER_ATOMIC_SWAP_X2) {
		return false;
	}
	if (op > GCN_BUFFER_ATOMIC_FMAX_X2 && op < GCN_BUFFER_WBINVL1_SC) {
		return false;
	}
	if (op > GCN_BUFFER_WBINVL1) {
		return false;
	}
	return true;
}

static inline uint16_t mubuf_offset(uint64_t instr) {
	return instr & 0xfff;
}
static inline uint8_t mubuf_offen(uint64_t instr) {
	return (instr >> 12) & 0x1;
}
static inline uint8_t mubuf_idxen(uint64_t instr) {
	return (instr >> 13) & 0x1;
}
static inline uint64_t mubuf_idxenw(bool en) {
	return (en & 0x1) << 13;
}
static inline uint8_t mubuf_glc(uint64_t instr) {
	return (instr >> 14) & 0x1;
}
static inline uint8_t mubuf_lds(uint64_t instr) {
	return (instr >> 16) & 0x1;
}
static inline uint8_t mubuf_op(uint64_t instr) {
	return (instr >> 18) & 0x7f;
}
static inline uint64_t mubuf_opw(GcnOpcodeMUBUF op) {
	return (op & 0x1f) << 18;
}
static inline uint8_t mubuf_vaddr(uint64_t instr) {
	return (instr >> 32) & 0xff;
}
static inline uint64_t mubuf_vaddrw(GcnOperandField f) {
	return (uint64_t)((f - GCN_OPFIELD_VGPR_0) & 0xff) << 32;
}
static inline uint8_t mubuf_vdata(uint64_t instr) {
	return (instr >> 40) & 0xff;
}
static inline uint64_t mubuf_vdataw(GcnOperandField f) {
	return (uint64_t)((f - GCN_OPFIELD_VGPR_0) & 0xff) << 40;
}
static inline uint8_t mubuf_srsrc(uint64_t instr) {
	return ((instr >> 48) & 0x1f) << 2;
}
static inline uint64_t mubuf_srsrcw(GcnOperandField f) {
	return (uint64_t)((f >> 2) & 0x1f) << 48;
}
static inline uint8_t mubuf_slc(uint64_t instr) {
	return (instr >> 54) & 0x1;
}
static inline uint8_t mubuf_tfe(uint64_t instr) {
	return (instr >> 55) & 0x1;
}
static inline uint8_t mubuf_soffset(uint64_t instr) {
	return (instr >> 56) & 0xff;
}
static inline uint64_t mubuf_soffsetw(GcnOperandField f) {
	return (uint64_t)(f & 0xff) << 56;
}

//
// smrd
//
static inline bool isvalidsmrd(uint8_t op) {
	if (op > GCN_S_DCACHE_INV) {
		return false;
	}
	if (op > GCN_S_LOAD_DWORDX16 && op < GCN_S_BUFFER_LOAD_DWORD) {
		return false;
	}
	if (op > GCN_S_BUFFER_LOAD_DWORDX16 && op < GCN_S_DCACHE_INV_VOL) {
		return false;
	}
	return true;
}

static inline uint8_t smrd_op(uint32_t instr) {
	return (instr >> 22) & 0x1f;
}
static inline uint32_t smrd_opw(GcnOpcodeSMRD op) {
	return (op & 0x1f) << 22;
}
static inline uint8_t smrd_imm(uint32_t instr) {
	return (instr >> 8) & 0x1;
}
static inline uint32_t smrd_immw(bool imm) {
	return (imm & 0x1) << 8;
}
static inline uint8_t smrd_offset(uint32_t instr) {
	return instr & 0xff;
}
static inline uint32_t smrd_offsetw(uint8_t off) {
	return off & 0xff;
}
static inline uint8_t smrd_sbase(uint32_t instr) {
	return (instr >> 9) & 0x3f;
}
static inline uint32_t smrd_sbasew(GcnOperandField f) {
	return ((f / 2) & 0x3f) << 9;
}
static inline uint8_t smrd_sdst(uint32_t instr) {
	return (instr >> 15) & 0x7f;
}
static inline uint32_t smrd_sdstw(GcnOperandField f) {
	return (f & 0x7f) << 15;
}

//
// sop1
//
static inline bool parseopcode_sop1(GcnOpcodeSOP1* outopcode, uint8_t op) {
	if (op < GCN_S_MOV_B32 || op > GCN_S_MOV_FED_B32) {
		return false;
	}

	*outopcode = op;
	return true;
}

static inline uint8_t sop1_op(uint32_t instr) {
	return (instr >> 8) & 0xff;
}
static inline uint32_t sop1_opw(GcnOpcodeSOP1 op) {
	return (op & 0xff) << 8;
}
static inline uint8_t sop1_ssrc0(uint32_t instr) {
	return instr & 0xff;
}
static inline uint32_t sop1_ssrc0w(GcnOperandField f) {
	return f & 0xff;
}
static inline uint8_t sop1_sdst(uint32_t instr) {
	return (instr >> 16) & 0x7f;
}
static inline uint32_t sop1_sdstw(GcnOperandField f) {
	return (f & 0x7f) << 16;
}

//
// sop2
//
static inline bool parseopcode_sop2(GcnOpcodeSOP2* outopcode, uint8_t op) {
	if (op > GCN_S_CSELECT_B64 && op < GCN_S_AND_B32) {
		return false;
	}
	if (op > GCN_S_ABSDIFF_I32) {
		return false;
	}

	*outopcode = op;
	return true;
}

static inline uint8_t sop2_op(uint32_t instr) {
	return (instr >> 23) & 0x7f;
}
static inline uint8_t sop2_ssrc0(uint32_t instr) {
	return instr & 0xff;
}
static inline uint8_t sop2_ssrc1(uint32_t instr) {
	return (instr >> 8) & 0xff;
}
static inline uint8_t sop2_sdst(uint32_t instr) {
	return (instr >> 16) & 0x7f;
}

//
// sopc
//
static inline bool isvalidsopc(uint8_t op) {
	return op <= GCN_S_SETVSKIP;
}

static inline uint8_t sopc_op(uint32_t instr) {
	return (instr >> 16) & 0x7f;
}
static inline uint8_t sopc_ssrc0(uint32_t instr) {
	return instr & 0xff;
}
static inline uint8_t sopc_ssrc1(uint32_t instr) {
	return (instr >> 8) & 0xff;
}

//
// sopk
//
static inline bool isvalidsopk(uint8_t op) {
	return op <= GCN_S_SETREG_IMM32_B32;
}

static inline uint8_t sopk_op(uint32_t instr) {
	return (instr >> 23) & 0x1f;
}
static inline uint8_t sopk_sdst(uint32_t instr) {
	return (instr >> 16) & 0x7f;
}
static inline uint16_t sopk_simm(uint32_t instr) {
	return instr & 0xffff;
}

//
// sopp
//
static inline bool isvalidsopp(uint8_t op) {
	if (op > GCN_S_CBRANCH_CDBGSYS_AND_USER) {
		return false;
	}
	return true;
}

static inline uint8_t sopp_op(uint32_t instr) {
	return (instr >> 16) & 0x7f;
}
static inline uint32_t sopp_opw(GcnOpcodeSOPP op) {
	return (op & 0x7f) << 16;
}
static inline uint16_t sopp_simm16(uint32_t instr) {
	return instr & 0xffff;
}
static inline uint32_t sopp_simm16w(uint16_t imm) {
	return imm;
}

//
// vintrp
//
static inline bool parseopcode_vintrp(GcnOpcodeVINTRP* outopcode, uint8_t op) {
	if (op > GCN_V_INTERP_MOV_F32) {
		return false;
	}

	*outopcode = op;
	return true;
}

static inline uint8_t vintrp_op(uint32_t instr) {
	return (instr >> 16) & 0x3;
}
static inline uint16_t vintrp_vsrc(uint32_t instr) {
	return instr & 0xff;
}
static inline uint8_t vintrp_vdst(uint32_t instr) {
	return (instr >> 18) & 0xff;
}
static inline uint8_t vintrp_attr(uint32_t instr) {
	return (instr >> 10) & 0x3f;
}
static inline uint8_t vintrp_attrchan(uint32_t instr) {
	return (instr >> 8) & 0x3;
}

//
// vop1
//
static inline bool parseopcode_vop1(GcnOpcodeVOP1* outopcode, uint8_t op) {
	if (op > GCN_V_EXP_LEGACY_F32) {
		return false;
	}

	*outopcode = op;
	return true;
}

static inline uint8_t vop1_op(uint32_t instr) {
	return (instr >> 9) & 0xff;
}
static inline uint16_t vop1_src0(uint32_t instr) {
	return instr & 0x1ff;
}
static inline uint8_t vop1_vdst(uint32_t instr) {
	return (instr >> 17) & 0xff;
}

//
// vop2
//
static inline bool isvalidvop2(uint8_t op) {
	if (op > GCN_V_CVT_PK_I16_I32) {
		return false;
	}
	return true;
}

static inline uint8_t vop2_op(uint32_t instr) {
	return (instr >> 25) & 0x3f;
}
static inline uint32_t vop2_opw(GcnOpcodeVOP2 op) {
	return (op & 0x3f) << 25;
}
static inline uint16_t vop2_src0(uint32_t instr) {
	return instr & 0x1ff;
}
static inline uint32_t vop2_src0w(GcnOperandField src) {
	return src & 0x1ff;
}
static inline uint8_t vop2_vsrc1(uint32_t instr) {
	return (instr >> 9) & 0xff;
}
static inline uint32_t vop2_vsrc1w(GcnOperandField src) {
	return ((src - GCN_OPFIELD_VGPR_0) & 0xff) << 9;
}
static inline uint8_t vop2_vdst(uint32_t instr) {
	return (instr >> 17) & 0xff;
}
static inline uint32_t vop2_vdstw(GcnOperandField dst) {
	return ((dst - GCN_OPFIELD_VGPR_0) & 0xff) << 17;
}

//
// vop3
//
static inline bool parseopcode_vop3(GcnOpcodeVOP3* outopcode, uint16_t op) {
	if (op > GCN_V_EXP_LEGACY_F32_E64) {
		return false;
	}

	*outopcode = op;
	return true;
}

static inline bool isvop3b(GcnOpcodeVOP3 op) {
	if (op >= GCN_V_ADD_I32_E64 && op <= GCN_V_SUBBREV_U32_E64) {
		return true;
	}
	if (op == GCN_V_DIV_SCALE_F32_E64 || op == GCN_V_DIV_SCALE_F64_E64 ||
	    op == GCN_V_MAD_U64_U32_E64 || op == GCN_V_MAD_I64_I32_E64) {
		return true;
	}
	return false;
}

static inline uint16_t vop3_op(uint64_t instr) {
	return (instr >> 17) & 0x1ff;
}
static inline uint8_t vop3_vdst(uint64_t instr) {
	return instr & 0xff;
}
static inline uint16_t vop3_src0(uint64_t instr) {
	return (instr >> 32) & 0x1ff;
}
static inline uint16_t vop3_src1(uint64_t instr) {
	return (instr >> 41) & 0x1ff;
}
static inline uint16_t vop3_src2(uint64_t instr) {
	return (instr >> 50) & 0x1ff;
}
static inline uint8_t vop3_omod(uint64_t instr) {
	return (instr >> 59) & 0x3;
}
static inline uint8_t vop3_neg(uint64_t instr) {
	return (instr >> 61) & 0x7;
}

static inline uint8_t vop3a_abs(uint64_t instr) {
	return (instr >> 8) & 0x7;
}
static inline uint8_t vop3a_clmp(uint64_t instr) {
	return (instr >> 11) & 0x1;
}
static inline uint8_t vop3b_sdst(uint64_t instr) {
	return (instr >> 8) & 0x7f;
}

//
// vopc
//
static inline bool parseopcode_vopc(GcnOpcodeVOPC* outopcode, uint8_t op) {
	if (op > GCN_V_CMPX_TRU_U64) {
		return false;
	}

	*outopcode = op;
	return true;
}

static inline uint8_t vopc_op(uint32_t instr) {
	return (instr >> 17) & 0xff;
}
static inline uint16_t vopc_src0(uint32_t instr) {
	return instr & 0x1ff;
}
static inline uint8_t vopc_vsrc1(uint32_t instr) {
	return (instr >> 9) & 0xff;
}

#endif	// _GCN_PRIVATE_MICROCODE_H_
