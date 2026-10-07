#include "gnm_rendertarget.h"

#include <stdlib.h>

#include "gpuaddr.h"
#include "platform.h"

static bool init_rt_cmask(GnmRenderTarget* rt, uint32_t numslices) {
	const GnmDataFormat datafmt = sceGnmRtGetFormat(rt);
	const uint32_t bitsperelem = sceGnmDfGetBitsPerElement(datafmt);
	const GnmGpuMode mingpumode =
	    rt->info.alt_tile_mode ? GNM_GPU_NEO : GNM_GPU_BASE;
	const GnmTileMode tm = rt->attrib.tilemode_index;

	rt->info.compression = 1;

	const GpaCmaskParams cparams = {
	    .pitch = sceGnmRtGetPitch(rt),
	    .height = sceGnmRtGetSliceSize(rt),
	    .numslices = numslices,
	    .numfrags = sceGnmRtGetNumFragments(rt),
	    .bpp = bitsperelem,
	    .tilemode = tm,

	    .mingpumode = mingpumode,
	    .flags =
		{
		    .tccompatible = mingpumode == GNM_GPU_NEO,
		},
	};
	GpaCmaskInfo cinfo = {0};
	GpaError err = sceGpaComputeCmaskInfo(&cinfo, &cparams);
	if (err != GPA_ERR_OK) {
		return false;
	}

	rt->cmask_slice.tilemax = (cinfo.cmaskbytes / numslices) / 128 - 1;

	return true;
}

static bool init_rt_fmask(GnmRenderTarget* rt) {
	if (!rt->info.compression) {
		return false;
	}

	const GnmGpuMode curgpumode = sceGnmGpuMode();
	if (curgpumode == GNM_GPU_NEO && !rt->info.alt_tile_mode) {
		return false;
	}

	const GnmTileMode tm = rt->attrib.tilemode_index;
	const bool isthin =
	    tm >= GNM_TM_THIN_1D_THIN && tm <= GNM_TM_THIN_3D_THIN_PRT;

	GnmTileMode fmask_tilemode = GNM_TM_THIN_2D_THIN;
	if (isthin == GNM_SURF_THIN_MICRO_TILING) {
		fmask_tilemode = rt->attrib.tilemode_index;
	}
	rt->attrib.fmask_tilemode_index = fmask_tilemode;

	rt->info.is_normalized = 1;  // default; updated in setdataformat based on channel type

	const GnmDataFormat datafmt = sceGnmRtGetFormat(rt);
	const uint32_t bitsperelem = sceGnmDfGetBitsPerElement(datafmt);
	const GnmGpuMode mingpumode =
	    rt->info.alt_tile_mode ? GNM_GPU_NEO : GNM_GPU_BASE;

	rt->info.compression = 1;

	const GpaFmaskParams fparams = {
	    .pitch = sceGnmRtGetPitch(rt),
	    .height = sceGnmRtGetSliceSize(rt),
	    .numslices = sceGnmRtGetNumSlices(rt),
	    .numfrags = sceGnmRtGetNumFragments(rt),
	    .bpp = bitsperelem,
	    .tilemode = fmask_tilemode,

	    .mingpumode = mingpumode,
	    .isblockcompressed = sceGnmDfIsBlockCompressed(datafmt),
	};
	GpaFmaskInfo finfo = {0};
	GpaError err = sceGpaComputeFmaskInfo(&finfo, &fparams);
	if (err != GPA_ERR_OK) {
		return false;
	}

	// setFmaskPitchDiv8Minus1
	rt->pitch.fmask_tilemax = (finfo.pitch / 8) - 1;
	// setFmaskSliceNumTilesMinus1
	rt->fmask_slice.tilemax = ((finfo.pitch * finfo.height) / 64) - 1;

	return true;
}

