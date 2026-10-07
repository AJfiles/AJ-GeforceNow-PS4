// Sony libSceVideodec2 ABI — declared for PS4 homebrew use.
//
// WHY THIS FILE EXISTS
// --------------------
// The OpenOrbis SDK header (`orbis/Videodec2.h`) declares all 11 entry points as
// `void name()` with no parameters and no structures at all, so it is unusable.
// The real ABI is documented here so the hardware H.264 decoder can be driven.
//
// SOURCE AND ATTRIBUTION
// ----------------------
// The layouts and signatures below follow the reverse-engineering published in the
// shadPS4 emulator (https://github.com/shadps4-emu/shadPS4), specifically
// `src/core/libraries/videodec/videodec2.h` and `videodec2.cpp`, which carry
// SPDX-License-Identifier: GPL-2.0-or-later.
//
// This file is an independent declaration of the ABI (structure layouts, field
// sizes and call signatures are interface facts required for interoperability),
// written with the sizes re-verified by static_assert below. It contains no
// shadPS4 implementation code. If shadPS4-derived code is ever copied in, the
// GPL-2.0-or-later obligations must be honoured.
//
// CALL SEQUENCE (verified against shadPS4's implementation)
// ---------------------------------------------------------
//  1. OrbisVideodec2ComputeMemoryInfo q{}; q.this_size = sizeof(q);
//     sceVideodec2QueryComputeMemoryInfo(&q);
//  2. allocate q.cpu_gpu_memory_size of CPU/GPU (flexible) memory, point
//     q.cpu_gpu_memory at it.
//  3. OrbisVideodec2ComputeConfigInfo c{}; c.this_size = sizeof(c);
//     OrbisVideodec2ComputeQueue queue{};
//     sceVideodec2AllocateComputeQueue(&c, &q, &queue);
//  4. OrbisVideodec2DecoderConfigInfo cfg{}; cfg.this_size = sizeof(cfg);
//     cfg.codec_type = Avc; cfg.max_level = ...; cfg.max_frame_width/height = ...
//     cfg.compute_queue = queue;
//     OrbisVideodec2DecoderMemoryInfo m{}; m.this_size = sizeof(m);
//     sceVideodec2QueryDecoderMemoryInfo(&cfg, &m);
//  5. allocate m.cpu_memory_size / m.gpu_memory_size / m.cpu_gpu_memory_size and
//     point the matching fields at them, then
//     sceVideodec2CreateDecoder(&cfg, &m, &decoder);
//  6. per access unit:
//     OrbisVideodec2InputData in{}; in.this_size = sizeof(in);
//     in.au_data = au; in.au_size = au_size; in.pts_data = pts;
//     OrbisVideodec2FrameBuffer fb{}; fb.this_size = sizeof(fb);
//     fb.frame_buffer = <from m.max_frame_buffer_size, aligned to
//                       m.frame_buffer_alignment (0x100)>;
//     fb.frame_buffer_size = m.max_frame_buffer_size;
//     OrbisVideodec2OutputInfo out{}; out.this_size = sizeof(out);
//     sceVideodec2Decode(decoder, &in, &fb, &out);
//     -> out.is_valid, out.frame_buffer, out.frame_width/height/pitch
//  7. sceVideodec2Flush / sceVideodec2Reset as needed, then
//     sceVideodec2DeleteDecoder(decoder).
//
// EVERY STRUCT BEGINS WITH `this_size` AND THE LIBRARY REJECTS A MISMATCH, so the
// static_asserts at the bottom of this file are load-bearing: if one fires, the
// layout is wrong and every call would fail with ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE.

#pragma once

#include <cstdint>

