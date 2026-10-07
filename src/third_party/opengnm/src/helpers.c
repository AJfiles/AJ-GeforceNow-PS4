#include "gnm_helpers.h"

#include <stdlib.h>
#include <string.h>

#ifndef __has_include
#define __has_include(_file) 0
#endif

#if defined(OPENGNM_ORBIS) && __has_include(<orbis/libkernel.h>)
#include <orbis/libkernel.h>
#include <sys/types.h>
#define OPENGNM_HELPERS_HAS_ORBIS_LIBKERNEL 1
#else
#define OPENGNM_HELPERS_HAS_ORBIS_LIBKERNEL 0
#endif

#if defined(OPENGNM_ORBIS) && __has_include(<orbis/VideoOut.h>)
#include <orbis/VideoOut.h>
#define OPENGNM_HELPERS_HAS_ORBIS_VIDEOOUT 1
#else
#define OPENGNM_HELPERS_HAS_ORBIS_VIDEOOUT 0
#endif

#if defined(OPENGNM_ORBIS) && !OPENGNM_HELPERS_HAS_ORBIS_LIBKERNEL
extern int64_t sceKernelGetDirectMemorySize(void);
extern int32_t sceKernelAllocateDirectMemory(
    int64_t searchstart, int64_t searchend, uint64_t len,
    uint64_t alignment, int32_t memorytype, int64_t* outoffset
);
extern int32_t sceKernelMapDirectMemory(
    void** outaddr, uint64_t len, int32_t protection, int32_t flags,
    int64_t directmemoryoffset, uint64_t alignment
);
extern int32_t sceKernelMunmap(void* addr, uint64_t len);
extern int32_t sceKernelReleaseDirectMemory(int64_t start, uint64_t len);
#endif

#ifndef ORBIS_VIDEO_OUT_ASPECT_RATIO_NONE
#define ORBIS_VIDEO_OUT_ASPECT_RATIO_NONE GNM_VIDEO_OUT_ASPECT_RATIO_NONE
#endif
#ifndef ORBIS_VIDEO_OUT_BUS_MAIN
#define ORBIS_VIDEO_OUT_BUS_MAIN GNM_VIDEO_OUT_BUS_MAIN
#endif
#ifndef ORBIS_VIDEO_OUT_FLIP_60HZ
#define ORBIS_VIDEO_OUT_FLIP_60HZ GNM_VIDEO_OUT_FLIP_60HZ
#endif
#ifndef ORBIS_VIDEO_OUT_FLIP_VSYNC
#define ORBIS_VIDEO_OUT_FLIP_VSYNC GNM_VIDEO_OUT_FLIP_VSYNC
#endif
#ifndef ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB
#define ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB \
	GNM_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB
#endif
#ifndef ORBIS_VIDEO_OUT_TILING_MODE_LINEAR
#define ORBIS_VIDEO_OUT_TILING_MODE_LINEAR GNM_VIDEO_OUT_TILING_MODE_LINEAR
#endif

static bool is_power_of_two_u64(uint64_t value) {
	return value && ((value & (value - 1)) == 0);
}

static bool align_up_u64(uint64_t value, uint64_t alignment, uint64_t* out) {
	if (!is_power_of_two_u64(alignment)) {
		return false;
	}
	const uint64_t mask = alignment - 1;
	if (value > UINT64_MAX - mask) {
		return false;
	}
	*out = (value + mask) & ~mask;
	return true;
}

static bool mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
	if (a && b > UINT64_MAX / a) {
		return false;
	}
	*out = a * b;
	return true;
}

GnmError PS4_SYSV_ABI sceGnmDirectMemoryAllocate(
    GnmDirectMemory* memory, uint64_t size, uint64_t alignment,
    int32_t memorytype, int32_t protection
) {
	if (!memory || !size) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!is_power_of_two_u64(alignment)) {
		return GNM_ERROR_INVALID_ALIGNMENT;
	}

	memset(memory, 0, sizeof(*memory));
	if (!align_up_u64(size, alignment, &memory->size)) {
		return GNM_ERROR_OVERFLOW;
	}
	memory->alignment = alignment;

