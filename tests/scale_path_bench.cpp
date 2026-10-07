// Benchmarks the REAL scaled path end to end, worker pool included, exactly as
// PS4VideoOutRenderer::Present() invokes it.
//
// WHY: the console reports convert_us=1121145 for frames=2, i.e. 560 ms per frame, in the
// scaled path only. A single-threaded row benchmark measures ~6 ms, and the scaler's own
// telemetry never even reached its 100-frame logging interval, so the cost is either in the
// worker pool dispatch or in how Present() calls this. This calls the production function
// with production arguments and times it, then prints the pool's thread count so a pool
// problem cannot hide.

namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
} // namespace opennow

#include "../src/opennow/stream/color_simd.cpp"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

std::FILE* g_report = nullptr;

void emit(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    if (g_report) {
        va_start(a, fmt);
        std::vfprintf(g_report, fmt, a);
        va_end(a);
    }
}

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

} // namespace

int main() {
    g_report = std::fopen("build/scale_path_bench_report.txt", "w");

    // The exact on-console geometry.
    const int src_w = 960, src_h = 540;
    const int dst_w = 1280, dst_h = 720;
    const int dst_pitch = dst_w * 4;

    emit("pool thread_count() = %d (participants including the caller)\n",
         opennow::color::RowWorkerPoolProbe());

    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<uint8_t> plane_y(static_cast<size_t>(src_w) * src_h);
    std::vector<uint8_t> plane_u(static_cast<size_t>(src_w / 2) * (src_h / 2));
    std::vector<uint8_t> plane_v(static_cast<size_t>(src_w / 2) * (src_h / 2));
    for (auto& b : plane_y) b = static_cast<uint8_t>(dist(rng));
    for (auto& b : plane_u) b = static_cast<uint8_t>(dist(rng));
    for (auto& b : plane_v) b = static_cast<uint8_t>(dist(rng));

    std::vector<uint8_t> dst(static_cast<size_t>(dst_pitch) * dst_h, 0);

    const int iters = 60;

    // Warm up (first call also performs the one-shot SIMD self-check).
    double first_call_ms = 0.0;
    {
        const double t0 = now_ms();
        opennow::color::ScaleBilinearYUV420PToBGRA_BT709(
            dst.data(), dst_pitch, dst_w, dst_h, src_w, src_h,
            plane_y.data(), src_w, plane_u.data(), src_w / 2, plane_v.data(), src_w / 2, false);
        first_call_ms = now_ms() - t0;
    }

    const double t0 = now_ms();
    for (int i = 0; i < iters; ++i) {
        opennow::color::ScaleBilinearYUV420PToBGRA_BT709(
            dst.data(), dst_pitch, dst_w, dst_h, src_w, src_h,
            plane_y.data(), src_w, plane_u.data(), src_w / 2, plane_v.data(), src_w / 2, false);
    }
    const double per_frame_ms = (now_ms() - t0) / iters;

    emit("geometry: %dx%d -> %dx%d\n", src_w, src_h, dst_w, dst_h);
    emit("first call (includes the one-shot self-check): %.3f ms\n", first_call_ms);
    emit("steady state: %.3f ms/frame over %d frames\n", per_frame_ms, iters);
    emit("\nFor reference, the console measured 560 ms/frame on a ~1.6 GHz Jaguar core.\n");
    emit("This host core is several times faster, but not 60x faster, so a result in the\n");
    emit("single-digit milliseconds here means the 560 ms on console is NOT this function.\n");

    if (g_report) std::fclose(g_report);
    return 0;
}
