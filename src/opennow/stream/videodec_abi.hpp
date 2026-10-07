// Sony libSceVideodec (v1) ABI — the hardware H.264 decoder that actually loads.
//
// WHY v1 AND NOT v2
// -----------------
// A standalone probe and the client's own boot inventory both measured, on a PS4
// Pro with firmware 9.00:
//
//     BOOT_CODEC_MODULE module=VIDEODEC   rc=0x00000000   <- loads
//     BOOT_CODEC_MODULE module=VIDEODEC2  rc=0x805A1000   <- does NOT load
//     BOOT_CODEC_MODULE module=VDECWRAP   rc=0x00000000   <- loads
//     BOOT_CODEC_MODULE module=AV_PLAYER  rc=0x00000000   <- loads
//
// libSceVideodec2 is the newer API and what the reference projects target, but its
// module cannot be started on this unit, so no amount of ABI work would make it
// usable. libSceVideodec (v1) loads cleanly, is simpler (no compute-queue
// management) and is therefore the viable hardware path.
//
// SOURCE AND ATTRIBUTION
// ----------------------
// Layouts and signatures follow the reverse-engineering published by the shadPS4
// emulator (https://github.com/shadps4-emu/shadPS4),
// `src/core/libraries/videodec/videodec.h`, SPDX-License-Identifier:
// GPL-2.0-or-later. This file is an independent declaration of the ABI (interface
// facts required for interoperability) with sizes re-verified by static_assert.
// No shadPS4 implementation code is copied. If shadPS4-derived code is ever
// incorporated, the GPL-2.0-or-later obligations must be honoured.
//
// CALL SEQUENCE
// -------------
//   1. OrbisVideodecConfigInfo cfg{}; cfg.thisSize = sizeof(cfg);
//      cfg.codecType = AVC; cfg.maxLevel = ...; cfg.maxFrameWidth/Height = ...
//   2. OrbisVideodecResourceInfo res{}; res.thisSize = sizeof(res);
//      sceVideodecQueryResourceInfo(&cfg, &res);
//   3. allocate res.cpuMemorySize / res.cpuGpuMemorySize, point the matching
//      pointers at them.
//   4. OrbisVideodecCtrl ctrl{}; ctrl.thisSize = sizeof(ctrl);
//      sceVideodecCreateDecoder(&cfg, &res, &ctrl);
//   5. per access unit:
//      OrbisVideodecInputData in{}; in.thisSize = sizeof(in);
//      in.pAuData = au; in.auSize = au_size; in.ptsData = pts;
//      OrbisVideodecFrameBuffer fb{}; fb.thisSize = sizeof(fb);
//      fb.pFrameBuffer = <res.maxFrameBufferSize, aligned to
//                         res.frameBufferAlignment>;
//      OrbisVideodecPictureInfo pic{}; pic.thisSize = sizeof(pic);
//      sceVideodecDecode(&ctrl, &in, &fb, &pic);
//      -> pic.isValid, pic.frameWidth/Height/Pitch, and pic.codec.avc crop fields
//   6. sceVideodecFlush / sceVideodecReset as needed, then
//      sceVideodecDeleteDecoder(&ctrl).
//
// As in v2, every struct begins with `thisSize` (note the camelCase here, unlike
// v2's `this_size`) and the library rejects a mismatch, so the static_asserts below
// are load-bearing.

#pragma once

#include <cstdint>