#if defined(OPENGNM_ORBIS)
	int64_t directmemory = 0;
	int32_t result = sceKernelAllocateDirectMemory(
	    0, sceKernelGetDirectMemorySize(), memory->size, alignment,
	    memorytype, &directmemory
	);
	if (result != 0) {
		memset(memory, 0, sizeof(*memory));
		return (GnmError)result;
	}

	void* mapped = NULL;
	result = sceKernelMapDirectMemory(
	    &mapped, memory->size, protection, 0, directmemory, alignment
	);
	if (result != 0) {
		sceKernelReleaseDirectMemory(directmemory, memory->size);
		memset(memory, 0, sizeof(*memory));
		return (GnmError)result;
	}
	memory->mapped = mapped;
	memory->directmemory = (uint64_t)directmemory;
	memory->allocated = true;
	return GNM_ERROR_OK;
#else
	(void)memorytype;
	(void)protection;
	if (memory->size > UINT64_MAX - alignment) {
		memset(memory, 0, sizeof(*memory));
		return GNM_ERROR_OVERFLOW;
	}
	uint64_t allocsize = 0;
	if (!align_up_u64(memory->size + alignment, alignment, &allocsize)) {
		memset(memory, 0, sizeof(*memory));
		return GNM_ERROR_OVERFLOW;
	}
	if (allocsize > (uint64_t)SIZE_MAX) {
		memset(memory, 0, sizeof(*memory));
		return GNM_ERROR_OVERFLOW;
	}
	void* raw = malloc((size_t)allocsize);
	if (!raw) {
		memset(memory, 0, sizeof(*memory));
		return GNM_ERROR_INTERNAL_FAILURE;
	}
	uintptr_t aligned = ((uintptr_t)raw + (uintptr_t)alignment - 1u) &
			    ~((uintptr_t)alignment - 1u);
	memory->allocation = raw;
	memory->mapped = (void*)aligned;
	memory->directmemory = (uint64_t)(uintptr_t)memory->mapped;
	memory->allocated = true;
	return GNM_ERROR_OK;
#endif
}

void PS4_SYSV_ABI sceGnmDirectMemoryRelease(GnmDirectMemory* memory) {
	if (!memory) {
		return;
	}
#if defined(OPENGNM_ORBIS)
	if (memory->mapped) {
		sceKernelMunmap(memory->mapped, memory->size);
	}
	if (memory->allocated) {
		sceKernelReleaseDirectMemory(
		    (int64_t)memory->directmemory, memory->size
		);
	}
#else
	free(memory->allocation);
#endif
	memset(memory, 0, sizeof(*memory));
}

void PS4_SYSV_ABI sceGnmVideoOutInitDefaultCreateInfo(
    GnmVideoOutCreateInfo* info, uint32_t width, uint32_t height
) {
	if (!info) {
		return;
	}
	memset(info, 0, sizeof(*info));
	info->width = width ? width : GNM_VIDEO_OUT_DEFAULT_WIDTH;
	info->height = height ? height : GNM_VIDEO_OUT_DEFAULT_HEIGHT;
	info->pitch = info->width;
	info->numbuffers = GNM_VIDEO_OUT_DEFAULT_BUFFERS;
	info->bytesperpixel = GNM_VIDEO_OUT_BYTES_PER_PIXEL_A8B8G8R8;
	info->alignment = GNM_VIDEO_OUT_MEMORY_ALIGNMENT;
	info->bus = GNM_VIDEO_OUT_BUS_MAIN;
	info->pixel_format = (int32_t)GNM_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB;
	info->tiling_mode = GNM_VIDEO_OUT_TILING_MODE_LINEAR;
	info->aspect_ratio = GNM_VIDEO_OUT_ASPECT_RATIO_NONE;
	info->flip_rate = GNM_VIDEO_OUT_FLIP_60HZ;
}

