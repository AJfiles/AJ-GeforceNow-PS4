#include "gnm_depthrendertarget.h"

#include "gpuaddr.h"
#include "platform.h"

#include "u/utility.h"

static inline bool drt_has_depth(const GnmDepthRenderTarget* drt) {
	return drt->zinfo.format != GNM_Z_INVALID;
}

static inline bool drt_has_stencil(const GnmDepthRenderTarget* drt) {
	return drt->stencilinfo.format != GNM_STENCIL_INVALID;
}

static inline uint64_t alignu64(uint64_t value, uint32_t alignment) {
	if (alignment <= 1) {
		return value;
	}
	return ((value + alignment - 1) / alignment) * alignment;
}

static GnmDataFormat drt_plane_format(
    const GnmDepthRenderTarget* drt, bool stencil
) {
	return stencil ? sceGnmDfInitFromStencil(
			     drt->stencilinfo.format, GNM_IMG_NUM_FORMAT_UINT
			 )
		       : sceGnmDfInitFromZ(drt->zinfo.format);
}

static GnmError drt_compute_plane_surface_info(
    GpaSurfaceInfo* outinfo, const GnmDepthRenderTarget* drt, bool stencil
) {
	const GnmDataFormat dfmt = drt_plane_format(drt, stencil);
	const uint32_t numtexels = sceGnmDfGetTexelsPerElement(dfmt);
	const uint32_t numtotalbits = sceGnmDfGetTotalBitsPerElement(dfmt);
	if (numtexels == 0 || numtotalbits == 0) {
		return GNM_ERROR_INVALID_STATE;
	}

	const GnmGpuMode mingpumode = sceGnmDrtGetMinGpuMode(drt);
	const GpaTilingParams tp = {
	    .tilemode = stencil ? drt->stencilinfo.tilemodeindex
				: drt->zinfo.tilemodeindex,
	    .mingpumode = mingpumode,

	    .linearwidth = sceGnmDrtGetPaddedWidth(drt),
	    .linearheight = sceGnmDrtGetPaddedHeight(drt),
	    .lineardepth = 1,
	    .numfragsperpixel = sceGnmDrtGetNumFragments(drt),
	    .basetiledpitch = sceGnmDrtGetPaddedWidth(drt),

	    .miplevel = 0,
	    .arrayslice = 0,
	    .surfaceflags =
		{
		    .depthtarget = !stencil,
		    .stenciltarget = stencil,
		    .texcompatible = mingpumode == GNM_GPU_NEO,
		},
	    .bitsperfrag = numtotalbits / numtexels,
	    .isblockcompressed = numtexels > 1,
	};

	GpaError err = sceGpaComputeSurfaceInfo(outinfo, &tp);
	if (err != GPA_ERR_OK) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN,
		    "Drt: failed to compute %s surface info with %s",
		    stencil ? "stencil" : "depth", sceGpaStrError(err)
		);
		return GNM_ERROR_INTERNAL_FAILURE;
	}
	return GNM_ERROR_OK;
}

static GnmError drt_calc_plane_byte_size(
    uint64_t* outsize, uint32_t* outalignment, const GnmDepthRenderTarget* drt,
    bool stencil
) {
	GpaSurfaceInfo surfinfo = {0};
	GnmError err = drt_compute_plane_surface_info(&surfinfo, drt, stencil);
	if (err != GNM_ERROR_OK) {
		return err;
	}

	if (outsize) {
		*outsize = surfinfo.surfacesize * sceGnmDrtGetNumSlices(drt);
	}
	if (outalignment) {
		*outalignment = surfinfo.basealign;
	}
	return GNM_ERROR_OK;
}

