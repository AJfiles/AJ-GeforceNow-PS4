#ifndef _GCN_DECODER_H_
#define _GCN_DECODER_H_

#include "gcn_types.h"

typedef struct {
	const uint8_t* codestart;
	const uint8_t* curinstr;
	uint32_t codesize;
} GcnDecoderContext;

GcnError gcnDecoderInit(
    GcnDecoderContext* ctx, const void* code, uint32_t codesize
);
GcnError gcnDecoderReset(GcnDecoderContext* ctx);
GcnError gcnDecodeInstruction(GcnDecoderContext* ctx, GcnInstruction* outinstr);

typedef GcnError (*GnmAssemblyWriter)(
    const void* data, uint32_t datasize, void* userdata
);
typedef struct {
	GnmAssemblyWriter writer;
	void* writeuserdata;
} GcnAssembler;

typedef struct {
	bool idxen;
} GcnAsMubufOptions;

GcnError gcnAsMubuf(
    GcnAssembler* as, GcnOpcodeMUBUF op, GcnOperandField vdata,
    GcnOperandField vaddr, GcnOperandField srsrc, GcnOperandField soffset,
    const GcnAsMubufOptions* opts
);
GcnError gcnAsSmrd(
    GcnAssembler* as, GcnOpcodeSMRD op, GcnOperandField sdst,
    GcnOperandField ssrc, uint8_t offset
);
GcnError gcnAsSop1(
    GcnAssembler* as, GcnOpcodeSOP1 op, GcnOperandField sdst,
    GcnOperandField ssrc0
);
GcnError gcnAsSopp(GcnAssembler* as, GcnOpcodeSOPP op, uint16_t imm);
GcnError gcnAsVop2(
    GcnAssembler* as, GcnOpcodeVOP2 op, GcnOperandField vdst,
    GcnOperandField src0, GcnOperandField vsrc1
);

GcnError gcnFormatInstruction(
    const GcnInstruction* instr, char* outbuffer, size_t outbuffersize
);

GcnError gcnAnalyzeShader(
    const void* code, uint32_t codesize, GcnAnalysis* out
);

#endif	// _GCN_DECODER_H_
