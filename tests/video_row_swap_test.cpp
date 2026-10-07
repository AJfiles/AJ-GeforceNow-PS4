// =====================================================================================================
// TEST DE LA ROTACION R<->B DE LA COPIA POR FILAS (v4.18)
// =====================================================================================================
// POR QUE EXISTE ESTE TEST, y no es una formalidad:
//
// En la v4.15 sustitui `SDL_RenderCopy` por un `memcpy` por filas razonando que "el orden de bytes ya era
// el correcto". **Era falso**, y el usuario lo vio en pantalla: los colores salieron alterados.
// `SDL_RenderCopy` **convertia** de formato, y esa conversion era la que dejaba los bytes en el orden que
// el driver necesita.
//
// La correccion deshace el intercambio R<->B dentro de la copia, con SSE2. **Razonar sobre rotaciones de
// bits es exactamente el tipo de cosa que en este proyecto ha estado mal varias veces**, asi que aqui se
// comprueba byte a byte contra una referencia escalar, con los DOS caminos escritos de forma distinta.
//
// Si esto falla, los colores del video saldrian mal otra vez. Es barato y detecta el error en el PC.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

static int g_fallos = 0;
static int g_total  = 0;

static void Check(const char* nombre, bool ok, const char* detalle) {
    ++g_total;
    if (!ok) { ++g_fallos; std::printf("    [FALLO] %s  %s\n", nombre, detalle); }
    else     { std::printf("    [OK]    %s\n", nombre); }
}

// ---------------------------------------------------------------------------------------------------
// La rotacion, EXACTAMENTE como esta en `SDLVideoRenderer.cpp`.
// ---------------------------------------------------------------------------------------------------
static void RotarInPlaceSse2(uint8_t* d, const uint8_t* s, int bytes) {
    const __m128i kLo = _mm_set1_epi32(static_cast<int>(0x00FF00FFu));
    const __m128i kHi = _mm_set1_epi32(static_cast<int>(0xFF00FF00u));
    int x = 0;
    for (; x + 16 <= bytes; x += 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x));
        const __m128i rl = _mm_or_si128(_mm_slli_epi32(v, 16), _mm_srli_epi32(v, 16));
        const __m128i swapped = _mm_or_si128(_mm_and_si128(rl, kLo), _mm_and_si128(v, kHi));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x), swapped);
    }
    for (; x < bytes; x += 4) {
        const uint8_t b0 = s[x + 0], b1 = s[x + 1];
        d[x + 0] = s[x + 2];
        d[x + 1] = b1;
        d[x + 2] = b0;
        d[x + 3] = s[x + 3];
    }
}

// ---------------------------------------------------------------------------------------------------
// Referencia escalar, escrita de OTRA forma: pixel a pixel con nombres de canal.
// ---------------------------------------------------------------------------------------------------
static void RotarReferencia(uint8_t* d, const uint8_t* s, int bytes) {
    for (int x = 0; x < bytes; x += 4) {
        const uint8_t b = s[x + 0];
        const uint8_t g = s[x + 1];
        const uint8_t r = s[x + 2];
        const uint8_t a = s[x + 3];
        d[x + 0] = r;
        d[x + 1] = g;
        d[x + 2] = b;
        d[x + 3] = a;
    }
}

