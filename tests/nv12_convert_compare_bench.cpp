// =================================================================================================
// Banco: el conversor SSE2 del proyecto contra el escalar de SDL, SIN escalar
// =================================================================================================
// POR QUE ESTE BANCO Y NO OTRO
// ---------------------------------------------------------------------------------------------
// El banco anterior (`sdl_video_path_cost_bench.cpp`) midio el camino de SDL en el PC:
//
//     conversion NV12 -> ARGB (escalar)     : 14,702 ms
//     escalado a 1920x1080 (vecino cercano) :  2,097 ms
//     memset+memcpy del framebuffer         :  1,153 ms
//
// **El escalado cuesta 2 ms y el memset+memcpy 1,2 ms. LA CONVERSION ES EL 82 % DEL COSTE.**
//
// Eso cambia la conclusion: llevo tres rondas culpando al "blit a 1080p" y **no era eso**. Lo que
// domina es la conversion de color, y ahi si puede haber una mejora real: el proyecto tiene un
// conversor **SSE2** (`ConvertNV12ToBGRA_BT709`) contra el codigo escalar de SDL.
//
// Este banco mide LOS DOS sin escalar (1280x720 -> 1280x720), para que la comparacion sea limpia:
// mismo tamano de salida, mismo numero de pixeles, unica diferencia el codigo de conversion.
//
// ---------------------------------------------------------------------------------------------
// QUE DECIDE ESTE RESULTADO
// ---------------------------------------------------------------------------------------------
//   - Si el SSE2 es claramente mas rapido  -> activar la ruta propia (v3.49) SI esta justificado,
//     pero **solo la conversion**, no el escalado bilineal a 1080p (que es lo que la hacia lenta).
//   - Si son parecidos                      -> la ruta propia no aporta y se queda desactivada.
//
// Es la diferencia entre "creo que es mas rapido" y saberlo.
// =================================================================================================

#include "../src/opennow/stream/color_simd.cpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
}

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// El mismo camino escalar que usa SDL, copiado del banco anterior para que la comparacion sea justa.
static void convertNv12ToArgbScalar(const uint8_t* y, int yPitch,
                                    const uint8_t* uv, int uvPitch,
                                    uint32_t* dst, int w, int h) {
    for (int row = 0; row < h; ++row) {
        const uint8_t* yRow = y + (size_t)row * yPitch;
        const uint8_t* uvRow = uv + (size_t)(row >> 1) * uvPitch;
        uint32_t* d = dst + (size_t)row * w;
        for (int col = 0; col < w; ++col) {
            const int Y = yRow[col];
            const int U = uvRow[(col >> 1) * 2 + 0] - 128;
            const int V = uvRow[(col >> 1) * 2 + 1] - 128;
            int r = Y + ((91881 * V) >> 16);
            int g = Y - ((22554 * U + 46802 * V) >> 16);
            int b = Y + ((116130 * U) >> 16);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            d[col] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

int main() {
    const int w = 1280, h = 720;
    const int iters = 30;

    std::mt19937 rng(99);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<uint8_t> y((size_t)w * h);
    std::vector<uint8_t> uv((size_t)w * (h / 2));
    for (auto& b : y) b = (uint8_t)dist(rng);
    for (auto& b : uv) b = (uint8_t)dist(rng);
    std::vector<uint8_t> dst((size_t)w * h * 4, 0);

    std::printf("\n  CONVERSOR SSE2 DEL PROYECTO  vs  ESCALAR (como SDL), sin escalar\n\n");

    // --- SSE2 del proyecto ---
    opennow::color::ConvertNV12ToBGRA_BT709(dst.data(), w * 4, w, h,
                                            y.data(), w, uv.data(), w,
                                            opennow::color::STORE_UNALIGNED, false);
    double t0 = now_ms();
    for (int i = 0; i < iters; ++i)
        opennow::color::ConvertNV12ToBGRA_BT709(dst.data(), w * 4, w, h,
                                                y.data(), w, uv.data(), w,
                                                opennow::color::STORE_UNALIGNED, false);
    const double simd = (now_ms() - t0) / iters;

    // --- escalar, como SDL ---
    convertNv12ToArgbScalar(y.data(), w, uv.data(), w, (uint32_t*)dst.data(), w, h);
    t0 = now_ms();
    for (int i = 0; i < iters; ++i)
        convertNv12ToArgbScalar(y.data(), w, uv.data(), w, (uint32_t*)dst.data(), w, h);
    const double scalar = (now_ms() - t0) / iters;

    std::printf("  SSE2 del proyecto (ConvertNV12ToBGRA_BT709) : %8.3f ms\n", simd);
    std::printf("  Escalar de SDL (mismos coeficientes)        : %8.3f ms\n", scalar);
    std::printf("  -----------------------------------------------------\n");
    if (simd > 0.0) {
        std::printf("  Ganancia del SSE2: %.2fx\n\n", scalar / simd);
        if ((scalar / simd) > 1.5) {
            std::printf("  -> El SSE2 es CLARAMENTE mas rapido: la ruta propia del proyecto (v3.49)\n");
            std::printf("     SI esta justificada en la conversion. Lo que la hacia lenta era el\n");
            std::printf("     escalado bilineal a 1080p que se le anadio en la v3.53.\n");
        } else {
            std::printf("  -> Los dos son parecidos: la ruta propia NO aporta en la conversion.\n");
            std::printf("     Debe quedarse desactivada y buscar la mejora en otro sitio.\n");
        }
    }
    std::printf("\n");
    return 0;
}