GnmError sceGnmCreateRenderTarget(
    GnmRenderTarget* rt, const GnmRenderTargetCreateInfo* ci
) {
	if (!rt) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "rt is null");
		return GNM_ERROR_INVALID_ARGS;
	}
	if (!ci) {
		sceGnmWriteMsg(GNM_MSGSEV_ERR, "spec is null");
		return GNM_ERROR_INVALID_ARGS;
	}
	if ((uint32_t)ci->numsamples < (uint32_t)ci->numfragments) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR,
		    "numSamples must be equal or larger than numFragments"
		);
		return GNM_ERROR_INVALID_ARGS;
	}
	if (ci->colorfmt.surfacefmt > GNM_IMG_DATA_FORMAT_X24_8_32) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "colorfmt 0x08%x is invalid", ci->colorfmt
		);
		return GNM_ERROR_INVALID_ARGS;
	}
	if (ci->colorfmt.surfacefmt == GNM_IMG_DATA_FORMAT_10_10_10_2 ||
	    ci->colorfmt.surfacefmt == GNM_IMG_DATA_FORMAT_2_10_10_10) {
		if (ci->colorfmt.chantype == GNM_IMG_NUM_FORMAT_SRGB) {
			sceGnmWriteMsgf(
			    GNM_MSGSEV_ERR, "colorfmt 0x08%x is invalid",
			    ci->colorfmt
			);
			return GNM_ERROR_INVALID_ARGS;
		}
	} else if (ci->colorfmt.surfacefmt == GNM_IMG_DATA_FORMAT_32_32_32 || ci->colorfmt.surfacefmt == 15) {
		// Format 0xF is reserved/undefined (gap between 32_32_32_32 and 5_6_5)
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "colorfmt 0x08%x is invalid", ci->colorfmt
		);
		return GNM_ERROR_INVALID_ARGS;
	}

	GnmSurfaceNumber rtchantype = 0;
	GnmSurfaceSwap rtchorder = 0;
	if (!sceGnmDfGetRtChannelType(ci->colorfmt, &rtchantype) ||
	    !sceGnmDfGetRtChannelOrder(ci->colorfmt, &rtchorder)) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "colorfmt 0x08%x is invalid", ci->colorfmt
		);
		return GNM_ERROR_INVALID_ARGS;
	}

	if (ci->width - 1 > 16383) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "width must be between 1 and 16384"
		);
		return GNM_ERROR_INVALID_ARGS;
	}
	if (ci->height - 1 > 16383) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "height must be between 1 and 16384"
		);
		return GNM_ERROR_INVALID_ARGS;
	}
	if (ci->pitch > 16384) {
		sceGnmWriteMsg(
		    GNM_MSGSEV_ERR, "pitch must be between 0 and 16384"
		);
		return GNM_ERROR_INVALID_ARGS;
	}

	const GnmGpuMode curgpumode = sceGnmGpuMode();
	if (curgpumode == GNM_GPU_NEO && ci->mingpumode == GNM_GPU_BASE) {
		if (ci->flags.enable_cmask_fastclear) {
			sceGnmWriteMsg(
			    GNM_MSGSEV_ERR,
			    "In NEO mode, enable_cmask_fastclear must be 0 if "
			    "mingpumode is BASE"
			);
			return GNM_ERROR_INVALID_ARGS;
		} else if (ci->flags.enable_fmask_compression) {
			sceGnmWriteMsg(
			    GNM_MSGSEV_ERR,
			    "In NEO mode, enable_fmask_compression must be 0 "
			    "if mingpumode is BASE"
			);
			return GNM_ERROR_INVALID_ARGS;
		} else if (ci->flags.enable_dcc_compression) {
			sceGnmWriteMsg(
			    GNM_MSGSEV_ERR,
			    "In NEO mode, enable_dcc_compression must be 0 if "
			    "mingpumode is BASE"
			);
			return GNM_ERROR_INVALID_ARGS;
		}
	}

	if (ci->mingpumode == GNM_GPU_NEO) {
		rt->dcc_control.min_compressed_blocksize = 1;
	}

	const GnmTileMode tm = rt->attrib.tilemode_index;
	const bool isthick =
	    tm >= GNM_TM_THICK_1D_THICK && tm <= GNM_TM_THICK_3D_XTHICK;

	uint32_t actualw = ci->width;
	uint32_t actualh = ci->height;
	uint32_t actualdepth = 1;
	if (ci->flags.enable_cmask_fastclear) {
		actualw = (actualw + 7) & (-8);
		actualh = (actualh + 7) & (-8);
	}
	if (isthick) {
		actualdepth = ci->numslices;
	}

	const uint32_t numtexels = sceGnmDfGetTexelsPerElement(ci->colorfmt);
	const uint32_t numtotalbits = sceGnmDfGetTotalBitsPerElement(ci->colorfmt);

	GpaSurfaceInfo surfinfo = {0};
	const GpaTilingParams tp = {
	    .tilemode = ci->colortilemodehint,
	    .mingpumode = ci->mingpumode,

	    .linearwidth = actualw,
	    .linearheight = actualh,
	    .lineardepth = actualdepth,
	    .numfragsperpixel = 1 << ci->numfragments,
	    .basetiledpitch = ci->pitch,

	    .miplevel = 0,
	    .arrayslice = 0,
	    .surfaceflags =
		{
		    .fmask = isthick,
		    .texcompatible = curgpumode == GNM_GPU_NEO,
		},
	    .bitsperfrag = numtotalbits / numtexels,
	    .isblockcompressed = numtexels > 1,
	};
	GpaError err = sceGpaComputeSurfaceInfo(&surfinfo, &tp);
	if (err != GPA_ERR_OK) {
		sceGnmWriteMsgf(
		    GNM_MSGSEV_ERR, "gpa_computesurfaceinfo failed with: %s",
		    sceGpaStrError(err)
		);
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	// setpitchdiv8minus1
	rt->pitch.tilemax = surfinfo.pitch / 8 - 1;
	// setFmaskPitchDiv8Minus1
	rt->pitch.fmask_tilemax = surfinfo.pitch / 8 - 1;

	// setSliceSizeDiv64Minus1
	rt->slice.tilemax = (surfinfo.pitch * surfinfo.height) / 64 - 1;

	// setarrayview
	rt->view.slicestart = 0;
	rt->view.slicemax = ci->numslices - 1;

	// setdataformat
	rt->info.format = ci->colorfmt.surfacefmt;
	rt->info.channeltype = rtchantype;
	rt->info.channelorder = rtchorder;
	rt->info.is_normalized = rtchantype == GNM_NUMBER_UNORM ||
				 rtchantype == GNM_NUMBER_SNORM ||
				 rtchantype == GNM_NUMBER_SRGB;
	// CB_COLOR_INFO.SIMPLE_FLOAT is required for normal color-target export
	// conversion and blending on GFX6-GFX8. Mesa enables it for every color
	// target, independently of the surface number type.
	rt->info.simple_float = 1;
	rt->info.is_int =
	    rtchantype == GNM_NUMBER_UINT || rtchantype == GNM_NUMBER_SINT;
	rt->info.is_scaled = rtchantype == GNM_NUMBER_UINT ||
			     rtchantype == GNM_NUMBER_SINT ||
			     rtchantype == GNM_NUMBER_FLOAT;
	// setUseAltTileMode
	rt->info.alt_tile_mode = ci->mingpumode == GNM_GPU_NEO;

	// setnumsamples
	rt->attrib.num_samples = ci->numsamples >> 1;
	// setnumfragments
	rt->attrib.num_fragments = ci->numfragments >> 1;
	// setTileMode
	rt->attrib.tilemode_index = surfinfo.tilemode;
	// setFmaskTileMode
	rt->attrib.fmask_tilemode_index = surfinfo.tilemode;

	// setWidth
	rt->size.width = ci->width;
	// setHeight
	rt->size.height = ci->height;

	if (ci->flags.enable_cmask_fastclear) {
		if (!init_rt_cmask(rt, ci->numslices)) {
			sceGnmWriteMsg(GNM_MSGSEV_ERR, "init_rt_cmask failed");
			return GNM_ERROR_INTERNAL_FAILURE;
		}
	}
	if (ci->flags.enable_fmask_compression && ci->numsamples > 0) {
		if (!init_rt_fmask(rt)) {
			sceGnmWriteMsg(GNM_MSGSEV_ERR, "init_rt_fmask failed");
			return GNM_ERROR_INTERNAL_FAILURE;
		}
	}

	if (curgpumode == GNM_GPU_NEO && ci->flags.enable_dcc_compression) {
		// setDccCompressionEnable
		rt->info.dcc_enable = 1;

		const uint32_t bitsperelem =
		    sceGnmDfGetBitsPerElement(ci->colorfmt);

		uint8_t maxuncompbs = 2 - (bitsperelem == 16);
		uint8_t maxcompbs = 0;
		if (bitsperelem == 8) {
			maxuncompbs = 0;
		}
		if (!ci->flags.enable_colortexture_without_decompress) {
			maxcompbs = maxuncompbs;
		}

		// setDccMaxUncompressedBlockSize
		rt->dcc_control.max_uncompressed_blocksize = maxuncompbs;
		// setDccMaxCompressedBlockSize
		rt->dcc_control.max_compressed_blocksize = maxcompbs;
		// setDccForceIndependentBlocks
		rt->dcc_control.independent_64b_blocks =
		    ci->flags.enable_colortexture_without_decompress;
	}

	return GNM_ERROR_OK;
}

