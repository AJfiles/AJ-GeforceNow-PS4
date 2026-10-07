#include "pm4.h"

#include "pm4/sid.h"

#include <stdarg.h>
#include <stdio.h>

#include "u/utility.h"

static Pm4Error doformat(
    char* outbuf, size_t outbufsize, const char* fmt, ...
) {
	va_list va = {0};

	va_start(va, fmt);
	const int actualsize = vsnprintf(NULL, 0, fmt, va);
	va_end(va);

	if (actualsize < 0 || (size_t)actualsize + 1 > outbufsize) {
		return PM4_ERR_BUFFER_TOO_SMALL;
	}

	va_start(va, fmt);
	vsnprintf(outbuf, outbufsize, fmt, va);
	va_end(va);

	va_end(va);
	return PM4_ERR_OK;
}

static Pm4Error format_pkt3(
    const Pm4Packet3* pkt3, char* outbuf, size_t outbuflen
) {
	const char* opname = pm4StrOpcode(pkt3->opcode);

	Pm4Error err = PM4_ERR_OK;
	switch (pkt3->opcode) {
	case PKT3_INDEX_BASE:
		err = doformat(
		    outbuf, outbuflen, "pkt3_%s(indexcount=0x%lx)", opname,
		    pkt3->index_base.va
		);
		break;
	case PKT3_DRAW_INDEX_AUTO:
		err = doformat(
		    outbuf, outbuflen, "pkt3_%s(indexcount=%u)", opname,
		    pkt3->drawia.indexcount
		);
		break;
	case PKT3_WAIT_REG_MEM:
		err = doformat(
		    outbuf, outbuflen,
		    "pkt3_%s(op=0x%x, addrlo=0x%x, addrhi=0x%x, ref=0x%x, "
		    "mask=0x%x, pollint=0x%x)",
		    opname, pkt3->wrm.op, pkt3->wrm.addrlo, pkt3->wrm.addrhi,
		    pkt3->wrm.ref, pkt3->wrm.mask, pkt3->wrm.pollint
		);
		break;
	case PKT3_EVENT_WRITE_EOP:
		err = doformat(
		    outbuf, outbuflen,
		    "pkt3_%s(flags=0x%x, addrlo=0x%x, addrhi=0x%x, "
		    "flags2=0x%x, datalo=0x%x, datahi=0x%x)",
		    opname, pkt3->eveop.flags, pkt3->eveop.addrlo,
		    pkt3->eveop.addrhi, pkt3->eveop.flags2, pkt3->eveop.datalo,
		    pkt3->eveop.datahi
		);
		break;
	case PKT3_EVENT_WRITE_EOS:
		err = doformat(
		    outbuf, outbuflen,
		    "pkt3_%s(type=0x%x, addrlo=0x%x, addrhi=0x%x, sel=0x%x, "
		    "val=0x%x)",
		    opname, pkt3->eveos.type, pkt3->eveos.addrlo,
		    pkt3->eveos.addrhi, pkt3->eveos.sel, pkt3->eveos.val
		);
		break;
	case PKT3_DMA_DATA:
		err = doformat(
		    outbuf, outbuflen,
		    "pkt3_%s(src_sel=0x%x, dst_sel=0x%x, src_va=0x%x, "
		    "dst_va=0x%x, length=0x%x wr_confirm=%u)",
		    opname, pkt3->dma_data.src_sel, pkt3->dma_data.dst_sel,
		    pkt3->dma_data.src_va, pkt3->dma_data.dst_va,
		    pkt3->dma_data.length, pkt3->dma_data.wr_confirm
		);
		break;
	case PKT3_ACQUIRE_MEM:
		err = doformat(
		    outbuf, outbuflen,
		    "pkt3_%s(ctrl=0x%x, cohersize=0x%x, cohersizehi=0x%x, "
		    "coherbase=0x%x, coherbasehi=0x%x, pollint=%u)",
		    opname, pkt3->acqmem.coherctrl, pkt3->acqmem.cohersize,
		    pkt3->acqmem.cohersizehi, pkt3->acqmem.coherbase,
		    pkt3->acqmem.coherbasehi, pkt3->acqmem.pollint
		);
		break;
	case PKT3_SET_CONTEXT_REG:
		err = doformat(
		    outbuf, outbuflen, "pkt3_%s(reg=0x%x)", opname,
		    pkt3->setctxreg.reg
		);
		break;
	case PKT3_SET_SH_REG:
		err = doformat(
		    outbuf, outbuflen, "pkt3_%s(reg=0x%x)", opname,
		    pkt3->setshreg.reg
		);
		break;
	case PKT3_SET_UCONFIG_REG:
		err = doformat(
		    outbuf, outbuflen, "pkt3_%s(reg=0x%x)", opname,
		    pkt3->setucfgreg.reg
		);
		break;
	default:
		err = doformat(outbuf, outbuflen, "pkt3_%s", opname);
		break;
	}

	if (pkt3->predicate) {
		if (u_strlcat(outbuf, " [pred]", outbuflen) > outbuflen) {
			return PM4_ERR_BUFFER_TOO_SMALL;
		}
	}

	return err;
}

Pm4Error pm4FormatPacket(const Pm4Packet* pkt, char* outbuf, size_t outbuflen) {
	if (!pkt || !outbuf || !outbuflen) {
		return PM4_ERR_INVALID_ARG;
	}

	outbuf[0] = 0;

	Pm4Error err = PM4_ERR_OK;
	switch (pkt->type) {
	case PM4_TYPE_0:
		err = doformat(
		    outbuf, outbuflen, "pkt0_draw, baseindex %u",
		    pkt->pkt0.baseindex
		);
		break;
	case PM4_TYPE_2:
		err = doformat(outbuf, outbuflen, "pkt2_nop");
		break;
	case PM4_TYPE_3:
		err = format_pkt3(&pkt->pkt3, outbuf, outbuflen);
		break;
	default:
		return PM4_ERR_INVALID_TYPE;
	}

	return err;
}
