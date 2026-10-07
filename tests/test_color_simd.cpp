#include "src/opennow/stream/color_simd.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>

using namespace opennow::color;

// Helpers to allocate aligned memory
void* aligned_alloc_compat(size_t alignment, size_t size) {
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) return nullptr;
    return ptr;
#endif
}

void aligned_free_compat(void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "  GEFORCE NOW PS4 - COLOR SIMD BT.709 UNIT TEST\n";
    std::cout << "========================================================\n\n";

    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    constexpr int kDstPitch = kWidth * 4;

    size_t y_size = static_cast<size_t>(kWidth) * kHeight;
    size_t uv_size = static_cast<size_t>(kWidth / 2) * (kHeight / 2);
    size_t bgra_size = static_cast<size_t>(kDstPitch) * kHeight;

    std::vector<uint8_t> y_plane(y_size);
    std::vector<uint8_t> u_plane(uv_size);
    std::vector<uint8_t> v_plane(uv_size);
    std::vector<uint8_t> uv_interleaved(uv_size * 2);

    // 1. Synthetic Frame: Gradient Y [16..235], colorful variation across U and V
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            // Y ramp across screen
            y_plane[y * kWidth + x] = static_cast<uint8_t>(16 + ((x + y) % (235 - 16 + 1)));
        }
    }
    for (int y = 0; y < kHeight / 2; y++) {
        for (int x = 0; x < kWidth / 2; x++) {
            size_t idx = y * (kWidth / 2) + x;
            uint8_t u_val = static_cast<uint8_t>(16 + (x % (240 - 16 + 1)));
            uint8_t v_val = static_cast<uint8_t>(16 + (y % (240 - 16 + 1)));
            u_plane[idx] = u_val;
            v_plane[idx] = v_val;
            uv_interleaved[idx * 2 + 0] = u_val;
            uv_interleaved[idx * 2 + 1] = v_val;
        }
    }

    uint8_t* dst_scalar = static_cast<uint8_t*>(aligned_alloc_compat(64, bgra_size));
    uint8_t* dst_simd_yuv = static_cast<uint8_t*>(aligned_alloc_compat(64, bgra_size));
    uint8_t* dst_simd_nv12 = static_cast<uint8_t*>(aligned_alloc_compat(64, bgra_size));

    std::memset(dst_scalar, 0, bgra_size);
    std::memset(dst_simd_yuv, 0, bgra_size);
    std::memset(dst_simd_nv12, 0, bgra_size);

    // Compute ground truth scalar
    ConvertYUV420PToBGRA_BT709_Scalar(
        dst_scalar, kDstPitch,
        kWidth, kHeight,
        y_plane.data(), kWidth,
        u_plane.data(), kWidth / 2,
        v_plane.data(), kWidth / 2
    );

    // Compute SIMD YUV420P
    ConvertYUV420PToBGRA_BT709(
        dst_simd_yuv, kDstPitch,
        kWidth, kHeight,
        y_plane.data(), kWidth,
        u_plane.data(), kWidth / 2,
        v_plane.data(), kWidth / 2,
        STORE_ALIGNED
    );

    // Compute SIMD NV12
    ConvertNV12ToBGRA_BT709(
        dst_simd_nv12, kDstPitch,
        kWidth, kHeight,
        y_plane.data(), kWidth,
        uv_interleaved.data(), kWidth,
        STORE_ALIGNED
    );

    // 2. Verification of correctness: Max diff between Scalar and SIMD
    int max_diff_yuv = 0;
    int max_diff_nv12 = 0;
    size_t mismatch_count_yuv = 0;
    size_t mismatch_count_nv12 = 0;

    int printed = 0;
    for (size_t i = 0; i < bgra_size; i++) {
        int diff_yuv = std::abs(static_cast<int>(dst_scalar[i]) - static_cast<int>(dst_simd_yuv[i]));
        if (diff_yuv > max_diff_yuv) max_diff_yuv = diff_yuv;
        if (diff_yuv > 1) {
            mismatch_count_yuv++;
            if (printed < 5) {
                size_t px = i / 4;
                int ch = i % 4;
                std::cout << "Mismatch at byte " << i << " (pixel " << px << " ch " << ch << "): scalar="
                          << (int)dst_scalar[i] << " simd=" << (int)dst_simd_yuv[i] << "\n";
                printed++;
            }
        }

        int diff_nv12 = std::abs(static_cast<int>(dst_scalar[i]) - static_cast<int>(dst_simd_nv12[i]));
        if (diff_nv12 > max_diff_nv12) max_diff_nv12 = diff_nv12;
        if (diff_nv12 > 1) mismatch_count_nv12++;
    }

    std::cout << "[TEST 1] YUV420P SIMD vs Scalar:\n";
    std::cout << "  - Max Difference: " << max_diff_yuv << " (Threshold <= 1)\n";
    std::cout << "  - Mismatches (>1 byte): " << mismatch_count_yuv << "\n";
    bool pass_yuv = (max_diff_yuv <= 1 && mismatch_count_yuv == 0);
    std::cout << "  - Status: " << (pass_yuv ? ">>> PASS <<<" : ">>> FAIL <<<") << "\n\n";

    std::cout << "[TEST 2] NV12 SIMD vs Scalar:\n";
    std::cout << "  - Max Difference: " << max_diff_nv12 << " (Threshold <= 1)\n";
    std::cout << "  - Mismatches (>1 byte): " << mismatch_count_nv12 << "\n";
    bool pass_nv12 = (max_diff_nv12 <= 1 && mismatch_count_nv12 == 0);
    std::cout << "  - Status: " << (pass_nv12 ? ">>> PASS <<<" : ">>> FAIL <<<") << "\n\n";

    // 3. Performance Benchmark: 100 frames 1080p
    constexpr int kIterations = 100;
    std::vector<double> durations_ms;
    durations_ms.reserve(kIterations);

    for (int iter = 0; iter < kIterations; iter++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        ConvertYUV420PToBGRA_BT709(
            dst_simd_yuv, kDstPitch,
            kWidth, kHeight,
            y_plane.data(), kWidth,
            u_plane.data(), kWidth / 2,
            v_plane.data(), kWidth / 2,
            STORE_STREAMING
        );
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        durations_ms.push_back(ms);
    }

    double total_ms = std::accumulate(durations_ms.begin(), durations_ms.end(), 0.0);
    double avg_ms = total_ms / kIterations;
    double sq_sum = 0.0;
    for (double d : durations_ms) sq_sum += (d - avg_ms) * (d - avg_ms);
    double stddev = std::sqrt(sq_sum / kIterations);

    std::cout << "[TEST 3] Benchmark Performance (1080p, single-thread SSE2):\n";
    std::cout << "  - Iterations: " << kIterations << "\n";
    std::cout << "  - Average Time per frame: " << avg_ms << " ms\n";
    std::cout << "  - StdDev: +/- " << stddev << " ms\n";
    std::cout << "  - Target: < 5.0 ms (Single Thread)\n";
    bool pass_perf = (avg_ms < 5.0);
    std::cout << "  - Status: " << (pass_perf ? ">>> PASS <<<" : ">>> WARN/PASS <<<") << "\n\n";

    aligned_free_compat(dst_scalar);
    aligned_free_compat(dst_simd_yuv);
    aligned_free_compat(dst_simd_nv12);

    if (pass_yuv && pass_nv12) {
        std::cout << "ALL SUITE TESTS PASSED SUCCESSFULLY.\n";
        return 0;
    } else {
        std::cout << "SUITE TESTS FAILED.\n";
        return 1;
    }
}
