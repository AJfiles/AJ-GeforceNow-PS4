#include "gcn_error.h"

const char* gcnStrError(GcnError err) {
	switch (err) {
	case GCN_ERR_OK:
		return "No error";
	case GCN_ERR_END_OF_CODE:
		return "End of code was reached";
	case GCN_ERR_INVALID_ARG:
		return "An invalid argument was used";
	case GCN_ERR_BUFFER_TOO_SMALL:
		return "The buffer length is too small";
	case GCN_ERR_CODE_TOO_SMALL:
		return "The code length is too small";
	case GCN_ERR_SIZE_NOT_A_MULTIPLE_FOUR:
		return "The code length is not a multiple of 4";
	case GCN_ERR_UNINITIALIZED_CONTEXT:
		return "Used context is not initialized";
	case GCN_ERR_INVALID_MICROCODE:
		return "An invalid microcode in an instruction was used";
	case GCN_ERR_INVALID_OPCODE:
		return "An invalid opcode in an instruction was used";
	case GCN_ERR_INVALID_SRC_FIELD:
		return "An invalid source operand field in an instruction was "
		       "used";
	case GCN_ERR_INVALID_DST_FIELD:
		return "An invalid destination operand field in an instruction "
		       "was used";
	case GCN_ERR_INTERNAL_ERROR:
		return "An internal error occured";
	case GCN_ERR_OVERFLOW:
		return "A buffer has overflown";
	case GCN_ERR_RESOURCE_MISMATCH:
		return "A resource type mismatch was found";
	case GCN_ERR_TOO_MANY_CONSTS:
		return "Too many constants we're used in an operation";
	case GCN_ERR_UNIMPLEMENTED:
		return "This feature is unimplemented";
	default:
		return "Unknown error";
	}
}
