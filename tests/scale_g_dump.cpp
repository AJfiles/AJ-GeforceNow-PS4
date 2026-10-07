// Dumps every intermediate of the G channel computation for both paths at x=0.
//
// The exhaustive test reports ~811k mismatching bytes, all in the G channel (chan=1),
// almost all by exactly 1. The two implementations contain what look like identical
// integer expressions, so the difference must be in a detail that reading cannot settle.
// This prints the inputs and each intermediate side by side.

namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
} // namespace opennow

#include "../src/opennow/stream/color_simd.cpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using opennow::color::scale_row_bgra_scalar;
using opennow::color::scale_row_bgra_simd;
using opennow::color::STORE_UNALIGNED;

int main() {
    const int src_w = 16, dst_w = 16;

    for (int fr = 0; fr < 2; ++fr) {
        const bool full_range = (fr == 1);
        // One row of gradually varying values, so every case appears.
        std::vector<uint8_t> plane_y(src_w), plane_u(src_w / 2), plane_v(src_w / 2);
        for (int i = 0; i < src_w; ++i) plane_y[i] = static_cast<uint8_t>(20 + i * 14);
        for (int i = 0; i < src_w / 2; ++i) plane_u[i] = static_cast<uint8_t>(40 + i * 30);
        for (int i = 0; i < src_w / 2; ++i) plane_v[i] = static_cast<uint8_t>(60 + i * 25);

        std::vector<int> sx_map(dst_w), sux_map(dst_w);
        for (int x = 0; x < dst_w; ++x) { sx_map[x] = x; sux_map[x] = x / 2; }

        std::vector<uint8_t> so(dst_w * 4, 0), ro(dst_w * 4, 0);
        scale_row_bgra_simd(so.data(), plane_y.data(), plane_u.data(), plane_v.data(),
                            sx_map.data(), sux_map.data(), dst_w, full_range, STORE_UNALIGNED);
        scale_row_bgra_scalar(ro.data(), plane_y.data(), plane_u.data(), plane_v.data(),
                              sx_map.data(), sux_map.data(), dst_w, full_range);

        std::printf("full_range=%d   (scalar reference formula shown alongside)\n",
                    static_cast<int>(full_range));
        for (int x = 0; x < dst_w; ++x) {
            const int sy = sx_map[x], sux = sux_map[x];
            const int y_raw = plane_y[sy];
            const int uu = static_cast<int>(plane_u[sux]) - 128;
            const int vv = static_cast<int>(plane_v[sux]) - 128;
            const int y_adj = full_range ? y_raw : (y_raw < 16 ? 0 : y_raw - 16);
            const int y_sc = ((full_range ? 128 : 149) * y_adj) >> 7;
            const int guv = ((full_range ? 24 : 27) * uu + (full_range ? 60 : 68) * vv) >> 7;
            const int g_expected = y_sc - guv;
            const int g_simd = so[x * 4 + 1];
            const int g_scalar = ro[x * 4 + 1];
            std::printf(" x=%2d y=%3d uu=%4d vv=%4d | y_adj=%3d y_sc=%3d guv=%4d -> g_calc=%4d | SIMD G=%3d SCALAR G=%3d %s\n",
                        x, y_raw, uu, vv, y_adj, y_sc, guv, g_expected,
                        g_simd, g_scalar,
                        (g_simd == g_scalar) ? "" : "  <-- DIFF");
        }
        std::printf("\n");
    }
    return 0;
}
