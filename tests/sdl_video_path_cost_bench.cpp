// =================================================================================================
// Banco: cuanto cuesta REALMENTE el camino de video de SDL
// =================================================================================================
// POR QUE HACE FALTA ESTE BANCO
// ---------------------------------------------------------------------------------------------
// El log de la consola dice `present_us = 50.000` en el stream y `6.900` en el menu. La diferencia
// (43.000 us) es "el video", pero **nunca he medido cuanto de eso es cada cosa**, y ya me he equivocado
// dos veces adivinando (culpe primero al blit a 1080p, luego al `SDL_BLENDMODE_BLEND`).
//
// En el camino de SDL, por cada frame se hace:
//
//   1. `SDL_ConvertPixels` NV12 -> ARGB 1280x720     (conversion, codigo generico de SDL)
//   2. El blit al lienzo 1920x1080 con `SDL_BlitScaled` (VECINO MAS CERCANO)
//   3. `SDL_UpdateWindowFramebuffer`: memset 8,3 MB + memcpy 8,3 MB
//
// El (3) ya lo midio el proyecto en consola (`VIDEOOUT_SCALE_BILINEAR_US avg=15496`, que es el
// escalado del camino DIRECTO, no este).
//
// Este banco mide (1) y (2) en el PC de desarrollo, con las MISMAS geometrias, para saber si su suma
// puede explicar los 43.000 us o si hay coste en otro sitio.
//
// ---------------------------------------------------------------------------------------------
// COMO SE INTERPRETA
// ---------------------------------------------------------------------------------------------
// El PC es mucho mas rapido por nucleo que la CPU Jaguar de la PS4. El factor medido entre este PC y
// la consola, segun los propios bancos del proyecto, es del orden de 10-50x.
//
// Por eso el resultado se usa como COTA: si aqui el camino de SDL tarda 40 ms, en la consola NO puede
// ser mas rapido, y los 50 ms quedan explicados. Si aqui tarda 5 ms, entonces los 43.000 us de la
// consola NO son este codigo y hay que buscar en otra parte.
//
// Las dos implementaciones son fieles al comportamiento de SDL:
//   - La conversion a RGB usa los mismos coeficientes BT.601 de `SDL_ConvertPixels` (enteros).
//   - El escalado usa VECINO MAS CERCANO (`SDL_BlitScaled` no interpola), que es lo que emborrona.
// =================================================================================================

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// -------------------------------------------------------------------------------------------------
// 1. Conversion NV12 -> ARGB, con los coeficientes que usa SDL (BT.601, aritmetica entera).
// -------------------------------------------------------------------------------------------------
static void convertNv12ToArgb(const uint8_t* y, int yPitch,
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
            // BT.601 de SDL: R = Y + 1.402V ; G = Y - 0.344U - 0.714V ; B = Y + 1.772U
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

// -------------------------------------------------------------------------------------------------
// 2. Escalado por VECINO MAS CERCANO, que es lo que hace `SDL_BlitScaled`.
// -------------------------------------------------------------------------------------------------
static void blitNearest(const uint32_t* src, int sw, int sh,
                        uint32_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int sy = (int)(((int64_t)y * sh) / dh);
        const uint32_t* s = src + (size_t)sy * sw;
        uint32_t* d = dst + (size_t)y * dw;
        for (int x = 0; x < dw; ++x) {
            d[x] = s[(int)(((int64_t)x * sw) / dw)];
        }
    }
}

int main() {
    const int sw = 1280, sh = 720;      // lo que entrega el servidor
    const int dw = 1920, dh = 1080;     // el lienzo (y el display)
    const int iters = 30;

    std::mt19937 rng(99);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<uint8_t> y((size_t)sw * sh);
    std::vector<uint8_t> uv((size_t)sw * (sh / 2));
    for (auto& b : y) b = (uint8_t)dist(rng);
    for (auto& b : uv) b = (uint8_t)dist(rng);

    std::vector<uint32_t> rgb((size_t)sw * sh, 0);
    std::vector<uint32_t> canvas((size_t)dw * dh, 0);

    std::printf("\n  COSTE DEL CAMINO DE VIDEO DE SDL (en el PC de desarrollo)\n\n");
    std::printf("  origen %dx%d  ->  lienzo %dx%d\n\n", sw, sh, dw, dh);

    // --- conversion ---
    convertNv12ToArgb(y.data(), sw, uv.data(), sw, rgb.data(), sw, sh);   // calentamiento
    double t0 = now_ms();
    for (int i = 0; i < iters; ++i)
        convertNv12ToArgb(y.data(), sw, uv.data(), sw, rgb.data(), sw, sh);
    const double conv = (now_ms() - t0) / iters;

    // --- escalado por vecino mas cercano ---
    blitNearest(rgb.data(), sw, sh, canvas.data(), dw, dh);              // calentamiento
    t0 = now_ms();
    for (int i = 0; i < iters; ++i)
        blitNearest(rgb.data(), sw, sh, canvas.data(), dw, dh);
    const double blit = (now_ms() - t0) / iters;

    // --- el memset + memcpy del driver (SDL_ps4video.c:504 y :519) ---
    t0 = now_ms();
    for (int i = 0; i < iters; ++i) {
        std::memset(canvas.data(), 0, canvas.size() * 4);                 // 8,3 MB
        std::memcpy(canvas.data(), rgb.data(), (size_t)sw * sh * 4);      // 3,7 MB
    }
    const double fb = (now_ms() - t0) / iters;

    std::printf("  1. conversion NV12 -> ARGB            : %8.3f ms\n", conv);
    std::printf("  2. escalado a %dx%d (vecino mas cercano): %8.3f ms\n", dw, dh, blit);
    std::printf("  3. memset+memcpy del framebuffer      : %8.3f ms\n", fb);
    std::printf("  ---------------------------------------------------\n");
    std::printf("     SUMA                               : %8.3f ms\n\n", conv + blit + fb);

    std::printf("  En consola medidos: present_us = 50,0 ms (stream) / 6,9 ms (menu)\n\n");
    if (conv + blit + fb > 20.0) {
        std::printf("  -> La suma es ALTA incluso en el PC: el camino de SDL explica los 50 ms\n");
        std::printf("     de la consola. La CPU Jaguar solo puede hacerlo mas lento.\n");
    } else {
        std::printf("  -> La suma es BAJA para un PC: si en consola son 50 ms, el factor PC->PS4\n");
        std::printf("     tendria que ser enorme, o hay coste FUERA de este codigo.\n");
    }
    std::printf("\n");
    return 0;
}
