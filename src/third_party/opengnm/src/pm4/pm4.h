#ifndef _PM4_DECODER_H_
#define _PM4_DECODER_H_

#include <stddef.h>

#include "pm4_error.h"
#include "pm4_types.h"

typedef struct {
	const uint32_t* cmdbegin;
	const uint32_t* cmdcur;
	uint32_t cmdnumdwords;
} Pm4Decoder;

Pm4Error pm4DecoderInit(Pm4Decoder* ctx, const void* cmd, uint32_t cmdsize);
Pm4Error pm4DecoderReset(Pm4Decoder* ctx);
Pm4Error pm4DecodePacket(Pm4Decoder* ctx, Pm4Packet* outpkt);

Pm4Error pm4FormatPacket(const Pm4Packet* pkt, char* outbuf, size_t outbuflen);

#endif	// _PM4_DECODER_H_
