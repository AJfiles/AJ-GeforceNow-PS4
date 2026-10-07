#include <stdbool.h>

#include "pm4.h"
#include "pm4/sid.h"

Pm4Error pm4DecoderInit(Pm4Decoder* ctx, const void* cmd, uint32_t cmdsize) {
	if (!ctx || !cmd || !cmdsize) {
		return PM4_ERR_INVALID_ARG;
	}
	if (cmdsize < sizeof(uint32_t)) {
		return PM4_ERR_PKT_TOO_SMALL;
	}
	if (cmdsize & 3) {
		return PM4_ERR_SIZE_NOT_A_MULTIPLE_FOUR;
	}

	*ctx = (Pm4Decoder){
	    .cmdcur = cmd,
	    .cmdbegin = cmd,
	    .cmdnumdwords = cmdsize / sizeof(uint32_t),
	};
	return PM4_ERR_OK;
}

Pm4Error pm4DecoderReset(Pm4Decoder* ctx) {
	if (!ctx) {
		return PM4_ERR_INVALID_ARG;
	}
	ctx->cmdcur = ctx->cmdbegin;
	return PM4_ERR_OK;
}

static inline bool isvalidtype(uint32_t type) {
	switch (type) {
	case PM4_TYPE_0:
	case PM4_TYPE_2:
	case PM4_TYPE_3:
		return true;
	default:
		return false;
	}
}

static inline void parse_pkt0(const Pm4Decoder* ctx, Pm4Packet* outpkt) {
	outpkt->pkt0.baseindex = PKT0_BASE_INDEX_G(ctx->cmdcur[0]);
}

static inline Pm4Error parse_pkt3(Pm4Decoder* ctx, Pm4Packet3* pkt) {
	const uint32_t header = ctx->cmdcur[0];
	const uint32_t count = PKT_COUNT_G(ctx->cmdcur[0]);

	// Opcode not validated — the decoder accepts any opcode value.
	// Validation is the caller's responsibility (e.g. command buffer validator).
	const uint32_t opcode = PKT3_IT_OPCODE_G(header);

	pkt->opcode = opcode;
	pkt->shtype = PKT3_SHADER_TYPE_G(header);
	pkt->predicate = PKT3_PREDICATE(header);

	Pm4Error err = PM4_ERR_OK;
	switch (opcode) {
	case PKT3_NOP:
		pkt->dataidx = 1;
		break;
	case PKT3_INDEX_BASE:
		if (count < 1) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->index_base.va =
		    ctx->cmdcur[1] | ((uint64_t)ctx->cmdcur[2] << 32);
		break;
	case PKT3_DRAW_INDEX_AUTO:
		if (count < 1) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->drawia.indexcount = ctx->cmdcur[1];
		break;
	case PKT3_WAIT_REG_MEM:
		if (count < 5) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->wrm.op = ctx->cmdcur[1] & 0x7;
		pkt->wrm.addrlo = ctx->cmdcur[2];
		pkt->wrm.addrhi = ctx->cmdcur[3];
		pkt->wrm.ref = ctx->cmdcur[4];
		pkt->wrm.mask = ctx->cmdcur[5];
		pkt->wrm.pollint = ctx->cmdcur[6];
		break;
	case PKT3_EVENT_WRITE_EOP:
		if (count < 4) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->eveop.flags = ctx->cmdcur[1];
		pkt->eveop.addrlo = ctx->cmdcur[2];
		pkt->eveop.addrhi = ctx->cmdcur[3] & 0x0000ffff;
		pkt->eveop.flags2 = ctx->cmdcur[3] & 0xffff0000;
		pkt->eveop.datalo = ctx->cmdcur[4];
		pkt->eveop.datahi = ctx->cmdcur[5];
		break;
	case PKT3_EVENT_WRITE_EOS:
		if (count < 3) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->eveos.type = ctx->cmdcur[1] & 0x3f;
		pkt->eveos.addrlo = ctx->cmdcur[2];
		pkt->eveos.addrhi = ctx->cmdcur[3] & 0x0000ffff;
		pkt->eveos.sel = ctx->cmdcur[3] & 0xffff0000;
		pkt->eveos.val = ctx->cmdcur[4];
		break;
	case PKT3_DMA_DATA:
		if (count < 5) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->dma_data.src_sel = G_500_SRC_SEL(ctx->cmdcur[1]);
		pkt->dma_data.dst_sel = G_500_DST_SEL(ctx->cmdcur[1]);
		pkt->dma_data.src_va =
		    ctx->cmdcur[2] | ((uint64_t)ctx->cmdcur[3] << 32);
		pkt->dma_data.dst_va =
		    ctx->cmdcur[4] | ((uint64_t)ctx->cmdcur[5] << 32);
		pkt->dma_data.length = G_415_BYTE_COUNT_GFX6(ctx->cmdcur[6]);
		pkt->dma_data.wr_confirm =
		    G_415_DISABLE_WR_CONFIRM_GFX6(ctx->cmdcur[6]) == 0;
		break;
	case PKT3_ACQUIRE_MEM:
		if (count < 5) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->acqmem.coherctrl = ctx->cmdcur[1];
		pkt->acqmem.cohersize = ctx->cmdcur[2];
		pkt->acqmem.cohersizehi = ctx->cmdcur[3] & 0x00ffffff;
		pkt->acqmem.coherbase = ctx->cmdcur[4];
		pkt->acqmem.coherbasehi = ctx->cmdcur[5] & 0x00ffffff;
		pkt->acqmem.pollint = ctx->cmdcur[6] & 0x0000ffff;
		break;
	case PKT3_SET_CONTEXT_REG:
		if (count < 1) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->dataidx = 2;
		pkt->setctxreg.reg = ctx->cmdcur[1];
		break;
	case PKT3_SET_SH_REG:
		if (count < 1) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->dataidx = 2;
		pkt->setshreg.reg = ctx->cmdcur[1];
		break;
	case PKT3_SET_UCONFIG_REG:
		if (count < 1) {
			return PM4_ERR_PKT_TOO_SMALL;
		}
		pkt->dataidx = 2;
		pkt->setucfgreg.reg = ctx->cmdcur[1];
		break;
	default:
		break;
	}

	return err;
}