int main() {
    std::printf("\n  ROTACION R<->B DE LA COPIA POR FILAS (SSE2 vs referencia)\n\n");

    // --- 1: bytes conocidos, para ver el orden a mano ---
    {
        // Un pixel BGRA puro: B=0x11, G=0x22, R=0x33, A=0x44
        uint8_t src[4] = {0x11, 0x22, 0x33, 0x44};
        uint8_t dst[4] = {0, 0, 0, 0};
        RotarInPlaceSse2(dst, src, 4);
        char d[96];
        std::snprintf(d, sizeof(d), "salida=%02X %02X %02X %02X (esperado 33 22 11 44)",
                      dst[0], dst[1], dst[2], dst[3]);
        Check("un pixel BGRA pasa a RGBA", dst[0] == 0x33 && dst[1] == 0x22 &&
                                          dst[2] == 0x11 && dst[3] == 0x44, d);
    }

    // --- 2: el alfa NO se toca (si se tocara, el video podria transparentarse) ---
    {
        uint8_t src[16], dst[16];
        for (int i = 0; i < 16; ++i) src[i] = static_cast<uint8_t>(i);
        std::memset(dst, 0, sizeof(dst));
        RotarInPlaceSse2(dst, src, 16);
        bool alfaOk = (dst[3] == src[3]) && (dst[7] == src[7]) &&
                      (dst[11] == src[11]) && (dst[15] == src[15]);
        Check("el canal alfa se conserva en los 4 pixeles", alfaOk, "");
    }

    // --- 3: verde intacto ---
    {
        uint8_t src[8], dst[8];
        for (int i = 0; i < 8; ++i) src[i] = static_cast<uint8_t>(0x80 + i);
        std::memset(dst, 0, sizeof(dst));
        RotarInPlaceSse2(dst, src, 8);
        Check("el canal verde se conserva", dst[1] == src[1] && dst[5] == src[5], "");
    }

    // --- 4: SSE2 y referencia coinciden en un barrido largo (incluye el camino vectorizado) ---
    {
        const int bytes = 1920 * 4;         // una fila real de 1920x1080
        static uint8_t src[1920 * 4];
        static uint8_t a[1920 * 4];
        static uint8_t b[1920 * 4];
        for (int i = 0; i < bytes; ++i) src[i] = static_cast<uint8_t>((i * 7 + (i >> 3)) & 0xFF);
        std::memset(a, 0, sizeof(a)); std::memset(b, 0, sizeof(b));
        RotarInPlaceSse2(a, src, bytes);
        RotarReferencia(b, src, bytes);
        int primera = -1;
        for (int i = 0; i < bytes; ++i) if (a[i] != b[i]) { primera = i; break; }
        char d[96];
        std::snprintf(d, sizeof(d), "primer byte distinto=%d", primera);
        Check("SSE2 == referencia en una fila de 1920 px", primera < 0, d);
    }

    // --- 5: un barrido que NO es multiplo de 16, para cubrir el bucle de cola ---
    {
        for (int px = 1; px <= 9; ++px) {
            const int bytes = px * 4;
            uint8_t src[64], a[64], b[64];
            for (int i = 0; i < bytes; ++i) src[i] = static_cast<uint8_t>(i * 13 + px);
            std::memset(a, 0, sizeof(a)); std::memset(b, 0, sizeof(b));
            RotarInPlaceSse2(a, src, bytes);
            RotarReferencia(b, src, bytes);
            if (std::memcmp(a, b, bytes) != 0) {
                char d[64];
                std::snprintf(d, sizeof(d), "px=%d no coinciden", px);
                Check("cola exacta para cualquier ancho (1..9 px)", false, d);
                goto fin_cola;
            }
        }
        Check("cola exacta para cualquier ancho (1..9 px)", true, "");
    }
fin_cola:

    // --- 6: la rotacion es su propia inversa (aplicarla dos veces devuelve el original) ---
    {
        uint8_t src[32], tmp[32], dst[32];
        for (int i = 0; i < 32; ++i) src[i] = static_cast<uint8_t>(i * 5 + 1);
        std::memset(tmp, 0, sizeof(tmp)); std::memset(dst, 0, sizeof(dst));
        RotarInPlaceSse2(tmp, src, 32);
        RotarInPlaceSse2(dst, tmp, 32);
        Check("aplicarla dos veces devuelve el original", std::memcmp(src, dst, 32) == 0, "");
    }

    std::printf("\n");
    if (g_fallos == 0) {
        std::printf("    Todas las comprobaciones de la rotacion R<->B pasaron\n\n");
        return 0;
    }
    std::printf("    FALLARON %d de %d comprobaciones de la rotacion R<->B\n\n", g_fallos, g_total);
    return 1;
}