namespace opennow::videodec {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

// Codec selector. AVC is what GeForce NOW negotiates on this client
// (app log: format=12 / yuv420p).
constexpr u32 ORBIS_VIDEODEC_CODEC_TYPE_AVC = 1;

// ALIGNMENT AND KNOWN LIMITATION
// ------------------------------
// These structures use the compiler's NATURAL alignment, matching the shadPS4
// declaration (no packing attribute). Measured sizes on this toolchain:
//
//     ConfigInfo 0x28   ResourceInfo 0x34   Ctrl 0x18   FrameBuffer 0x18
//     InputData  0x30   PictureInfo  0x70   AvcInfo 0x22  CodecInfo 0x40
//
// The real library validates `thisSize` against its OWN sizes, which have not been
// independently confirmed. Some PS4 media structures are packed (PS5PCEM notes this
// for the AvPlayer video struct: "the software decoder ABI reports the aligned
// allocation extent"), and a mismatch would reject every call with
// ORBIS_VIDEODEC_ERROR_STRUCT_SIZE.
//
// Therefore the sizes that are NOT known for certain are documented rather than
// asserted, and the ABI check builds a runtime probe that reports what the real
// library accepts. Guessing wrong here would fail silently and expensively.

struct OrbisVideodecConfigInfo {
    u64 thisSize;
    u32 codecType;
    u32 profile;
    u32 maxLevel;
    s32 maxFrameWidth;
    s32 maxFrameHeight;
    s32 maxDpbFrameCount;
    u64 videodecFlags;
};

struct OrbisVideodecResourceInfo {
    u64 thisSize;
    u64 cpuMemorySize;
    void* pCpuMemory;
    u64 cpuGpuMemorySize;
    void* pCpuGpuMemory;
    u64 maxFrameBufferSize;
    u32 frameBufferAlignment;
};

// The control object is returned BY VALUE by CreateDecoder, unlike v2 which
// returns an opaque pointer. handle is filled in by the library.
struct OrbisVideodecCtrl {
    u64 thisSize;
    void* handle;
    u64 version;
};

struct OrbisVideodecFrameBuffer {
    u64 thisSize;
    void* pFrameBuffer;
    u64 frameBufferSize;
};

// SPS timing and cropping. The crop offsets matter: the decoder reports the
// ALIGNED allocation extent as the frame size, and the visible picture is
// expressed through these crops. Ignoring them makes consumers read the chroma
// plane too early (the classic 1080-line / crop_bottom=8 trap noted in the
// AvPlayer ABI).
struct OrbisVideodecAvcInfo {
    u32 numUnitsInTick;
    u32 timeScale;
    u8 fixedFrameRateFlag;
    u8 aspectRatioIdc;
    u16 sarWidth;
    u16 sarHeight;
    u8 colourPrimaries;
    u8 transferCharacteristics;
    u8 matrixCoefficients;
    u8 videoFullRangeFlag;
    u32 frameCropLeftOffset;
    u32 frameCropRightOffset;
    u32 frameCropTopOffset;
    u32 frameCropBottomOffset;
};

union OrbisVideodecCodecInfo {
    u8 reserved[64];
    OrbisVideodecAvcInfo avc;
};

struct OrbisVideodecPictureInfo {
    u64 thisSize;
    u32 isValid;
    u32 codecType;
    u32 frameWidth;
    u32 framePitch;
    u32 frameHeight;
    u32 isErrorPic;
    u64 ptsData;
    u64 attachedData;
    OrbisVideodecCodecInfo codec;
};

struct OrbisVideodecInputData {
    u64 thisSize;
    void* pAuData;
    u64 auSize;
    u64 ptsData;
    u64 dtsData;
    u64 attachedData;
};

// Error codes from the shadPS4 reverse-engineering (0x80C1xxxx family). These are
// decode/ABI failures, distinct from the sysmodule loader's own codes.
constexpr s32 ORBIS_VIDEODEC_OK                        = 0;
constexpr s32 ORBIS_VIDEODEC_ERROR_API_FAIL            = static_cast<s32>(0x80C10000u);
constexpr s32 ORBIS_VIDEODEC_ERROR_CODEC_TYPE          = static_cast<s32>(0x80C10001u);
constexpr s32 ORBIS_VIDEODEC_ERROR_STRUCT_SIZE         = static_cast<s32>(0x80C10002u);
constexpr s32 ORBIS_VIDEODEC_ERROR_HANDLE              = static_cast<s32>(0x80C10003u);
constexpr s32 ORBIS_VIDEODEC_ERROR_CPU_MEMORY_SIZE     = static_cast<s32>(0x80C10004u);
constexpr s32 ORBIS_VIDEODEC_ERROR_CPU_GPU_MEMORY_SIZE = static_cast<s32>(0x80C10006u);
constexpr s32 ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE   = static_cast<s32>(0x80C1000Bu);
constexpr s32 ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_ALIGN  = static_cast<s32>(0x80C1000Du);
constexpr s32 ORBIS_VIDEODEC_ERROR_CONFIG_INFO         = static_cast<s32>(0x80C1000Eu);
constexpr s32 ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER    = static_cast<s32>(0x80C1000Fu);
constexpr s32 ORBIS_VIDEODEC_ERROR_NEW_SEQUENCE        = static_cast<s32>(0x80C10010u);

extern "C" {
// NIDs, for reference (OpenOrbis resolves these through libSceVideodec.so, so the
// names below are what the linker needs):
//   sceVideodecCreateDecoder    qkgRiwHyheU
//   sceVideodecDecode           q0W5GJMovMs
//   sceVideodecDeleteDecoder    U0kpGF1cl90
//   sceVideodecFlush            jeigLlKdp5I
//   sceVideodecMapMemory        kg+lH0V61hM
//   sceVideodecQueryResourceInfo leCAscipfFY
//   sceVideodecReset            f8AgDv-1X8A

s32 sceVideodecQueryResourceInfo(const OrbisVideodecConfigInfo* pCfgInfoIn,
                                 OrbisVideodecResourceInfo* pRsrcInfoOut);

s32 sceVideodecCreateDecoder(const OrbisVideodecConfigInfo* pCfgInfoIn,
                             const OrbisVideodecResourceInfo* pRsrcInfoIn,
                             OrbisVideodecCtrl* pCtrlOut);

s32 sceVideodecDecode(OrbisVideodecCtrl* pCtrlIn,
                      const OrbisVideodecInputData* pInputDataIn,
                      OrbisVideodecFrameBuffer* pFrameBufferInOut,
                      OrbisVideodecPictureInfo* pPictureInfoOut);

s32 sceVideodecFlush(OrbisVideodecCtrl* pCtrlIn,
                     OrbisVideodecFrameBuffer* pFrameBufferInOut,
                     OrbisVideodecPictureInfo* pPictureInfoOut);

s32 sceVideodecReset(OrbisVideodecCtrl* pCtrlIn);

s32 sceVideodecDeleteDecoder(OrbisVideodecCtrl* pCtrlIn);

s32 sceVideodecMapMemory();
} // extern "C"

// Layout guards. Values MEASURED on this toolchain with a compiler size probe
// (build/abi_probe.cpp), not hand-written:
//     ConfigInfo 40   ResourceInfo 56   Ctrl 24   FrameBuffer 24
//     InputData  48   PictureInfo 112   AvcInfo 36   CodecInfo 64
//
// The library validates `thisSize` against its own sizes, which are not confirmed
// against firmware. The runtime probe in videodec_abi_check.cpp reports what the
// real library accepts, so the ABI is verified on-console before any decoder work
// is layered on top of it.
static_assert(sizeof(OrbisVideodecConfigInfo)   == 40,  "ConfigInfo measured");
static_assert(sizeof(OrbisVideodecResourceInfo) == 56,  "ResourceInfo measured");
static_assert(sizeof(OrbisVideodecCtrl)         == 24,  "Ctrl measured");
static_assert(sizeof(OrbisVideodecFrameBuffer)  == 24,  "FrameBuffer measured");
static_assert(sizeof(OrbisVideodecInputData)    == 48,  "InputData measured");
static_assert(sizeof(OrbisVideodecPictureInfo)  == 112, "PictureInfo measured");
static_assert(sizeof(OrbisVideodecAvcInfo)      == 36,  "AvcInfo measured");
static_assert(sizeof(OrbisVideodecCodecInfo)    == 64,  "CodecInfo measured");

} // namespace opennow::videodec