static GnmError inithtile(
    GnmDepthRenderTarget* drt, const GnmDepthRenderTargetCreateInfoFlags flags,
    uint32_t numslices
) {
	const GnmGpuMode mingpumode = sceGnmDrtGetMinGpuMode(drt);

	if (sceGnmGpuMode() == GNM_GPU_NEO && mingpumode == GNM_GPU_BASE) {
		return GNM_ERROR_INVALID_STATE;
	}

	drt->zinfo.tilesurfaceenable = true;

	const GpaHtileParams hparams = {
	    .pitch = sceGnmDrtGetPaddedWidth(drt),
	    .height = sceGnmDrtGetPaddedHeight(drt),
	    .numslices = numslices,
	    .numfrags = sceGnmDrtGetNumFragments(drt),
	    .bpp = 32,
	    .arraymode = drt->depthinfo.arraymode,
	    .banks = drt->depthinfo.numbanks,
	    .pipeconfig = drt->depthinfo.pipeconfig,

	    .mingpumode = mingpumode,
	    .flags =
		{
		    .tccompatible = mingpumode == GNM_GPU_NEO,
		},
	};
	GpaHtileInfo hinfo = {0};
	GpaError gerr = sceGpaComputeHtileInfo(&hinfo, &hparams);
	if (gerr != GPA_ERR_OK) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	const uint32_t numfrags = sceGnmDrtGetNumFragments(drt);

	if (flags.enable_texture_without_decompress &&
	    sceGnmDrtGetMinGpuMode(drt) == GNM_GPU_NEO && numfrags == 1) {
		const GnmTileMode tilemode = drt->zinfo.tilemodeindex;
		if (tilemode == GNM_TM_THIN_1D_THIN) {
			return GNM_ERROR_UNSUPPORTED;
		}

		drt->htilesurface.tccompatible = true;

		const GnmZFormat zfmt = drt->zinfo.format;

		uint16_t unk = 0;
		if (zfmt == GNM_Z_16) {
			unk = 1;
		} else {
			if (numfrags != 1) {
				unk = (numfrags < 8) | 2;
			} else {
				unk = 5;
			}
		}

		drt->zinfo._unused3 = unk;
	}

	return GNM_ERROR_OK;
}

GnmError sceGnmCreateDepthRenderTarget(
    GnmDepthRenderTarget* drt, const GnmDepthRenderTargetCreateInfo* ci
) {
	if (!drt || !ci) {
		return GNM_ERROR_INVALID_ARGS;
	}

	if (ci->zfmt == GNM_Z_INVALID &&
	    ci->stencilfmt == GNM_STENCIL_INVALID) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (ci->width < 1 || ci->width > 16384) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (ci->height < 1 || ci->height > 16384) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (sceGnmGpuMode() == GNM_GPU_NEO && ci->mingpumode == GNM_GPU_BASE &&
	    ci->flags.enable_htile_acceleration) {
		return GNM_ERROR_UNSUPPORTED;
	}
	if (ci->zfmt == GNM_Z_INVALID && ci->flags.enable_htile_acceleration) {
		return GNM_ERROR_UNSUPPORTED;
	}
	if (ci->numfragments != 1 &&
	    ci->flags.enable_texture_without_decompress) {
		return GNM_ERROR_UNSUPPORTED;
	}

	*drt = (GnmDepthRenderTarget){0};

	drt->depthview.slicestart = 0;
	drt->depthview.slicemax = ci->numslices - 1;

	sceGnmDrtSetNumFragments(drt, ci->numfragments);
	drt->zinfo.zrangeprecision = true;

	GnmTileMode actualtilemode =
	    (ci->mingpumode == GNM_GPU_NEO &&
	     ci->flags.enable_htile_acceleration &&
	     ci->flags.enable_texture_without_decompress)
		? GNM_TM_DEPTH_2D_THIN_1K
		: ci->tilemodehint;
	uint32_t actualpitch = ci->width;
	uint32_t actualheight = ci->height;
	if (ci->zfmt != GNM_Z_INVALID) {
		const GnmDataFormat zdf = sceGnmDfInitFromZ(ci->zfmt);
		const uint32_t numtexels = sceGnmDfGetTexelsPerElement(zdf);
		const uint32_t numtotalbits = sceGnmDfGetTotalBitsPerElement(zdf);

		const GpaTilingParams tp = {
		    .tilemode = actualtilemode,
		    .mingpumode = ci->mingpumode,

		    .linearwidth = actualpitch,
		    .linearheight = actualheight,
		    .lineardepth = 1,
		    .numfragsperpixel = ci->numfragments,
		    .basetiledpitch = ci->pitch,

		    .miplevel = 0,
		    .arrayslice = 0,
		    .surfaceflags =
			{
			    .depthtarget = 1,
			    .texcompatible = ci->mingpumode == GNM_GPU_NEO,
			},
		    .bitsperfrag = numtotalbits / numtexels,
		    .isblockcompressed = numtexels > 1,
		};
		GpaSurfaceInfo surfinfo = {0};
		GpaError err = sceGpaComputeSurfaceInfo(&surfinfo, &tp);
		if (err != GPA_ERR_OK) {
			return GNM_ERROR_INTERNAL_FAILURE;
		}

		actualtilemode = surfinfo.tilemode;
		actualpitch = umax(actualpitch, surfinfo.pitch);
		actualheight = umax(actualheight, surfinfo.height);
	}
	if (ci->stencilfmt != GNM_STENCIL_INVALID) {
		const GnmDataFormat sdf = sceGnmDfInitFromStencil(
		    ci->stencilfmt, GNM_IMG_NUM_FORMAT_UINT
		);
		const uint32_t numtexels = sceGnmDfGetTexelsPerElement(sdf);
		const uint32_t numtotalbits = sceGnmDfGetTotalBitsPerElement(sdf);

		const GpaTilingParams tp = {
		    .tilemode = actualtilemode,
		    .mingpumode = ci->mingpumode,

		    .linearwidth = actualpitch,
		    .linearheight = actualheight,
		    .lineardepth = 1,
		    .numfragsperpixel = ci->numfragments,
		    .basetiledpitch = ci->pitch,

		    .miplevel = 0,
		    .arrayslice = 0,
		    .surfaceflags =
			{
			    .stenciltarget = 1,
			    .texcompatible = ci->mingpumode == GNM_GPU_NEO,
			},
		    .bitsperfrag = numtotalbits / numtexels,
		    .isblockcompressed = numtexels > 1,

		};
		GpaSurfaceInfo surfinfo = {0};
		GpaError err = sceGpaComputeSurfaceInfo(&surfinfo, &tp);
		if (err != GPA_ERR_OK) {
			return GNM_ERROR_INTERNAL_FAILURE;
		}

		actualtilemode = surfinfo.tilemode;
		actualpitch = umax(actualpitch, surfinfo.pitch);
		actualheight = umax(actualheight, surfinfo.height);
	}

	drt->zinfo.format = ci->zfmt;
	drt->stencilinfo.format = ci->stencilfmt;
	drt->stencilinfo.tilestencildisable = true;

	sceGnmDrtSetWidth(drt, ci->width);
	sceGnmDrtSetHeight(drt, ci->height);

	GnmError gerr = sceGnmDrtSetPaddedWidth(drt, actualpitch);
	if (gerr != GNM_ERROR_OK) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}
	gerr = sceGnmDrtSetPaddedHeight(drt, actualheight);
	if (gerr != GNM_ERROR_OK) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}
	sceGnmDrtSetSliceSize(drt, actualpitch, actualheight);

	drt->depthinfo.pipeconfig = ci->mingpumode == GNM_GPU_NEO
					? GNM_ADDR_SURF_P16_32x32_8x16
					: GNM_ADDR_SURF_P8_32x32_8x16;

	gerr = sceGnmDrtSetTileMode(drt, actualtilemode);
	if (gerr != GNM_ERROR_OK) {
		return gerr;
	}

	if (ci->flags.enable_htile_acceleration) {
		gerr = inithtile(drt, ci->flags, ci->numslices);
		if (gerr != GNM_ERROR_OK) {
			return gerr;
		}
	}

	return GNM_ERROR_OK;
}

