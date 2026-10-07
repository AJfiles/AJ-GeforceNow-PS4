#include "gnm_error.h"

#include <stdarg.h>
#include <stdio.h>

static GnmMessageHandlerFunc s_handler = 0;
static void* s_userdata = 0;

const char* sceGnmStrError(GnmError err) PS4_SYSV_ABI {
	switch (err) {
	case GNM_ERROR_OK:
		return "No error";
	case ORBIS_GNM_ERROR_SUBMISSION_FAILED_INVALID_ARGUMENT:
		return "An invalid argument was passed to the submit function";
	case ORBIS_GNM_ERROR_SUBMISSION_NOT_ENOUGH_RESOURCES:
		return "There are not enough resources to submit the command "
		       "buffers";
	case ORBIS_GNM_ERROR_SUBMISSION_AND_FLIP_FAILED_INVALID_COMMAND_BUFFER:
		return "prepareFlip has not been called";
	case ORBIS_GNM_ERROR_SUBMISSION_AND_FLIP_FAILED_INVALID_QUEUE_FULL:
		return "The flip queue is full";
	case ORBIS_GNM_ERROR_SUBMISSION_AND_FLIP_FAILED_REQUEST_FAILED:
		return "The flip request has failed";
	case ORBIS_GNM_ERROR_SUBMISSION_FAILED_INTERNAL_ERROR:
		return "An internal submission error has occured";
	case GNM_ERROR_INVALID_ARGS:
		return "An invalid argument was passed";
	case GNM_ERROR_INTERNAL_FAILURE:
		return "An internal failure occured";
	case GNM_ERROR_CMD_FAILED:
		return "Failed to write command to command buffer";
	case GNM_ERROR_INVALID_ALIGNMENT:
		return "A value uses an invalid alignment";
	case GNM_ERROR_INVALID_STATE:
		return "An invalid state was found";
	case GNM_ERROR_OVERFLOW:
		return "A buffer has overflown";
	case GNM_ERROR_UNDERFLOW:
		return "A buffer has underflown";
	case GNM_ERROR_UNSUPPORTED:
		return "A feature is not supported";
	case GNM_ERROR_SEMANTIC_NOT_FOUND:
		return "A semantic was not found";
	case GNM_ERROR_ASM_FAILED:
		return "The GCN assembler failed";
	default:
		return "Unknown error";
	}
}

void sceGnmSetMessageHandler(GnmMessageHandlerFunc handlerfunc, void* userdata) {
	s_handler = handlerfunc;
	s_userdata = userdata;
}

void sceGnmWriteMsg(GnmMessageSeverity sev, const char* msg) {
	if (s_handler) {
		s_handler(sev, msg, s_userdata);
	}
}

void sceGnmWriteMsgf(GnmMessageSeverity sev, const char* fmt, ...) {
	if (s_handler) {
		char buf[256] = {0};

		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);

		s_handler(sev, buf, s_userdata);
	}
}