Pm4Error pm4DecodePacket(Pm4Decoder* ctx, Pm4Packet* outpkt) {
	if (!ctx || !outpkt) {
		return PM4_ERR_INVALID_ARG;
	}
	if (!ctx->cmdbegin || !ctx->cmdcur || !ctx->cmdnumdwords) {
		return PM4_ERR_UNINITIALIZED_CONTEXT;
	}

	const uint32_t curoffset = ctx->cmdcur - ctx->cmdbegin;
	const uint32_t remaininglen = ctx->cmdnumdwords - curoffset;
	if (!remaininglen) {
		return PM4_ERR_END_OF_CODE;
	}

	const uint32_t count = PKT_COUNT_G(ctx->cmdcur[0]);
	const uint32_t reqdwords = 2 + count;
	if (reqdwords > remaininglen) {
		return PM4_ERR_OVERFLOW;
	}

	const uint32_t type = PKT_TYPE_G(ctx->cmdcur[0]);
	if (!isvalidtype(type)) {
		return PM4_ERR_INVALID_TYPE;
	}

	*outpkt = (Pm4Packet){
	    .type = type,
	    .count = count,
	    .offset = curoffset * sizeof(uint32_t),
	};

	Pm4Error err = PM4_ERR_OK;
	switch (outpkt->type) {
	case PM4_TYPE_0:
		parse_pkt0(ctx, outpkt);
		break;
	case PM4_TYPE_2:
		// nop packet, nothing to parse
		break;
	case PM4_TYPE_3:
		err = parse_pkt3(ctx, &outpkt->pkt3);
		break;
	default:
		return PM4_ERR_INTERNAL_ERROR;
	}

	ctx->cmdcur += reqdwords;
	return err;
}