GnmError PS4_SYSV_ABI sceGnmVideoOutCalcBufferLayout(
    const GnmVideoOutCreateInfo* info, uint64_t* outbuffersize,
    uint64_t* outbufferstride
) {
	if (!info || !info->width || !info->height || !info->pitch ||
	    !info->bytesperpixel || !is_power_of_two_u64(info->alignment)) {
		return GNM_ERROR_INVALID_ARGS;
	}

	uint64_t rowbytes = 0;
	uint64_t size = 0;
	if (!mul_u64(info->pitch, info->bytesperpixel, &rowbytes) ||
	    !mul_u64(rowbytes, info->height, &size)) {
		return GNM_ERROR_OVERFLOW;
	}

	uint64_t stride = 0;
	if (!align_up_u64(size, info->alignment, &stride)) {
		return GNM_ERROR_OVERFLOW;
	}
	if (outbuffersize) {
		*outbuffersize = size;
	}
	if (outbufferstride) {
		*outbufferstride = stride;
	}
	return GNM_ERROR_OK;
}

GnmError PS4_SYSV_ABI sceGnmVideoOutOpen(
    GnmVideoOut* videoout, const GnmVideoOutCreateInfo* info
) {
	if (!videoout) {
		return GNM_ERROR_INVALID_ARGS;
	}

	memset(videoout, 0, sizeof(*videoout));
	videoout->handle = -1;
	videoout->last_error_stage = 1;
	videoout->last_error_code = GNM_ERROR_INVALID_ARGS;
	if (!info || info->numbuffers == 0 ||
	    info->numbuffers > GNM_VIDEO_OUT_MAX_BUFFERS) {
		return GNM_ERROR_INVALID_ARGS;
	}
	videoout->last_error_code = GNM_ERROR_OK;

	uint64_t buffersize = 0;
	uint64_t bufferstride = 0;
	GnmError err =
	    sceGnmVideoOutCalcBufferLayout(info, &buffersize, &bufferstride);
	if (err != GNM_ERROR_OK) {
		videoout->last_error_code = (int32_t)err;
		return err;
	}
	uint64_t totalbuffersize = 0;
	if (!mul_u64(bufferstride, info->numbuffers, &totalbuffersize)) {
		videoout->last_error_stage = 1;
		videoout->last_error_code = GNM_ERROR_OVERFLOW;
		return GNM_ERROR_OVERFLOW;
	}

#if OPENGNM_HELPERS_HAS_ORBIS_LIBKERNEL && OPENGNM_HELPERS_HAS_ORBIS_VIDEOOUT
	videoout->handle = sceVideoOutOpen(0, info->bus, 0, NULL);
	if (videoout->handle < 0) {
		videoout->last_error_stage = 2;
		videoout->last_error_code = videoout->handle;
		err = (GnmError)videoout->handle;
		sceGnmVideoOutClose(videoout);
		return err;
	}

	err = sceGnmDirectMemoryAllocate(
	    &videoout->memory, totalbuffersize, info->alignment,
	    GNM_DIRECT_MEMORY_TYPE_WC_GARLIC, GNM_PROT_CPU_GPU_RW
	);
	if (err != GNM_ERROR_OK) {
		videoout->last_error_stage = 3;
		videoout->last_error_code = (int32_t)err;
		sceGnmVideoOutClose(videoout);
		return err;
	}

	for (uint32_t i = 0; i < info->numbuffers; i += 1) {
		videoout->buffers[i] =
		    (uint8_t*)videoout->memory.mapped + i * bufferstride;
		memset(videoout->buffers[i], 0, (size_t)buffersize);
	}
	videoout->width = info->width;
	videoout->height = info->height;
	videoout->pitch = info->pitch;
	videoout->numbuffers = info->numbuffers;
	videoout->buffersize = buffersize;
	videoout->bufferstride = bufferstride;

	OrbisVideoOutBufferAttribute attr;
	memset(&attr, 0, sizeof(attr));
	sceVideoOutSetBufferAttribute(
	    &attr, info->pixel_format, info->tiling_mode, info->aspect_ratio,
	    info->width, info->height, info->pitch
	);

	int32_t result = sceVideoOutRegisterBuffers(
	    videoout->handle, 0, videoout->buffers, (int32_t)info->numbuffers,
	    &attr
	);
	if (result < 0) {
		videoout->last_error_stage = 4;
		videoout->last_error_code = result;
		sceGnmVideoOutClose(videoout);
		return (GnmError)result;
	}
	videoout->registeredbuffers = info->numbuffers;

	OrbisKernelEqueue queue = 0;
	result = sceKernelCreateEqueue(&queue, "opengnm videoout flips");
	if (result != 0) {
		videoout->last_error_stage = 5;
		videoout->last_error_code = result;
		sceGnmVideoOutClose(videoout);
		return (GnmError)result;
	}
	videoout->flipqueue = (uintptr_t)queue;

	result = sceVideoOutAddFlipEvent(queue, videoout->handle, NULL);
	if (result != 0) {
		videoout->last_error_stage = 6;
		videoout->last_error_code = result;
		sceGnmVideoOutClose(videoout);
		return (GnmError)result;
	}

	sceVideoOutSetFlipRate(videoout->handle, info->flip_rate);
	videoout->last_error_stage = 0;
	return GNM_ERROR_OK;
#else
	(void)buffersize;
	(void)bufferstride;
	(void)totalbuffersize;
	videoout->last_error_code = GNM_ERROR_UNSUPPORTED;
	return GNM_ERROR_UNSUPPORTED;
#endif
}

