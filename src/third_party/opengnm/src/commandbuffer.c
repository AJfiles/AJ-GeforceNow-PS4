#include "gnm_commandbuffer.h"
#include "gnm_error.h"

#include "pm4/sid.h"

static bool cmddefaultcallback(
    GnmCommandBuffer* cmdbuf, uint32_t sizedwords, void* userdata
) {
	(void)cmdbuf;
	(void)sizedwords;
	(void)userdata;

	sceGnmWriteMsg(
	    GNM_MSGSEV_ERR,
	    "Command buffer is full, and no custom callback was specified"
	);
	return false;
}

GnmCommandBuffer sceGnmCmdInit(
    void* buffer, uint32_t bytesize, GnmCommandCallbackFunc* cb, void* cbdata
) {
	const uint32_t numdwords = bytesize / sizeof(uint32_t);
	void* endptr = (uint8_t*)buffer + (numdwords * sizeof(uint32_t));

	GnmCommandBuffer res;
	res.beginptr = buffer;
	res.endptr = endptr;
	res.cmdptr = buffer;
	res.callback.func = cb ? *cb : cmddefaultcallback;
	res.callback.userdata = cbdata;
	res.sizedwords = numdwords;
	return res;
}

void* sceGnmCmdAllocInside(
    GnmCommandBuffer* cmd, uint32_t size, uint32_t alignment
) {
	if (size == 0) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "AllocInside: size must not be 0");
		return 0;
	}
	if (alignment < 4) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "AllocInside: alignment must be 4 or larger"
		);
		return 0;
	}
	if (alignment & 3) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "AllocInside: alignment must be a multiple of 4"
		);
		return 0;
	}
	if (alignment & (alignment - 1)) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "AllocInside: alignment must be a power of two"
		);
		return 0;
	}

	const uint32_t startoff =
	    (cmd->cmdptr - cmd->beginptr + 1) * sizeof(uint32_t);
	const uint32_t alignoff =
	    (startoff + (alignment - 1)) & (~(alignment - 1));

	const uint32_t aligndwords = (alignoff - startoff) / sizeof(uint32_t);
	const uint32_t sizedwords = (size + sizeof(uint32_t) - 1) / sizeof(uint32_t);
	const uint32_t numdwords = 1 + aligndwords + sizedwords;
	if (cmd->cmdptr + numdwords > cmd->endptr) {
		if (!cmd->callback.func(
			cmd, numdwords, cmd->callback.userdata
		    )) {
			sceGnmWriteMsg(
			    GNM_MSGSEV_ERR, "AllocInside: out of memory"
			);
			return 0;
		}
		if (cmd->cmdptr + numdwords > cmd->endptr) {
			sceGnmWriteMsg(
			    GNM_MSGSEV_ERR, "AllocInside: resized buffer is too small"
			);
			return 0;
		}
	}

	uint32_t* res = cmd->cmdptr + 1 + aligndwords;
	cmd->cmdptr[0] = PKT3(PKT3_NOP, numdwords - 2, 0);
	cmd->cmdptr += numdwords;
	return res;
}
