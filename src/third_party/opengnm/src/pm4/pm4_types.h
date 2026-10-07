#ifndef _PM4_TYPES_H_
#define _PM4_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	PM4_TYPE_0 = 0,	 // register set packet (base index + count + data)
	PM4_TYPE_2 = 2,	 // nop packet
	PM4_TYPE_3 = 3,	 // extended packet
} Pm4PacketType;

typedef enum {
	PM4_SH_GRAPHICS = 0,
	PM4_SH_COMPUTE = 1,
} Pm4ShaderType;

typedef struct {
	uint32_t opcode;  // PM4 type-3 opcode (see PKT3_IT_OPCODE defines)
	uint32_t
	    dataidx;  // optional index to packet (u32) array where data starts
	Pm4ShaderType shtype;
	bool predicate;

	union {
		struct {
			uint64_t va;
		} index_base;
		struct {
			uint32_t indexcount;
		} drawia;
		struct {
			uint32_t op;
			uint32_t addrlo;
			uint32_t addrhi;
			uint32_t ref;
			uint32_t mask;
			uint32_t pollint;
		} wrm;
		struct {
			uint32_t flags;
			uint32_t addrlo;
			uint32_t addrhi;
			uint32_t flags2;
			uint32_t datalo;
			uint32_t datahi;
		} eveop;
		struct {
			uint32_t type;
			uint32_t addrlo;
			uint32_t addrhi;
			uint32_t sel;
			uint32_t val;
		} eveos;
		struct {
			uint8_t src_sel;
			uint8_t dst_sel;
			uint64_t src_va;
			uint64_t dst_va;
			uint32_t length;
			bool wr_confirm;
		} dma_data;
		struct {
			uint32_t coherctrl;
			uint32_t cohersize;
			uint32_t cohersizehi;
			uint32_t coherbase;
			uint32_t coherbasehi;
			uint32_t pollint;
		} acqmem;
		struct {
			uint32_t reg;
		} setctxreg;
		struct {
			uint32_t reg;
		} setshreg;
		struct {
			uint32_t reg;
		} setucfgreg;
	};
} Pm4Packet3;

typedef struct {
	Pm4PacketType type;
	uint32_t count;
	uint32_t offset;

	struct {
		uint32_t baseindex;
	} pkt0;
	Pm4Packet3 pkt3;
} Pm4Packet;

const char* pm4StrType(Pm4PacketType type);
const char* pm4StrOpcode(uint32_t opcode);

#endif	// _PM4_TYPES_H_