void PS4_SYSV_ABI sceGnmVideoOutClose(GnmVideoOut* videoout) {
	if (!videoout) {
		return;
	}
#if OPENGNM_HELPERS_HAS_ORBIS_LIBKERNEL && OPENGNM_HELPERS_HAS_ORBIS_VIDEOOUT
	if (videoout->handle >= 0) {
		for (uint32_t i = 0; i < videoout->registeredbuffers; i += 1) {
			sceVideoOutUnregisterBuffers(videoout->handle, (int32_t)i);
		}
		sceVideoOutClose(videoout->handle);
	}
	if (videoout->flipqueue) {
		sceKernelDeleteEqueue((OrbisKernelEqueue)videoout->flipqueue);
	}
	sceGnmDirectMemoryRelease(&videoout->memory);
#endif
	memset(videoout, 0, sizeof(*videoout));
	videoout->handle = -1;
}

void* PS4_SYSV_ABI sceGnmVideoOutGetBuffer(
    const GnmVideoOut* videoout, uint32_t bufferindex
) {
	if (!videoout || bufferindex >= videoout->numbuffers) {
		return NULL;
	}
	return videoout->buffers[bufferindex];
}

GnmError PS4_SYSV_ABI sceGnmVideoOutSubmitFlipAndWait(
    GnmVideoOut* videoout, uint32_t bufferindex, int64_t fliparg,
    int32_t flipmode
) {
	if (!videoout || videoout->handle < 0 ||
	    bufferindex >= videoout->numbuffers) {
		return GNM_ERROR_INVALID_ARGS;
	}
#if OPENGNM_HELPERS_HAS_ORBIS_LIBKERNEL && OPENGNM_HELPERS_HAS_ORBIS_VIDEOOUT
	int32_t result = sceVideoOutSubmitFlip(
	    videoout->handle, (int32_t)bufferindex, flipmode, fliparg
	);
	if (result != 0) {
		return (GnmError)result;
	}
	if (videoout->flipqueue) {
		OrbisKernelEvent event;
		memset(&event, 0, sizeof(event));
		int32_t out = 0;
		result = sceKernelWaitEqueue(
		    (OrbisKernelEqueue)videoout->flipqueue, &event, 1, &out, 0
		);
		if (result != 0) {
			return (GnmError)result;
		}
	}
	videoout->frame += 1;
	videoout->currentbuffer = (bufferindex + 1) % videoout->numbuffers;
	return GNM_ERROR_OK;
#else
	return GNM_ERROR_UNSUPPORTED;
#endif
}