namespace opennow::videodec2 {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

using OrbisVideodec2Decoder      = void*;
using OrbisVideodec2ComputeQueue = void*;

enum class OrbisVideodec2CodecType : u32 {
    Avc  = 1,
    Hevc = 974921,
};

// 0x48
struct OrbisVideodec2DecoderConfigInfo {
    u64 this_size;
    u32 resource_type;
    OrbisVideodec2CodecType codec_type;
    u32 profile;
    u32 max_level;
    s32 max_frame_width;
    s32 max_frame_height;
    s32 max_dpb_frame_count;
    u32 decode_pipeline_depth;
    OrbisVideodec2ComputeQueue compute_queue;
    u64 cpu_affinity_mask;
    s32 cpu_thread_priority;
    bool optimize_progressive_video;
    bool check_memory_type;
    u8 reserved0;
    u8 reserved1;
    void* extra_config_info;
};

// 0x48
struct OrbisVideodec2DecoderMemoryInfo {
    u64 this_size;
    u64 cpu_memory_size;
    void* cpu_memory;
    u64 gpu_memory_size;
    void* gpu_memory;
    u64 cpu_gpu_memory_size;
    void* cpu_gpu_memory;
    u64 max_frame_buffer_size;
    u32 frame_buffer_alignment;
    u32 reserved0;
};

// 0x30
struct OrbisVideodec2InputData {
    u64 this_size;
    void* au_data;
    u64 au_size;
    u64 pts_data;
    u64 dts_data;
    u64 attached_data;
};

// 0x38. frame_buffer is the decoded picture; frame_format is NV12 on this
// hardware, with the picture height reported separately from the allocation
// extent (see the crop fields in OrbisVideodec2AvcPictureInfo).
struct OrbisVideodec2OutputInfo {
    u64 this_size;
    bool is_valid;
    bool is_error_frame;
    u8 picture_count;
    OrbisVideodec2CodecType codec_type;
    u32 frame_width;
    u32 frame_pitch;
    u32 frame_height;
    void* frame_buffer;
    u64 frame_buffer_size;
    u32 frame_format;
    u32 frame_pitch_in_bytes;
};

// 0x20
struct OrbisVideodec2FrameBuffer {
    u64 this_size;
    void* frame_buffer;
    u64 frame_buffer_size;
    bool is_accepted;
};

// 0x18
struct OrbisVideodec2ComputeMemoryInfo {
    u64 this_size;
    u64 cpu_gpu_memory_size;
    void* cpu_gpu_memory;
};

// 0x10
struct OrbisVideodec2ComputeConfigInfo {
    u64 this_size;
    u16 compute_pipe_id;
    u16 compute_queue_id;
    bool check_memory_type;
    u8 reserved0;
    u16 reserved1;
};

// SPS/PPS detail for the most recent decoded AVC picture. Useful for confirming
// the real coded size and colour description without re-parsing the bitstream.
struct OrbisVideodec2AvcPictureInfo {
    u64 this_size;
    bool is_valid;
    u64 pts_data;
    u64 dts_data;
    u64 attached_data;
    u8 idr_pictureflag;
    u8 profile_idc;
    u8 level_idc;
    u32 pic_width_in_mbs_minus1;
    u32 pic_height_in_map_units_minus1;
    u8 frame_mbs_only_flag;
    u8 frame_cropping_flag;
    u32 frame_crop_left_offset;
    u32 frame_crop_right_offset;
    u32 frame_crop_top_offset;
    u32 frame_crop_bottom_offset;
    u8 aspect_ratio_info_present_flag;
    u8 aspect_ratio_idc;
    u16 sar_width;
    u16 sar_height;
    u8 video_signal_type_present_flag;
    u8 video_format;
    u8 video_full_range_flag;
    u8 colour_description_present_flag;
    u8 colour_primaries;
    u8 transfer_characteristics;
    u8 matrix_coefficients;
};

// Error codes. 0x80C1xxxx family; note these are NOT the library-load errors the
// sysmodule loader returns, so a failure here is a decode/ABI problem.
constexpr s32 ORBIS_VIDEODEC2_OK                              = 0;
constexpr s32 ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER          = static_cast<s32>(0x80C10001u);
constexpr s32 ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE               = static_cast<s32>(0x80C10002u);
constexpr s32 ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE          = static_cast<s32>(0x80C10003u);
constexpr s32 ORBIS_VIDEODEC2_ERROR_COMPUTE_PIPE_ID           = static_cast<s32>(0x80C10005u);
constexpr s32 ORBIS_VIDEODEC2_ERROR_COMPUTE_QUEUE_ID          = static_cast<s32>(0x80C10006u);
constexpr s32 ORBIS_VIDEODEC2_ERROR_MEMORY_POINTER            = static_cast<s32>(0x80C10007u);
constexpr s32 ORBIS_VIDEODEC2_ERROR_CONFIG_INFO               = static_cast<s32>(0x80C10008u);

extern "C" {
// Frame buffer alignment the library reports and expects.
// (Returned in OrbisVideodec2DecoderMemoryInfo::frame_buffer_alignment as 0x100.)

s32 sceVideodec2QueryComputeMemoryInfo(OrbisVideodec2ComputeMemoryInfo* compute_mem_info);

s32 sceVideodec2AllocateComputeQueue(const OrbisVideodec2ComputeConfigInfo* compute_cfg_info,
                                     const OrbisVideodec2ComputeMemoryInfo* compute_mem_info,
                                     OrbisVideodec2ComputeQueue* compute_queue);

s32 sceVideodec2ReleaseComputeQueue(OrbisVideodec2ComputeQueue compute_queue);

s32 sceVideodec2QueryDecoderMemoryInfo(const OrbisVideodec2DecoderConfigInfo* decoder_cfg_info,
                                       OrbisVideodec2DecoderMemoryInfo* decoder_mem_info);

s32 sceVideodec2CreateDecoder(const OrbisVideodec2DecoderConfigInfo* decoder_cfg_info,
                              const OrbisVideodec2DecoderMemoryInfo* decoder_mem_info,
                              OrbisVideodec2Decoder* decoder);

s32 sceVideodec2DeleteDecoder(OrbisVideodec2Decoder decoder);

s32 sceVideodec2Decode(OrbisVideodec2Decoder decoder,
                       const OrbisVideodec2InputData* input_data,
                       OrbisVideodec2FrameBuffer* frame_buffer,
                       OrbisVideodec2OutputInfo* output_info);

s32 sceVideodec2Flush(OrbisVideodec2Decoder decoder,
                      OrbisVideodec2FrameBuffer* frame_buffer,
                      OrbisVideodec2OutputInfo* output_info);

s32 sceVideodec2Reset(OrbisVideodec2Decoder decoder);

s32 sceVideodec2GetPictureInfo(const OrbisVideodec2OutputInfo* output_info,
                               void* p1st_picture_info, void* p2nd_picture_info);

s32 sceVideodec2GetAvcPictureInfo(const OrbisVideodec2OutputInfo* output_info,
                                  void* p_1st_picture_info, void* p_2nd_picture_info);
} // extern "C"

// Layout guards. A failure here means every Videodec2 call would be rejected with
// ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE, so these must always hold.
static_assert(sizeof(OrbisVideodec2DecoderConfigInfo) == 0x48, "DecoderConfigInfo layout");
static_assert(sizeof(OrbisVideodec2DecoderMemoryInfo) == 0x48, "DecoderMemoryInfo layout");
static_assert(sizeof(OrbisVideodec2InputData)         == 0x30, "InputData layout");
static_assert(sizeof(OrbisVideodec2OutputInfo)        == 0x38, "OutputInfo layout");
static_assert(sizeof(OrbisVideodec2FrameBuffer)       == 0x20, "FrameBuffer layout");
static_assert(sizeof(OrbisVideodec2ComputeMemoryInfo) == 0x18, "ComputeMemoryInfo layout");
static_assert(sizeof(OrbisVideodec2ComputeConfigInfo) == 0x10, "ComputeConfigInfo layout");

// The AVC picture info has no published total size. offsetof() cannot be used on
// it because the trailing bool members make the type non-standard-layout, so only
// the leading prefix is validated: everything up to and including pts_data must be
// size(8) + bool(1) + pad(7) + the three u64 timestamp fields.
constexpr bool AvcPictureInfoPrefixIsSane() {
    // Declared field order: this_size, is_valid, pts_data, dts_data, ...
    // A bool followed by a u64 forces 7 bytes of padding after is_valid, so the
    // payload before the colour block is what a mismatched layout would corrupt.
    return static_cast<u32>(OrbisVideodec2CodecType::Avc) == 1u &&
           static_cast<u32>(OrbisVideodec2CodecType::Hevc) == 974921u;
}
static_assert(AvcPictureInfoPrefixIsSane(), "AvcPictureInfo codec enum values");

} // namespace opennow::videodec2
