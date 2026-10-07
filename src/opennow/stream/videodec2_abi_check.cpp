// Compile-time verification of the libSceVideodec2 ABI declarations.
//
// This translation unit only includes the header: the static_asserts inside it are
// the test. It is built by scripts/build-ps4.ps1 alongside the client (it is
// header-only and adds no code), so a layout mistake fails the normal build rather
// than surfacing as ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE on the console at runtime.

#include "opennow/stream/videodec2_abi.hpp"

namespace {

using namespace opennow::videodec2;

constexpr bool AbiLayoutIsCorrect() {
    // Re-assert here as well so the intent is visible from the build script.
    static_assert(sizeof(OrbisVideodec2DecoderConfigInfo) == 0x48);
    static_assert(sizeof(OrbisVideodec2DecoderMemoryInfo) == 0x48);
    static_assert(sizeof(OrbisVideodec2InputData)         == 0x30);
    static_assert(sizeof(OrbisVideodec2OutputInfo)        == 0x38);
    static_assert(sizeof(OrbisVideodec2FrameBuffer)       == 0x20);
    static_assert(sizeof(OrbisVideodec2ComputeMemoryInfo) == 0x18);
    static_assert(sizeof(OrbisVideodec2ComputeConfigInfo) == 0x10);
    static_assert(static_cast<u32>(OrbisVideodec2CodecType::Avc) == 1u);
    return true;
}

// Referenced so the declarations are odr-used and the linker verifies that
// libSceVideodec2 actually exports them when the client links against it.
void* const kDecodeEntryPoint = reinterpret_cast<void*>(&sceVideodec2Decode);
void* const kCreateEntryPoint = reinterpret_cast<void*>(&sceVideodec2CreateDecoder);
void* const kAvcInfoEntryPoint = reinterpret_cast<void*>(&sceVideodec2GetAvcPictureInfo);

} // namespace

// Single definition guard so the compiler cannot elide the checks above.
extern "C" int opennow_videodec2_abi_verified() {
    return AbiLayoutIsCorrect() &&
           kDecodeEntryPoint && kCreateEntryPoint && kAvcInfoEntryPoint ? 1 : 0;
}