void PS4_SYSV_ABI sceGnmTexInit2dCreateInfo(
    GnmTextureCreateInfo* info, GnmDataFormat format, uint32_t width,
    uint32_t height, uint32_t mipcount, GnmTileMode tilemode,
    GnmGpuMode mingpumode
) {
	if (!info) {
		return;
	}
	memset(info, 0, sizeof(*info));
	info->format = format;
	info->texturetype = GNM_TEXTURE_2D;
	info->width = width;
	info->height = height;
	info->depth = 1;
	info->pitch = width;
	info->nummiplevels = mipcount ? mipcount : 1;
	info->numslices = 1;
	info->numfragments = 1;
	info->tilemodehint = tilemode;
	info->mingpumode = mingpumode;
}

GnmError PS4_SYSV_ABI sceGnmTexCreate2d(
    GnmTexture* texture, void* baseaddr, GnmDataFormat format,
    uint32_t width, uint32_t height, uint32_t mipcount,
    GnmTileMode tilemode, GnmGpuMode mingpumode, uint64_t* outsize,
    uint32_t* outalignment
) {
	if (!texture) {
		return GNM_ERROR_INVALID_ARGS;
	}
	memset(texture, 0, sizeof(*texture));
	GnmTextureCreateInfo info;
	sceGnmTexInit2dCreateInfo(
	    &info, format, width, height, mipcount, tilemode, mingpumode
	);
	GnmError err = sceGnmCreateTexture(texture, &info);
	if (err != GNM_ERROR_OK) {
		return err;
	}
	if (baseaddr) {
		sceGnmTexSetBaseAddress(texture, baseaddr);
		sceGnmTexSetMemoryType(texture, GNM_MEMORY_READONLY, false);
	}
	uint64_t size = 0;
	uint32_t alignment = 0;
	err = sceGnmTexCalcByteSize(&size, &alignment, texture);
	if (err != GNM_ERROR_OK) {
		return err;
	}
	if (outsize) {
		*outsize = size;
	}
	if (outalignment) {
		*outalignment = alignment;
	}
	return GNM_ERROR_OK;
}

void PS4_SYSV_ABI sceGnmRtInitColorTargetCreateInfo(
    GnmRenderTargetCreateInfo* info, GnmDataFormat format, uint32_t width,
    uint32_t height, uint32_t numslices, uint32_t numsamples,
    uint32_t numfragments, GnmTileMode tilemode, GnmGpuMode mingpumode
) {
	if (!info) {
		return;
	}
	memset(info, 0, sizeof(*info));
	info->colorfmt = format;
	info->width = width;
	info->height = height;
	info->pitch = width;
	info->numslices = numslices ? numslices : 1;
	info->numsamples = numsamples ? numsamples : 1;
	info->numfragments = numfragments ? numfragments : 1;
	info->colortilemodehint = tilemode;
	info->mingpumode = mingpumode;
}

GnmError PS4_SYSV_ABI sceGnmRtCreateColorTarget(
    GnmRenderTarget* rt, void* baseaddr, GnmDataFormat format,
    uint32_t width, uint32_t height, uint32_t numslices,
    uint32_t numsamples, uint32_t numfragments, GnmTileMode tilemode,
    GnmGpuMode mingpumode, uint64_t* outsize, uint32_t* outalignment
) {
	if (!rt) {
		return GNM_ERROR_INVALID_ARGS;
	}
	memset(rt, 0, sizeof(*rt));
	GnmRenderTargetCreateInfo info;
	sceGnmRtInitColorTargetCreateInfo(
	    &info, format, width, height, numslices, numsamples, numfragments,
	    tilemode, mingpumode
	);
	GnmError err = sceGnmCreateRenderTarget(rt, &info);
	if (err != GNM_ERROR_OK) {
		return err;
	}
	if (baseaddr) {
		sceGnmRtSetBaseAddr(rt, baseaddr);
	}
	uint64_t size = 0;
	uint32_t alignment = 0;
	err = sceGnmRtCalcByteSize(&size, &alignment, rt);
	if (err != GNM_ERROR_OK) {
		return err;
	}
	if (outsize) {
		*outsize = size;
	}
	if (outalignment) {
		*outalignment = alignment;
	}
	return GNM_ERROR_OK;
}

