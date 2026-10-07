// Diagnostic for the SSE2 scaled row converter. Isolates WHICH term differs by
// exercising one channel at a time with everything else held at a known value.
//
// The full comparison test reported 400/400 failures with byte=3 simd=0 scalar=255
// (alpha wrong) and byte=1 off-by-one (chroma). Rather than keep reasoning about it,
// this pins down each contribution separately.

namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
} // namespace opennow

#include "../src/opennow/stream/color_simd.cpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using opennow::color::scale_row_bgra_scalar;
using opennow::color::scale_row_bgra_simd;
using opennow::color::STORE_UNALIGNED;

int main() {
    const int src_w = 16, dst_w = 16;

    // Flat grey: Y=128, U=V=128 (neutral chroma). Every output pixel must be identical.
    std::vector<uint8_t> y(src_w * 4, 128);
    std::vector<uint8_t> u((src_w / 2) * 2, 128);
    std::vector<uint8_t> v((src_w / 2) * 2, 128);

    std::vector<int> sx_map(dst_w), sux_map(dst_w);
    for (int x = 0; x < dst_w; ++x) { sx_map[x] = x; sux_map[x] = x / 2; }

    for (int fr = 0; fr < 2; ++fr) {
        const bool full_range = (fr == 1);
        std::vector<uint8_t> simd_out(dst_w * 4, 0xAA);
        std::vector<uint8_t> scalar_out(dst_w * 4, 0x55);
        scale_row_bgra_simd(simd_out.data(), y.data(), u.data(), v.data(),
                            sx_map.data(), sux_map.data(), dst_w, full_range, STORE_UNALIGNED);
        scale_row_bgra_scalar(scalar_out.data(), y.data(), u.data(), v.data(),
                              sx_map.data(), sux_map.data(), dst_w, full_range);
        std::printf("full_range=%d\n", static_cast<int>(full_range));
        std::printf("  simd   px0..3 = %3d %3d %3d %3d | px4..7 = %3d %3d %3d %3d\n",
                    simd_out[0], simd_out[1], simd_out[2], simd_out[3],
                    simd_out[16], simd_out[17], simd_out[18], simd_out[19]);
        std::printf("  scalar px0..3 = %3d %3d %3d %3d | px4..7 = %3d %3d %3d %3d\n",
                    scalar_out[0], scalar_out[1], scalar_out[2], scalar_out[3],
                    scalar_out[16], scalar_out[17], scalar_out[18], scalar_out[19]);
    }

    // Now vary only Y with neutral chroma: isolates the Y term.
    std::printf("\nY sweep (flat chroma 128, full_range=1):\n");
    for (int yv : {0, 16, 64, 128, 200, 235, 255}) {
        std::fill(y.begin(), y.end(), static_cast<uint8_t>(yv));
        std::vector<uint8_t> simd_out(dst_w * 4, 0), scalar_out(dst_w * 4, 0);
        scale_row_bgra_simd(simd_out.data(), y.data(), u.data(), v.data(),
                            sx_map.data(), sux_map.data(), dst_w, true, STORE_UNALIGNED);
        scale_row_bgra_scalar(scalar_out.data(), y.data(), u.data(), v.data(),
                              sx_map.data(), sux_map.data(), dst_w, true);
        std::printf("  Y=%3d  simd=%3d,%3d,%3d,%3d  scalar=%3d,%3d,%3d,%3d  %s\n",
                    yv, simd_out[0], simd_out[1], simd_out[2], simd_out[3],
                    scalar_out[0], scalar_out[1], scalar_out[2], scalar_out[3],
                    (simd_out[0] == scalar_out[0] && simd_out[1] == scalar_out[1] &&
                     simd_out[2] == scalar_out[2] && simd_out[3] == scalar_out[3])
                        ? "OK" : "DIFF");
    }

    // Chroma sweep with a fixed mid Y: isolates the chroma terms. This is where the
    // remaining +-1 differences appear (byte=1, the G channel).
    std::printf("\nChroma sweep (Y=100, full_range=1), G channel in focus:\n");
    std::fill(y.begin(), y.end(), 100);
    for (int c : {0, 1, 63, 64, 127, 128, 129, 192, 255}) {
        std::fill(u.begin(), u.end(), static_cast<uint8_t>(c));
        std::fill(v.begin(), v.end(), static_cast<uint8_t>(c));
        std::vector<uint8_t> so(dst_w * 4, 0), ro(dst_w * 4, 0);
        scale_row_bgra_simd(so.data(), y.data(), u.data(), v.data(),
                            sx_map.data(), sux_map.data(), dst_w, true, STORE_UNALIGNED);
        scale_row_bgra_scalar(ro.data(), y.data(), u.data(), v.data(),
                              sx_map.data(), sux_map.data(), dst_w, true);
        std::printf("  U=V=%3d  simd B,G,R=%3d,%3d,%3d  scalar B,G,R=%3d,%3d,%3d  %s\n",
                    c, so[0], so[1], so[2], ro[0], ro[1], ro[2],
                    (so[0] == ro[0] && so[1] == ro[1] && so[2] == ro[2]) ? "OK" : "DIFF");
    }
    return 0;
}
