#include "pm4_error.h"

const char* pm4StrError(Pm4Error err) {
	switch (err) {
	case PM4_ERR_OK:
		return "No error";
	case PM4_ERR_END_OF_CODE:
		return "End of code was reached";
	case PM4_ERR_INVALID_ARG:
		return "An invalid argument was used";
	case PM4_ERR_INVALID_TYPE:
		return "An invalid packet type was used";
	case PM4_ERR_BUFFER_TOO_SMALL:
		return "The buffer length is too small";
	case PM4_ERR_PKT_TOO_SMALL:
		return "The packet's length is too small";
	case PM4_ERR_SIZE_NOT_A_MULTIPLE_FOUR:
		return "The command length is not a multiple of 4";
	case PM4_ERR_UNINITIALIZED_CONTEXT:
		return "Used context is not initialized";
	case PM4_ERR_INTERNAL_ERROR:
		return "An internal error occured";
	case PM4_ERR_OVERFLOW:
		return "A buffer has overflown";
	default:
		return "Unknown error";
	}
}