static bool range_inside(
    const uint8_t* base, size_t size, const void* ptr, size_t len
) {
	const uintptr_t start = (uintptr_t)base;
	const uintptr_t p = (uintptr_t)ptr;
	if (p < start) {
		return false;
	}
	const uintptr_t rel = p - start;
	return rel <= size && len <= size - rel;
}

GnmError PS4_SYSV_ABI sceGnmShaderBinaryGetMetadata(
    const void* data, size_t size, GnmShaderMetadata* outmetadata
) {
	if (!data || !outmetadata || size < sizeof(GnmShaderFileHeader)) {
		return GNM_ERROR_INVALID_ARGS;
	}

	const uint8_t* containerbase = (const uint8_t*)data;
	size_t shaderoffset = 0;
	bool wrappedcontainer = false;
	const GnmShaderFileHeader* header = (const GnmShaderFileHeader*)containerbase;
	if (header->magic != GNM_SHADER_FILE_HEADER_ID &&
	    size >= 0x24 + sizeof(GnmShaderFileHeader)) {
		const GnmShaderFileHeader* wrapped =
		    (const GnmShaderFileHeader*)(containerbase + 0x24);
		if (wrapped->magic == GNM_SHADER_FILE_HEADER_ID) {
			shaderoffset = 0x24;
			wrappedcontainer = true;
			header = wrapped;
		}
	}
	if (header->magic != GNM_SHADER_FILE_HEADER_ID) {
		return GNM_ERROR_INVALID_ARGS;
	}
	const uint8_t* base = containerbase + shaderoffset;
	const size_t shadersize = size - shaderoffset;

	const size_t stageheaderbytes =
	    header->headersizedwords ? header->headersizedwords * 4u
				     : sizeof(GnmShaderCommonData);
	if (stageheaderbytes < sizeof(GnmShaderCommonData) ||
	    sizeof(GnmShaderFileHeader) + stageheaderbytes > shadersize) {
		return GNM_ERROR_INVALID_ARGS;
	}

	memset(outmetadata, 0, sizeof(*outmetadata));
	outmetadata->type = (GnmShaderType)header->type;
	outmetadata->targetgpumodes = (GnmTargetGpuMode)header->targetgpumodes;
	outmetadata->versionmajor = header->vermajor;
	outmetadata->versionminor = header->verminor;
	outmetadata->fileheader = header;
	outmetadata->common = (const GnmShaderCommonData*)(
	    base + sizeof(GnmShaderFileHeader)
	);
	outmetadata->stage = outmetadata->common;
	outmetadata->shadercodesize =
	    sceGnmShaderCommonCodeSize(outmetadata->common);
	if (wrappedcontainer) {
		uint32_t containercodesize = 0;
		memcpy(&containercodesize, containerbase + 0x10, sizeof(containercodesize));
		if (containercodesize < sizeof(GnmShaderBinaryInfo) ||
		    shaderoffset + containercodesize > size) {
			return GNM_ERROR_INVALID_ARGS;
		}
		const GnmShaderBinaryInfo* binaryinfo =
		    (const GnmShaderBinaryInfo*)(base + containercodesize -
					      sizeof(GnmShaderBinaryInfo));
		outmetadata->shadercodesize = binaryinfo->length;
	}
	outmetadata->numinputusageslots =
	    outmetadata->common->numinputusageslots;

	switch (outmetadata->type) {
	case GNM_SHADER_VERTEX: {
		if (sizeof(GnmShaderFileHeader) + sizeof(GnmVsShader) > shadersize) {
			return GNM_ERROR_INVALID_ARGS;
		}
		const GnmVsShader* vs = (const GnmVsShader*)outmetadata->stage;
		const uint32_t stagesize = sceGnmVsShaderCalcSize(vs);
		if (stagesize > stageheaderbytes ||
		    sizeof(GnmShaderFileHeader) + stagesize > shadersize) {
			return GNM_ERROR_INVALID_ARGS;
		}
		outmetadata->stagesize = stagesize;
		outmetadata->inputusageslots = sceGnmVsShaderInputUsageSlotTable(vs);
		outmetadata->numinputsemantics = vs->numinputsemantics;
		outmetadata->numexportsemantics = vs->numexportsemantics;
		outmetadata->shadercode = sceGnmVsShaderCodePtr(vs);
		break;
	}
	case GNM_SHADER_PIXEL: {
		if (sizeof(GnmShaderFileHeader) + sizeof(GnmPsShader) > shadersize) {
			return GNM_ERROR_INVALID_ARGS;
		}
		const GnmPsShader* ps = (const GnmPsShader*)outmetadata->stage;
		const uint32_t stagesize = sceGnmPsShaderCalcSize(ps);
		if (stagesize > stageheaderbytes ||
		    sizeof(GnmShaderFileHeader) + stagesize > shadersize) {
			return GNM_ERROR_INVALID_ARGS;
		}
		outmetadata->stagesize = stagesize;
		outmetadata->inputusageslots = sceGnmPsShaderInputUsageSlotTable(ps);
		outmetadata->numinputsemantics = ps->numinputsemantics;
		outmetadata->shadercode = sceGnmPsShaderCodePtr(ps);
		break;
	}
	default:
		outmetadata->stagesize = sizeof(GnmShaderCommonData);
		break;
	}

	if (outmetadata->shadercode &&
	    !range_inside(
		base, shadersize, outmetadata->shadercode,
		outmetadata->shadercodesize
	    )) {
		return GNM_ERROR_INVALID_STATE;
	}

	return GNM_ERROR_OK;
}

