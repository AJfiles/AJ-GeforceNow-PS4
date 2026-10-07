// Benchmark for the scaled row path, run on the host.
//
// WHY: on console, once the server downshifted to 960x540 and the scaled path became
// active, Present() went from 2 ms to 593 ms - a 300x regression, not the ~14 ms the
// scaler previously measured. The suspicion is the scalar fallback function that was
// added for the self-check, which took over the hot path. This measures each candidate
// implementation on a real-sized frame so the culprit is identified by timing rather
// than by reading the code.

namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
} // namespace opennow

#include "../src/opennow/stream/color_simd.cpp"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace opennow::color;

namespace {

constexpr int kSrcW = 960, kSrcH = 540;
constexpr int kDstW = 1280, kDstH = 720;

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

void build_maps(int src_w, int dst_w, std::vector<int>& sx_map, std::vector<int>& sux_map) {
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

} // namespace

int main() {
    std::vector<uint8_t> src_y(static_cast<size_t>(kSrcW) * kSrcH, 128);
    std::vector<uint8_t> src_u(static_cast<size_t>(kSrcW / 2) * (kSrcH / 2), 128);
    std::vector<uint8_t> src_v(static_cast<size_t>(kSrcW / 2) * (kSrcH / 2), 128);
    std::vector<uint8_t> dst(static_cast<size_t>(kDstW) * kDstH * 4, 0);

    std::vector<int> sx_map, sux_map;
    build_maps(kSrcW, kDstW, sx_map, sux_map);

    std::FILE* report = std::fopen("build/scale_simd_bench_report.txt", "w");
    auto emit = [report](const char* fmt, ...) {
        va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a);
        if (report) { va_start(a, fmt); std::vfprintf(report, fmt, a); va_end(a); }
    };

    const int iters = 30;

    // 1) SSE2 path, single-threaded, full frame.
    double t0 = now_ms();
    for (int i = 0; i < iters; ++i) {
        for (int y = 0; y < kDstH; ++y) {
            const int sy = (y * kSrcH) / kDstH;
            scale_row_bgra_simd(dst.data() + static_cast<size_t>(y) * kDstW * 4,
                                src_y.data() + static_cast<size_t>(sy) * kSrcW,
                                src_u.data() + static_cast<size_t>(sy / 2) * (kSrcW / 2),
                                src_v.data() + static_cast<size_t>(sy / 2) * (kSrcW / 2),
                                sx_map.data(), sux_map.data(), kDstW, false, STORE_STREAMING);
        }
    }
    const double simd_ms = (now_ms() - t0) / iters;

    // 2) The scalar fallback function used by the self-check.
    t0 = now_ms();
    for (int i = 0; i < iters; ++i) {
        for (int y = 0; y < kDstH; ++y) {
            const int sy = (y * kSrcH) / kDstH;
            scale_row_bgra_scalar(dst.data() + static_cast<size_t>(y) * kDstW * 4,
                                  src_y.data() + static_cast<size_t>(sy) * kSrcW,
                                  src_u.data() + static_cast<size_t>(sy / 2) * (kSrcW / 2),
                                  src_v.data() + static_cast<size_t>(sy / 2) * (kSrcW / 2),
                                  sx_map.data(), sux_map.data(), kDstW, false);
        }
    }
    const double scalar_ms = (now_ms() - t0) / iters;

    // 3) A plain inline per-pixel loop, i.e. what the hot path used to be.
    t0 = now_ms();
    for (int i = 0; i < iters; ++i) {
        for (int y = 0; y < kDstH; ++y) {
            const int sy = (y * kSrcH) / kDstH;
            uint8_t* drow = dst.data() + static_cast<size_t>(y) * kDstW * 4;
            const uint8_t* yrow = src_y.data() + static_cast<size_t>(sy) * kSrcW;
            const uint8_t* urow = src_u.data() + static_cast<size_t>(sy / 2) * (kSrcW / 2);
            const uint8_t* vrow = src_v.data() + static_cast<size_t>(sy / 2) * (kSrcW / 2);
            for (int x = 0; x < kDstW; ++x) {
                const int sx = sx_map[x], sux = sux_map[x];
                yuv_to_bgra_px(drow + static_cast<size_t>(x) * 4, yrow[sx],
                               static_cast<int>(urow[sux]) - 128,
                               static_cast<int>(vrow[sux]) - 128, false);
            }
        }
    }
    const double inline_ms = (now_ms() - t0) / iters;

    emit("frame %dx%d -> %dx%d, %d iterations each\n", kSrcW, kSrcH, kDstW, kDstH, iters);
    emit("scale_row_bgra_simd   : %8.3f ms/frame\n", simd_ms);
    emit("scale_row_bgra_scalar : %8.3f ms/frame\n", scalar_ms);
    emit("inline per-pixel loop : %8.3f ms/frame\n", inline_ms);
    emit("speedup simd vs scalar: %.2fx\n", scalar_ms / (simd_ms > 0 ? simd_ms : 1));
    emit("NOTE: single-threaded. The console splits rows across 3 participants, so divide\n");
    emit("      these by ~2.5-3 for the threaded figure, and remember the console runs a\n");
    emit("      ~1.6 GHz Jaguar core versus this host's much faster core.\n");
    if (report) std::fclose(report);
    return 0;
}
