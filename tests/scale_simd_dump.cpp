// Focused dump for the remaining full_range=0 mismatch.
//
// The full comparison test reports exactly 20/40 failures per resolution pair, all with
// full_range=0, at 4-byte-aligned offsets with delta=+-255. That is one whole pixel
// wrong, not a rounding difference. This prints the INPUT bytes and both OUTPUT pixels
// for the first mismatching x so the cause is visible instead of inferred.

namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
} // namespace opennow

#include "../src/opennow/stream/color_simd.cpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using opennow::color::scale_row_bgra_scalar;
using opennow::color::scale_row_bgra_simd;
using opennow::color::STORE_UNALIGNED;

// Same mapping the real ScaleBilinearYUV420PToBGRA_BT709 builds, so the test exercises
// the production index maths rather than a simplification.
static void build_maps(int src_w, int dst_w, std::vector<int>& sx_map, std::vector<int>& sux_map) {
    sx_map.resize(dst_w);
    sux_map.resize(dst_w);
    const uint32_t x_ratio = ((static_cast<uint32_t>(src_w) << 16) / dst_w);
    for (int x = 0; x < dst_w; x++) {
        int sx = static_cast<int>((x * x_ratio) >> 16);
        if (sx >= src_w) sx = src_w - 1;
        if (sx < 0) sx = 0;
        sx_map[x] = sx;
        int sux = sx / 2;
        if (src_w / 2 > 0 && sux >= src_w / 2) sux = (src_w / 2) - 1;
        if (sux < 0) sux = 0;
        sux_map[x] = sux;
    }
}

int main() {
    // Sweep several geometries and many random rows, dumping the first real mismatch
    // (delta outside tolerance). All planes are allocated with the row count actually
    // used, so no out-of-bounds read can masquerade as a subtraction bug.
    const int geoms[][2] = {{32, 32}, {960, 1280}, {1280, 1280}, {854, 1280},
                            {640, 1280}, {1920, 1920}, {1000, 1001}, {960, 1283}};

    for (const auto& g : geoms) {
        const int src_w = g[0], dst_w = g[1];
        const int src_h = 2; // only row 0 is exercised below
        std::mt19937 rng(1234);
        std::uniform_int_distribution<int> dist(0, 255);

        std::vector<uint8_t> y(static_cast<size_t>(src_w) * src_h);
        std::vector<uint8_t> u(static_cast<size_t>(src_w / 2) * 1);
        std::vector<uint8_t> v(static_cast<size_t>(src_w / 2) * 1);
        for (auto& b : y) b = static_cast<uint8_t>(dist(rng));
        for (auto& b : u) b = static_cast<uint8_t>(dist(rng));
        for (auto& b : v) b = static_cast<uint8_t>(dist(rng));

        std::vector<int> sx_map, sux_map;
        build_maps(src_w, dst_w, sx_map, sux_map);

        for (int fr = 0; fr < 2; ++fr) {
            const bool full_range = (fr == 1);
            std::vector<uint8_t> so(static_cast<size_t>(dst_w) * 4, 0x11);
            std::vector<uint8_t> ro(static_cast<size_t>(dst_w) * 4, 0x22);
            scale_row_bgra_simd(so.data(), y.data(), u.data(), v.data(),
                                sx_map.data(), sux_map.data(), dst_w, full_range, STORE_UNALIGNED);
            scale_row_bgra_scalar(ro.data(), y.data(), u.data(), v.data(),
                                  sx_map.data(), sux_map.data(), dst_w, full_range);

            int shown = 0, worst = 0;
            for (int x = 0; x < dst_w && shown < 3; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const int d = static_cast<int>(so[x * 4 + c]) - static_cast<int>(ro[x * 4 + c]);
                    const int ad = d < 0 ? -d : d;
                    if (c == 3 ? ad > 0 : ad > 1) {
                        if (ad > worst) worst = ad;
                        const int yi = sx_map[x], ci = sux_map[x];
                        std::printf("src=%-5d dst=%-5d fr=%d x=%4d c=%d IN y=%3u u=%3u v=%3u  SIMD=%3u SCALAR=%3u\n",
                                    src_w, dst_w, static_cast<int>(full_range), x, c,
                                    y[yi], u[ci], v[ci], so[x * 4 + c], ro[x * 4 + c]);
                        ++shown;
                        break;
                    }
                }
            }
            if (shown == 0) {
                std::printf("src=%-5d dst=%-5d fr=%d OK (no mismatch, worst_delta<=1 for colour)\n",
                            src_w, dst_w, static_cast<int>(full_range));
            }
        }
    }
    return 0;
}