GnmError sceGnmDrtCalcByteSize(
    uint64_t* outsize, uint32_t* outalignment, const GnmDepthRenderTarget* drt
) {
	if (!drt) {
		return GNM_ERROR_INVALID_ARGS;
	}

	if (!drt_has_depth(drt) && !drt_has_stencil(drt)) {
		return GNM_ERROR_INVALID_STATE;
	}

	uint64_t zsize = 0;
	uint64_t stencilsize = 0;
	uint32_t zalign = 1;
	uint32_t stencilalign = 1;
	GnmError err = GNM_ERROR_OK;
	if (drt_has_depth(drt)) {
		err = drt_calc_plane_byte_size(&zsize, &zalign, drt, false);
		if (err != GNM_ERROR_OK) {
			return err;
		}
	}
	if (drt_has_stencil(drt)) {
		err = drt_calc_plane_byte_size(
		    &stencilsize, &stencilalign, drt, true
		);
		if (err != GNM_ERROR_OK) {
			return err;
		}
	}

	const uint64_t stenciloffset =
	    drt_has_stencil(drt) ? alignu64(zsize, stencilalign) : 0;
	if (outsize) {
		*outsize = drt_has_stencil(drt) ? stenciloffset + stencilsize
						: zsize;
	}
	if (outalignment) {
		*outalignment = umax(zalign, stencilalign);
	}
	return GNM_ERROR_OK;
}

