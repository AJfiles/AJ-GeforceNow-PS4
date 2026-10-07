// Compile-time and runtime verification of the libSceVideodec (v1) ABI.
//
// The static_asserts inside videodec_abi.hpp are the compile-time test: a layout
// mistake fails the normal build instead of surfacing on the console as
// ORBIS_VIDEODEC_ERROR_STRUCT_SIZE at runtime. This translation unit also exports
// the runtime probes the boot inventory calls.

#include "opennow/stream/videodec_abi.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using namespace opennow::videodec;

constexpr bool AbiLayoutIsCorrect() {
    // Measured with the compiler size probe; see the header for the table.
    static_assert(sizeof(OrbisVideodecConfigInfo)   == 40);
    static_assert(sizeof(OrbisVideodecResourceInfo) == 56);
    static_assert(sizeof(OrbisVideodecCtrl)         == 24);
    static_assert(sizeof(OrbisVideodecFrameBuffer)  == 24);
    static_assert(sizeof(OrbisVideodecInputData)    == 48);
    static_assert(sizeof(OrbisVideodecPictureInfo)  == 112);
    static_assert(sizeof(OrbisVideodecCodecInfo)    == 64);
    return true;
}

// odr-use the entry points so the linker verifies libSceVideodec exports them.
void* const kCreate = reinterpret_cast<void*>(&sceVideodecCreateDecoder);
void* const kDecode = reinterpret_cast<void*>(&sceVideodecDecode);
void* const kQuery  = reinterpret_cast<void*>(&sceVideodecQueryResourceInfo);
void* const kDelete = reinterpret_cast<void*>(&sceVideodecDeleteDecoder);

// codecType candidates, tried in one console run.
//
// Measured on console with codecType=1: rc=0x80C10001 (ORBIS_VIDEODEC_ERROR_CODEC_TYPE).
// It was NOT 0x80C10002 (STRUCT_SIZE), so the struct layouts measured in
// videodec_abi.hpp are CORRECT - the call passed the size check and rejected only the
// codec type. The ABI is right; the constant is not.
//
// The correct value for the v1 HARDWARE API is not documented in any available
// source: shadPS4 implements only vdecsw and videodec2, both of which use 1 for AVC,
// and the hardware v1 rejects exactly that. Rather than guess build after build, try
// every plausible value once and report each result.
const uint32_t kCodecTypeCandidates[] = {
    1u,         // what vdecsw / videodec2 use for AVC
    0u,         // "unspecified"
    2u, 3u, 4u, // HEVC-style numbering used by some Sony media APIs
    0x31435641u,// 'AVC1' FourCC
    0x68323634u,// 'h264' as a big-endian FourCC
};
const char* const kCodecTypeLabels[] = {
    "1(vdecsw-style)", "0(unspecified)", "2", "3", "4", "'AVC1'", "'h264'",
};
constexpr std::size_t kCodecTypeCandidateCount =
    sizeof(kCodecTypeCandidates) / sizeof(kCodecTypeCandidates[0]);

static_assert(kCodecTypeCandidateCount ==
                  sizeof(kCodecTypeLabels) / sizeof(kCodecTypeLabels[0]),
              "candidate and label arrays must match");

} // namespace

extern "C" int opennow_videodec_abi_verified() {
    return AbiLayoutIsCorrect() && kCreate && kDecode && kQuery && kDelete ? 1 : 0;
}

// Single-shot ABI probe with codecType=1: reports whether the layouts are accepted.
extern "C" int32_t opennow_videodec_abi_probe() {
    OrbisVideodecConfigInfo cfg{};
    cfg.thisSize = sizeof(OrbisVideodecConfigInfo);
    cfg.codecType = ORBIS_VIDEODEC_CODEC_TYPE_AVC;
    cfg.maxLevel = 42; // H.264 level 4.2, enough for 1080p60
    cfg.maxFrameWidth = 1920;
    cfg.maxFrameHeight = 1080;
    cfg.maxDpbFrameCount = 0; // let the library choose

    OrbisVideodecResourceInfo res{};
    res.thisSize = sizeof(OrbisVideodecResourceInfo);

    return sceVideodecQueryResourceInfo(&cfg, &res);
}

// Tries every codecType candidate. Writes each rc into out_rc[i] and the matching
// label into out_labels[i]; returns the number of candidates.
extern "C" int opennow_videodec_codec_type_probe(int32_t* out_rc, const char** out_labels) {
    if (!out_rc || !out_labels) return 0;
    for (std::size_t i = 0; i < kCodecTypeCandidateCount; ++i) {
        OrbisVideodecConfigInfo cfg{};
        cfg.thisSize = sizeof(OrbisVideodecConfigInfo);
        cfg.codecType = kCodecTypeCandidates[i];
        cfg.maxLevel = 42;
        cfg.maxFrameWidth = 1920;
        cfg.maxFrameHeight = 1080;
        cfg.maxDpbFrameCount = 0;

        OrbisVideodecResourceInfo res{};
        res.thisSize = sizeof(OrbisVideodecResourceInfo);

        out_rc[i] = sceVideodecQueryResourceInfo(&cfg, &res);
        out_labels[i] = kCodecTypeLabels[i];
    }
    return static_cast<int>(kCodecTypeCandidateCount);
}
