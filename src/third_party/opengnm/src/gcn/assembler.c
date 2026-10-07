#include "gcn.h"

#include "microcode.h"

static inline GcnError writeu32(GcnAssembler* as, uint32_t data) {
	return as->writer(&data, sizeof(data), as->writeuserdata);
}
static inline GcnError writeu64(GcnAssembler* as, uint64_t data) {
	return as->writer(&data, sizeof(data), as->writeuserdata);
}

GcnError gcnAsMubuf(
    GcnAssembler* as, GcnOpcodeMUBUF op, GcnOperandField vdata,
    GcnOperandField vaddr, GcnOperandField srsrc, GcnOperandField soffset,
    const GcnAsMubufOptions* opts
) {
	if (!as || !as->writer || !opts) {
		return GCN_ERR_INVALID_ARG;
	}
	if (vdata < GCN_OPFIELD_VGPR_0 || vdata > GCN_OPFIELD_VGPR_255) {
		return GCN_ERR_INVALID_DST_FIELD;
	}
	if (vaddr < GCN_OPFIELD_VGPR_0 || vaddr > GCN_OPFIELD_VGPR_255) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	if (srsrc > GCN_OPFIELD_SGPR_103) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	return writeu64(
	    as, 0xe0000000 | mubuf_opw(op) | mubuf_vdataw(vdata) |
		    mubuf_vaddrw(vaddr) | mubuf_srsrcw(srsrc) |
		    mubuf_soffsetw(soffset) | mubuf_idxenw(opts->idxen)
	);
}

GcnError gcnAsSmrd(
    GcnAssembler* as, GcnOpcodeSMRD op, GcnOperandField sdst,
    GcnOperandField ssrc, uint8_t offset
) {
	if (!as || !as->writer) {
		return GCN_ERR_INVALID_ARG;
	}
	if (sdst > GCN_OPFIELD_SGPR_103) {
		return GCN_ERR_INVALID_DST_FIELD;
	}
	if (ssrc > GCN_OPFIELD_SGPR_103) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	return writeu32(
	    as, 0xc0000000 | smrd_opw(op) | smrd_sdstw(sdst) |
		    smrd_sbasew(ssrc) | smrd_immw(true) | smrd_offsetw(offset)
	);
}

GcnError gcnAsSop1(
    GcnAssembler* as, GcnOpcodeSOP1 op, GcnOperandField sdst,
    GcnOperandField ssrc0
) {
	if (!as || !as->writer) {
		return GCN_ERR_INVALID_ARG;
	}
	if (sdst > GCN_OPFIELD_SGPR_103) {
		return GCN_ERR_INVALID_DST_FIELD;
	}
	if (ssrc0 > GCN_OPFIELD_SGPR_103) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	return writeu32(
	    as,
	    0xbe800000 | sop1_opw(op) | sop1_sdstw(sdst) | sop1_ssrc0w(ssrc0)
	);
}

GcnError gcnAsSopp(GcnAssembler* as, GcnOpcodeSOPP op, uint16_t imm) {
	if (!as || !as->writer) {
		return GCN_ERR_INVALID_ARG;
	}
	return writeu32(as, 0xbf800000 | sopp_opw(op) | sopp_simm16w(imm));
}

GcnError gcnAsVop2(
    GcnAssembler* as, GcnOpcodeVOP2 op, GcnOperandField vdst,
    GcnOperandField src0, GcnOperandField vsrc1
) {
	if (!as || !as->writer) {
		return GCN_ERR_INVALID_ARG;
	}
	if (vdst < GCN_OPFIELD_VGPR_0 || vdst > GCN_OPFIELD_VGPR_255) {
		return GCN_ERR_INVALID_DST_FIELD;
	}
	if (vsrc1 < GCN_OPFIELD_VGPR_0 || vsrc1 > GCN_OPFIELD_VGPR_255) {
		return GCN_ERR_INVALID_SRC_FIELD;
	}
	return writeu32(
	    as, vop2_opw(op) | vop2_vdstw(vdst) | vop2_src0w(src0) |
		    vop2_vsrc1w(vsrc1)
	);
}