GnmError sceGnmRtCalcByteSize(
    uint64_t* outsize, uint32_t* outalign, const GnmRenderTarget* rt
) {
	const GnmTileMode tm = rt->attrib.tilemode_index;
	const bool isdisplay =
	    tm == GNM_TM_DISPLAY_LINEAR_GENERAL ||
	    tm == GNM_TM_DISPLAY_LINEAR_ALIGNED ||
	    tm == GNM_TM_DISPLAY_1D_THIN || tm == GNM_TM_DISPLAY_2D_THIN ||
	    tm == GNM_TM_DISPLAY_THIN_PRT || tm == GNM_TM_DISPLAY_2D_THIN_PRT;

	const GnmDataFormat datafmt = sceGnmRtGetFormat(rt);
	const uint32_t numtexels = sceGnmDfGetTexelsPerElement(datafmt);
	const uint32_t numtotalbits = sceGnmDfGetTotalBitsPerElement(datafmt);

	const GnmGpuMode mingpumode =
	    rt->info.alt_tile_mode ? GNM_GPU_NEO : GNM_GPU_BASE;

	GpaTilingParams tp = {
	    .tilemode = rt->attrib.tilemode_index,
	    .mingpumode = mingpumode,

	    .linearwidth = sceGnmRtGetPitch(rt),
	    .linearheight = sceGnmRtGetSliceSize(rt),
	    .lineardepth = 1,
	    .numfragsperpixel = sceGnmRtGetNumFragments(rt),
	    .basetiledpitch = (rt->pitch.tilemax + 1) * 8,

	    .miplevel = 0,
	    .arrayslice = 0,
	    .surfaceflags =
		{
		    .display = isdisplay,
		},
	    .bitsperfrag = numtotalbits / numtexels,
	    .isblockcompressed = numtexels > 1,
	};
	if (mingpumode == GNM_GPU_NEO) {
		if (rt->info.dcc_enable &&
		    (!rt->dcc_control.independent_64b_blocks ||
		     rt->dcc_control.max_uncompressed_blocksize > GNM_DCC_MBS_64
		    )) {
			tp.surfaceflags.texcompatible = false;
		}
	}

	GpaSurfaceInfo surfinfo = {0};
	GpaError err = sceGpaComputeSurfaceInfo(&surfinfo, &tp);
	if (err != GPA_ERR_OK) {
		return GNM_ERROR_INTERNAL_FAILURE;
	}

	const uint64_t slicesize =
	    surfinfo.surfacesize * (rt->view.slicemax + 1);

	if (outsize) {
		*outsize = slicesize;
	}
	if (outalign) {
		*outalign = surfinfo.basealign;
	}
	return GNM_ERROR_OK;
}