GnmError sceGnmDrtCalcStencilByteOffset(
    uint64_t* outoffset, const GnmDepthRenderTarget* drt
) {
	if (!outoffset || !drt) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!drt_has_stencil(drt)) {
		return GNM_ERROR_INVALID_STATE;
	}

	uint64_t zsize = 0;
	uint32_t stencilalign = 1;
	GnmError err = GNM_ERROR_OK;
	if (drt_has_depth(drt)) {
		err = drt_calc_plane_byte_size(&zsize, NULL, drt, false);
		if (err != GNM_ERROR_OK) {
			return err;
		}
	}
	err = drt_calc_plane_byte_size(NULL, &stencilalign, drt, true);
	if (err != GNM_ERROR_OK) {
		return err;
	}

	*outoffset = alignu64(zsize, stencilalign);
	return GNM_ERROR_OK;
}

GnmError sceGnmDrtSetTileMode(GnmDepthRenderTarget* drt, GnmTileMode mode) {
	if (!drt || mode > GNM_TM_DEPTH_2D_THIN_PRT_1K) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (drt->depthinfo.pipeconfig < GNM_ADDR_SURF_P8_32x32_8x16 ||
	    drt->depthinfo.pipeconfig > GNM_ADDR_SURF_P16_32x32_8x16) {
		return GNM_ERROR_INVALID_STATE;
	}

	const GnmGpuMode gpumode = sceGnmDrtGetMinGpuMode(drt);
	const uint32_t numsamples = sceGnmDrtGetNumFragments(drt);

	GpaTileInfo depthtileinfo = {0};
	GpaTileInfo stenciltileinfo = {0};
	GpaTileInfo* primarytileinfo = NULL;
	if (drt_has_depth(drt)) {
		const GnmDataFormat fmt = drt_plane_format(drt, false);
		const uint32_t bitsperelem = sceGnmDfGetBitsPerElement(fmt);
		GpaError err = sceGpaGetTileInfo(
		    &depthtileinfo, mode, bitsperelem, numsamples, gpumode
		);
		if (err != GPA_ERR_OK) {
			return GNM_ERROR_INTERNAL_FAILURE;
		}
		primarytileinfo = &depthtileinfo;
	}
	if (drt_has_stencil(drt)) {
		const GnmDataFormat fmt = drt_plane_format(drt, true);
		const uint32_t bitsperelem = sceGnmDfGetBitsPerElement(fmt);
		GpaError err = sceGpaGetTileInfo(
		    &stenciltileinfo, mode, bitsperelem, numsamples, gpumode
		);
		if (err != GPA_ERR_OK) {
			return GNM_ERROR_INTERNAL_FAILURE;
		}
		if (!primarytileinfo) {
			primarytileinfo = &stenciltileinfo;
		}
	}
	if (!primarytileinfo) {
		return GNM_ERROR_INVALID_STATE;
	}

	drt->zinfo.tilemodeindex = mode;
	drt->zinfo.tilesplit = depthtileinfo.tilesplit;

	drt->stencilinfo.tilemodeindex = mode;
	drt->stencilinfo.tilesplit = stenciltileinfo.tilesplit;

	drt->depthinfo.arraymode = primarytileinfo->arraymode;
	drt->depthinfo.pipeconfig = primarytileinfo->pipeconfig;
	drt->depthinfo.bankwidth = primarytileinfo->bankwidth;
	drt->depthinfo.bankheight = primarytileinfo->bankheight;
	drt->depthinfo.macrotileaspect = primarytileinfo->macroaspectratio;
	drt->depthinfo.numbanks = primarytileinfo->banks;
	return GNM_ERROR_OK;
}

static inline GpaError getswizzlemask(
    uint32_t* outmask, const GnmDepthRenderTarget* drt, bool stencil
) {
	const GnmTileMode tm = stencil ? drt->stencilinfo.tilemodeindex
				       : drt->zinfo.tilemodeindex;
	const GnmDataFormat dfmt = drt_plane_format(drt, stencil);
	const GnmGpuMode mingpumode = sceGnmDrtGetMinGpuMode(drt);
	const uint32_t bitsperelem = sceGnmDfGetBitsPerElement(dfmt);
	const uint32_t numfrags = sceGnmDrtGetNumFragments(drt);

	return sceGpaComputeBaseSwizzle(
	    outmask, tm, 0, bitsperelem, numfrags, mingpumode
	);
}

void* sceGnmDrtGetZReadAddress(const GnmDepthRenderTarget* drt) {
	if (!drt_has_depth(drt)) {
		return NULL;
	}

	uint32_t addr256 = drt->zreadbase256b;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, false);
	if (err == GPA_ERR_OK) {
		// remove the swizzle bits from the address
		addr256 &= ~swizzlemask;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	return (void*)((uintptr_t)addr256 << 8);
}

