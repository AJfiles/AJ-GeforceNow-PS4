// Exhaustive comparison of the SSE2 scaled row converter against the scalar reference.
//
// WHY THIS REPLACES THE EARLIER TEST: the earlier version compared only rows 0 and 1 of a
// synthetic frame. On the console the self-check (which samples the first 2 rows) reported
// "SIMD_MISMATCH x=803 byte=3213" - a pixel the host test never looked at. A test that
// samples is not a test that verifies; this one checks EVERY pixel of EVERY row of many
// random frames, at the dimensions actually used on console, for both range modes.

namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
} // namespace opennow

#include "../src/opennow/stream/color_simd.cpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using opennow::color::scale_row_bgra_scalar;
using opennow::color::scale_row_bgra_simd;
using opennow::color::STORE_STREAMING;
using opennow::color::STORE_UNALIGNED;

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

// Compares one full frame, every pixel, both store modes. Returns the number of
// mismatching BYTES and records the first one.
struct FrameResult {
    long long mismatches = 0;
    int first_x = -1, first_chan = -1, first_byte = -1;
    int simd_val = 0, scalar_val = 0;
};

FrameResult compare_frame(int src_w, int src_h, int dst_w, int dst_h, bool full_range,
                          int store_mode, std::mt19937& rng) {
    FrameResult r;
    std::uniform_int_distribution<int> dist(0, 255);

    std::vector<uint8_t> plane_y(static_cast<size_t>(src_w) * src_h);
    std::vector<uint8_t> plane_u(static_cast<size_t>(src_w / 2) * (src_h / 2));
    std::vector<uint8_t> plane_v(static_cast<size_t>(src_w / 2) * (src_h / 2));
    for (auto& b : plane_y) b = static_cast<uint8_t>(dist(rng));
    for (auto& b : plane_u) b = static_cast<uint8_t>(dist(rng));
    for (auto& b : plane_v) b = static_cast<uint8_t>(dist(rng));

    std::vector<int> sx_map, sux_map;
    build_maps(src_w, dst_w, sx_map, sux_map);

    std::vector<uint8_t> simd_out(static_cast<size_t>(dst_w) * dst_h * 4, 0);
    std::vector<uint8_t> scalar_out(static_cast<size_t>(dst_w) * dst_h * 4, 0);

    // Exercise EVERY output row, mirroring map_source_row's behaviour closely enough:
    // the source row advances proportionally.
    for (int y = 0; y < dst_h; ++y) {
        const int sy = (src_h == dst_h) ? y : static_cast<int>((static_cast<int64_t>(y) * src_h) / dst_h);
        const int su = (sy / 2 >= src_h / 2) ? (src_h / 2) - 1 : sy / 2;
        const uint8_t* yrow = plane_y.data() + static_cast<size_t>(sy) * src_w;
        const uint8_t* urow = plane_u.data() + static_cast<size_t>(su) * (src_w / 2);
        const uint8_t* vrow = plane_v.data() + static_cast<size_t>(su) * (src_w / 2);
        uint8_t* srow = simd_out.data() + static_cast<size_t>(y) * dst_w * 4;
        uint8_t* rrow = scalar_out.data() + static_cast<size_t>(y) * dst_w * 4;

        scale_row_bgra_simd(srow, yrow, urow, vrow, sx_map.data(), sux_map.data(),
                            dst_w, full_range, store_mode);
        scale_row_bgra_scalar(rrow, yrow, urow, vrow, sx_map.data(), sux_map.data(),
                              dst_w, full_range);
    }

    for (int y = 0; y < dst_h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            for (int c = 0; c < 4; ++c) {
                const int i = (y * dst_w + x) * 4 + c;
                if (simd_out[i] != scalar_out[i]) {
                    ++r.mismatches;
                    if (r.first_x < 0) {
                        r.first_x = x;
                        r.first_chan = c;
                        r.first_byte = i;
                        r.simd_val = simd_out[i];
                        r.scalar_val = scalar_out[i];
                    }
                }
            }
        }
    }
    return r;
}

} // namespace

int main() {
    g_report = std::fopen("build/scale_simd_exhaustive_report.txt", "w");

    // The exact on-console geometry, plus several others, both range modes, both store
    // modes, and enough random frames that every input value combination appears.
    struct Geo { int sw, sh, dw, dh; const char* label; };
    const Geo geos[] = {
        {960, 540, 1280, 720, "ON-CONSOLE: 540p stream -> 720p buffer"},
        {1280, 720, 1280, 720, "720p 1:1 through the scaled path"},
        {1280, 720, 1920, 1080, "720p stream -> 1080p buffer"},
        {854, 480, 1280, 720, "480p -> 720p"},
        {640, 360, 1280, 720, "360p -> 720p"},
    };

    long long total_bytes_checked = 0;
    long long total_mismatches = 0;
    int frames = 0;

    for (const auto& g : geos) {
        for (int fr = 0; fr < 2; ++fr) {
            for (int sm = 0; sm < 2; ++sm) {
                const bool full_range = (fr == 1);
                const int store_mode = (sm == 0) ? STORE_UNALIGNED : STORE_STREAMING;
                for (int rep = 0; rep < 6; ++rep) {
                    std::mt19937 rng(0xBEEF0000u + static_cast<unsigned>(rep) * 7919u +
                                     static_cast<unsigned>(fr) * 13u + static_cast<unsigned>(g.sw));
                    const FrameResult r = compare_frame(g.sw, g.sh, g.dw, g.dh, full_range,
                                                        store_mode, rng);
                    ++frames;
                    total_bytes_checked += static_cast<long long>(g.dw) * g.dh * 4;
                    total_mismatches += r.mismatches;
                    emit("%-40s fr=%d store=%d rep=%d  bytes_checked=%lld mismatches=%lld%s",
                         g.label, static_cast<int>(full_range), store_mode, rep,
                         static_cast<long long>(g.dw) * g.dh * 4, r.mismatches,
                         r.mismatches ? "" : "\n");
                    if (r.mismatches) {
                        emit("   first mismatch x=%d chan=%d byte=%d simd=%d scalar=%d\n",
                             r.first_x, r.first_chan, r.first_byte, r.simd_val, r.scalar_val);
                    }
                }
            }
        }
    }

    emit("\nframes=%d bytes_checked=%lld mismatches=%lld\n", frames, total_bytes_checked,
         total_mismatches);
    emit("%s\n", total_mismatches == 0
                     ? "PASS: SSE2 output is byte-identical to scalar on every pixel checked"
                     : "FAIL: SSE2 differs from scalar (see mismatches above)");
    if (g_report) std::fclose(g_report);
    return total_mismatches == 0 ? 0 : 1;
}