static void fill_cmd_validation(
    const GnmCommandBuffer* cmd, GnmCommandBufferValidationInfo* outinfo,
    const char* message
) {
	if (!outinfo) {
		return;
	}
	memset(outinfo, 0, sizeof(*outinfo));
	outinfo->message = message;
	if (!cmd || !cmd->beginptr || !cmd->endptr || !cmd->cmdptr) {
		return;
	}
	const uintptr_t begin = (uintptr_t)cmd->beginptr;
	const uintptr_t end = (uintptr_t)cmd->endptr;
	const uintptr_t cur = (uintptr_t)cmd->cmdptr;
	if (end >= begin && cur >= begin && cur <= end) {
		outinfo->capacitydwords = (uint32_t)((end - begin) / 4);
		outinfo->useddwords = (uint32_t)((cur - begin) / 4);
		outinfo->remainingdwords =
		    outinfo->capacitydwords - outinfo->useddwords;
	}
}

GnmError PS4_SYSV_ABI sceGnmCmdValidate(
    const GnmCommandBuffer* cmd, GnmCommandBufferValidationInfo* outinfo
) {
	if (!cmd) {
		fill_cmd_validation(cmd, outinfo, "command buffer is null");
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!cmd->beginptr || !cmd->endptr || !cmd->cmdptr) {
		fill_cmd_validation(cmd, outinfo, "command buffer has null pointers");
		return GNM_ERROR_INVALID_ARGS;
	}
	const uintptr_t begin = (uintptr_t)cmd->beginptr;
	const uintptr_t end = (uintptr_t)cmd->endptr;
	const uintptr_t cur = (uintptr_t)cmd->cmdptr;
	if ((begin & 3u) || (end & 3u) || (cur & 3u)) {
		fill_cmd_validation(cmd, outinfo, "command buffer pointers are unaligned");
		return GNM_ERROR_INVALID_ALIGNMENT;
	}
	if (end < begin) {
		fill_cmd_validation(cmd, outinfo, "command buffer end is before begin");
		return GNM_ERROR_INVALID_STATE;
	}
	if (cur < begin || cur > end) {
		fill_cmd_validation(cmd, outinfo, "command buffer cursor is out of range");
		return GNM_ERROR_OVERFLOW;
	}
	const uint32_t capacity = (uint32_t)((end - begin) / 4);
	if (cmd->sizedwords && cmd->sizedwords != capacity) {
		fill_cmd_validation(cmd, outinfo, "command buffer size metadata is stale");
		return GNM_ERROR_INVALID_STATE;
	}
	fill_cmd_validation(cmd, outinfo, "ok");
	return GNM_ERROR_OK;
}