GnmError sceGnmDrtSetZReadAddress(GnmDepthRenderTarget* drt, void* baseaddr) {
	if (!drt) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!drt_has_depth(drt)) {
		return GNM_ERROR_INVALID_STATE;
	}
	if ((uintptr_t)baseaddr & 0xff) {
		return GNM_ERROR_INVALID_ALIGNMENT;
	}

	uint32_t newaddr = (uintptr_t)baseaddr >> 8;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, false);
	if (err == GPA_ERR_OK) {
		newaddr = (newaddr & ~swizzlemask) |
			  (drt->zreadbase256b & swizzlemask);
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	drt->zreadbase256b = newaddr;
	return GNM_ERROR_OK;
}

void* sceGnmDrtGetStencilReadAddress(const GnmDepthRenderTarget* drt) {
	if (!drt_has_stencil(drt)) {
		return NULL;
	}

	uint32_t addr256 = drt->stencilreadbase256b;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, true);
	if (err == GPA_ERR_OK) {
		// remove the swizzle bits from the address
		addr256 &= ~swizzlemask;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	return (void*)((uintptr_t)addr256 << 8);
}

GnmError sceGnmDrtSetStencilReadAddress(
    GnmDepthRenderTarget* drt, void* baseaddr
) {
	if (!drt) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!drt_has_stencil(drt)) {
		return GNM_ERROR_INVALID_STATE;
	}
	if ((uintptr_t)baseaddr & 0xff) {
		return GNM_ERROR_INVALID_ALIGNMENT;
	}

	uint32_t newaddr = (uintptr_t)baseaddr >> 8;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, true);
	if (err == GPA_ERR_OK) {
		newaddr = (newaddr & ~swizzlemask) |
			  (drt->stencilreadbase256b & swizzlemask);
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	drt->stencilreadbase256b = newaddr;
	return GNM_ERROR_OK;
}

void* sceGnmDrtGetZWriteAddress(const GnmDepthRenderTarget* drt) {
	if (!drt_has_depth(drt)) {
		return NULL;
	}

	uint32_t addr256 = drt->zwritebase256b;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, false);
	if (err == GPA_ERR_OK) {
		// remove the swizzle bits from the address
		addr256 &= ~swizzlemask;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	return (void*)((uintptr_t)addr256 << 8);
}

GnmError sceGnmDrtSetZWriteAddress(GnmDepthRenderTarget* drt, void* baseaddr) {
	if (!drt) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!drt_has_depth(drt)) {
		return GNM_ERROR_INVALID_STATE;
	}
	if ((uintptr_t)baseaddr & 0xff) {
		return GNM_ERROR_INVALID_ALIGNMENT;
	}

	uint32_t newaddr = (uintptr_t)baseaddr >> 8;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, false);
	if (err == GPA_ERR_OK) {
		newaddr = (newaddr & ~swizzlemask) |
			  (drt->zwritebase256b & swizzlemask);
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	drt->zwritebase256b = newaddr;
	return GNM_ERROR_OK;
}

void* sceGnmDrtGetStencilWriteAddress(const GnmDepthRenderTarget* drt) {
	if (!drt_has_stencil(drt)) {
		return NULL;
	}

	uint32_t addr256 = drt->stencilwritebase256b;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, true);
	if (err == GPA_ERR_OK) {
		// remove the swizzle bits from the address
		addr256 &= ~swizzlemask;
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	return (void*)((uintptr_t)addr256 << 8);
}

GnmError sceGnmDrtSetStencilWriteAddress(
    GnmDepthRenderTarget* drt, void* baseaddr
) {
	if (!drt) {
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!drt_has_stencil(drt)) {
		return GNM_ERROR_INVALID_STATE;
	}
	if ((uintptr_t)baseaddr & 0xff) {
		return GNM_ERROR_INVALID_ALIGNMENT;
	}

	uint32_t newaddr = (uintptr_t)baseaddr >> 8;

	uint32_t swizzlemask = 0;
	GpaError err = getswizzlemask(&swizzlemask, drt, true);
	if (err == GPA_ERR_OK) {
		newaddr = (newaddr & ~swizzlemask) |
			  (drt->stencilwritebase256b & swizzlemask);
	} else {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_WARN, "Drt: failed to get swizzle mask with %s",
		    sceGpaStrError(err)
		);
	}

	drt->stencilwritebase256b = newaddr;
	return GNM_ERROR_OK;
}