GnmDataFormat sceGnmRtGetFormat(const GnmRenderTarget* rt) {
	GnmImgNumFormat chantype = 0;
	switch (rt->info.channeltype) {
	case GNM_NUMBER_UNORM:
		chantype = GNM_IMG_NUM_FORMAT_UNORM;
		break;
	case GNM_NUMBER_SNORM:
		chantype = GNM_IMG_NUM_FORMAT_SNORM;
		break;
	case GNM_NUMBER_UINT:
		chantype = GNM_IMG_NUM_FORMAT_UINT;
		break;
	case GNM_NUMBER_SINT:
		chantype = GNM_IMG_NUM_FORMAT_SINT;
		break;
	case GNM_NUMBER_SRGB:
		chantype = GNM_IMG_NUM_FORMAT_SRGB;
		break;
	case GNM_NUMBER_FLOAT:
		chantype = GNM_IMG_NUM_FORMAT_FLOAT;
		break;
	default:
		abort();
	}

	GnmDataFormat res = {
	    .surfacefmt = (GnmImageFormat)rt->info.format,
	    .chantype = chantype,
	};

	const uint32_t numcomps = sceGnmDfGetNumComponents(res);
	if (numcomps > 4) {
		abort();
	}

	switch (rt->info.channelorder) {
	case GNM_SWAP_STD:
		if (numcomps == 1) {
			res.chanx = GNM_CHAN_X;
			res.chany = GNM_CHAN_CONSTANT0;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_CONSTANT0;
		} else if (numcomps == 2) {
			res.chanx = GNM_CHAN_X;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_CONSTANT0;
		} else if (numcomps == 3) {
			res.chanx = GNM_CHAN_X;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_Z;
			res.chanw = GNM_CHAN_CONSTANT0;
		} else if (numcomps == 4) {
			res.chanx = GNM_CHAN_X;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_Z;
			res.chanw = GNM_CHAN_W;
		}
		break;
	case GNM_SWAP_ALT:
		if (numcomps == 1) {
			res.chanx = GNM_CHAN_CONSTANT0;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_CONSTANT0;
		} else if (numcomps == 2) {
			res.chanx = GNM_CHAN_X;
			res.chany = GNM_CHAN_CONSTANT0;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_Y;
		} else if (numcomps == 3) {
			res.chanx = GNM_CHAN_X;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_Z;
		} else if (numcomps == 4) {
			res.chanx = GNM_CHAN_Z;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_X;
			res.chanw = GNM_CHAN_W;
		}
		break;
	case GNM_SWAP_STD_REV:
		if (numcomps == 1) {
			res.chanx = GNM_CHAN_CONSTANT0;
			res.chany = GNM_CHAN_CONSTANT0;
			res.chanz = GNM_CHAN_X;
			res.chanw = GNM_CHAN_CONSTANT0;
		} else if (numcomps == 2) {
			res.chanx = GNM_CHAN_Y;
			res.chany = GNM_CHAN_X;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_CONSTANT0;
		} else if (numcomps == 3) {
			res.chanx = GNM_CHAN_Z;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_X;
			res.chanw = GNM_CHAN_CONSTANT0;
		} else if (numcomps == 4) {
			res.chanx = GNM_CHAN_W;
			res.chany = GNM_CHAN_Z;
			res.chanz = GNM_CHAN_Y;
			res.chanw = GNM_CHAN_X;
		}
		break;
	case GNM_SWAP_ALT_REV:
		if (numcomps == 1) {
			res.chanx = GNM_CHAN_CONSTANT0;
			res.chany = GNM_CHAN_CONSTANT0;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_X;
		} else if (numcomps == 2) {
			res.chanx = GNM_CHAN_Y;
			res.chany = GNM_CHAN_CONSTANT0;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_X;
		} else if (numcomps == 3) {
			res.chanx = GNM_CHAN_Z;
			res.chany = GNM_CHAN_Y;
			res.chanz = GNM_CHAN_CONSTANT0;
			res.chanw = GNM_CHAN_X;
		} else if (numcomps == 4) {
			res.chanx = GNM_CHAN_Y;
			res.chany = GNM_CHAN_Z;
			res.chanz = GNM_CHAN_W;
			res.chanw = GNM_CHAN_X;
		}
		break;
	default:
		abort();
	}

	return res;
}
