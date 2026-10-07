// =================================================================================================
// Verificacion del conversor de color que usa la RUTA DE VIDEO POR DEFECTO desde la v3.49
// =================================================================================================
// POR QUE EXISTE ESTA PRUEBA
// ---------------------------------------------------------------------------------------------
// La v3.49 activa por defecto la conversion NV12/YUV420P -> BGRA con el codigo SSE2 del proyecto
// (`opennow::color::ConvertNV12ToBGRA_BT709`), en lugar de dejar que lo haga SDL con su
// `SDL_ConvertPixels`.
//
// Eso significa que **todos los colores del video que se ve en pantalla pasan por ese conversor**. Si
// tuviera un error de matriz o de orden de canales, el juego se veria con colores cambiados y seria un
// fallo mucho peor que el que se estaba arreglando.
//
// El proyecto YA tenia `tests/test_color_simd.cpp`, pero **no esta en la bateria** de
// `scripts/run-host-tests.ps1` y depende de `LogAppLifecycleEvent`, que no existe en un binario de
// host. Esta prueba es independiente, no depende de nada del cliente y comprueba lo unico que importa
// para este cambio: **que los colores salgan bien**.
//
// QUE COMPRUEBA
// ---------------------------------------------------------------------------------------------
//   1. Que YUV420P y NV12 dan EXACTAMENTE el mismo resultado para el mismo contenido. Son dos
//      representaciones del mismo color: si difieren, una de las dos rutas esta mal.
//   2. Que el blanco puro sale blanco y el negro puro sale negro (limites del rango).
//   3. Que el gris neutro (U=V=128) sale gris, es decir R=G=B. Es la comprobacion que caza un error de
//      matriz: si los coeficientes de U y V estan mal, el gris sale tintado.
//   4. Que una imagen con un gris conocido (Y=126, el gris "de video" de 16-235) da un gris razonable.
//   5. Que el canal alfa queda a 255 (opaco). Si quedara a 0, SDL dibujaria el video transparente y no
//      se veria nada.

#include "../src/opennow/stream/color_simd.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace opennow::color;

// ------------------------------------------------------------------------------------------------
// STUB DE LOG
// ------------------------------------------------------------------------------------------------
// `color_simd.cpp` llama a `opennow::LogAppLifecycleEvent` en su instrumentacion. En un binario de host
// esa funcion no existe (vive en el logger del cliente, que escribe en /data del PS4), asi que se
// proporciona un stub vacio. Sirve para poder enlazar la prueba SIN arrastrar todo el cliente.
namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
}


static int g_fallos = 0;
static int g_total = 0;

static void Check(const char* nombre, bool ok, const std::string& detalle = std::string()) {
    ++g_total;
    if (ok) {
        std::printf("    [OK]    %s\n", nombre);
    } else {
        ++g_fallos;
        std::printf("    [FALLO] %s%s%s\n", nombre, detalle.empty() ? "" : "  -> ", detalle.c_str());
    }
}

// Rellena una imagen plana del color pedido en los dos formatos, para poder compararlos.
struct Imagen {
    int w = 64, h = 48;
    std::vector<uint8_t> y, u, v, uv;   // YUV420P usa y/u/v ; NV12 usa y + uv intercalado
    std::vector<uint8_t> bgra420, bgraNv12;

    void Construir(uint8_t Y, uint8_t U, uint8_t V) {
        const int cw = w / 2, ch = h / 2;
        y.assign(static_cast<size_t>(w) * h, Y);
        u.assign(static_cast<size_t>(cw) * ch, U);
        v.assign(static_cast<size_t>(cw) * ch, V);
        uv.assign(static_cast<size_t>(cw) * ch * 2, 0);
        for (size_t i = 0; i < uv.size(); i += 2) { uv[i] = U; uv[i + 1] = V; }
        bgra420.assign(static_cast<size_t>(w) * h * 4, 0);
        bgraNv12.assign(static_cast<size_t>(w) * h * 4, 0);
    }

    void Convertir(bool fullRange) {
        ConvertYUV420PToBGRA_BT709(bgra420.data(), w * 4, w, h,
                                   y.data(), w, u.data(), w / 2, v.data(), w / 2,
                                   STORE_UNALIGNED, fullRange);
        ConvertNV12ToBGRA_BT709(bgraNv12.data(), w * 4, w, h,
                                y.data(), w, uv.data(), w,
                                STORE_UNALIGNED, fullRange);
    }
};

static void PixelEn(const std::vector<uint8_t>& buf, int w, int x, int y, int& b, int& g, int& r, int& a) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
    b = buf[i]; g = buf[i + 1]; r = buf[i + 2]; a = buf[i + 3];
}

int main() {
    std::printf("\n  VERIFICACION DEL CONVERSOR DE COLOR DE LA RUTA DE VIDEO\n\n");

    // --- 1: YUV420P y NV12 coinciden ---
    {
        Imagen img;
        img.Construir(126, 128, 128);
        img.Convertir(false);
        const bool iguales = std::memcmp(img.bgra420.data(), img.bgraNv12.data(), img.bgra420.size()) == 0;
        std::string det;
        if (!iguales) {
            int b1, g1, r1, a1, b2, g2, r2, a2;
            PixelEn(img.bgra420, img.w, 0, 0, b1, g1, r1, a1);
            PixelEn(img.bgraNv12, img.w, 0, 0, b2, g2, r2, a2);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "YUV420P=(%d,%d,%d) vs NV12=(%d,%d,%d)", r1, g1, b1, r2, g2, b2);
            det = buf;
        }
        Check("YUV420P y NV12 dan el MISMO resultado para el mismo color", iguales, det);
    }

    // --- 2: negro y blanco en rango completo ---
    {
        Imagen negro, blanco;
        negro.Construir(0, 128, 128);
        negro.Convertir(true);          // rango completo: Y=0 es negro puro
        blanco.Construir(255, 128, 128);
        blanco.Convertir(true);         // Y=255 es blanco puro

        int b, g, r, a;
        PixelEn(negro.bgra420, negro.w, 0, 0, b, g, r, a);
        Check("Y=0 (rango completo) da NEGRO", r <= 4 && g <= 4 && b <= 4,
              "r=" + std::to_string(r) + " g=" + std::to_string(g) + " b=" + std::to_string(b));

        PixelEn(blanco.bgra420, blanco.w, 0, 0, b, g, r, a);
        Check("Y=255 (rango completo) da BLANCO", r >= 251 && g >= 251 && b >= 251,
              "r=" + std::to_string(r) + " g=" + std::to_string(g) + " b=" + std::to_string(b));
    }

    // --- 3: el gris neutro NO debe salir tintado (caza un error de matriz) ---
    {
        Imagen img;
        img.Construir(128, 128, 128);
        img.Convertir(true);
        int b, g, r, a;
        PixelEn(img.bgra420, img.w, 0, 0, b, g, r, a);
        const int maxDif = std::max(std::abs(r - g), std::max(std::abs(g - b), std::abs(r - b)));
        Check("U=V=128 da GRIS NEUTRO (R=G=B), sin tinte", maxDif <= 2,
              "r=" + std::to_string(r) + " g=" + std::to_string(g) + " b=" + std::to_string(b) +
              " dif_max=" + std::to_string(maxDif));
    }

    // --- 4: el gris de video (Y=126, rango limitado) ---
    {
        Imagen img;
        img.Construir(126, 128, 128);
        img.Convertir(false);           // rango limitado 16-235
        int b, g, r, a;
        PixelEn(img.bgra420, img.w, 0, 0, b, g, r, a);
        // Y=126 en 16-235 es el punto medio: debe dar un gris medio, ni negro ni blanco.
        const bool grisMedio = (r >= 100 && r <= 160 && g >= 100 && g <= 160 && b >= 100 && b <= 160);
        Check("Y=126 en rango limitado da un gris MEDIO", grisMedio,
              "r=" + std::to_string(r) + " g=" + std::to_string(g) + " b=" + std::to_string(b));
    }

    // --- 5: el alfa queda OPACO (si fuera 0, el video no se veria) ---
    {
        Imagen img;
        img.Construir(200, 90, 200);
        img.Convertir(true);
        int b, g, r, a;
        PixelEn(img.bgra420, img.w, 0, 0, b, g, r, a);
        Check("el canal alfa queda a 255 (opaco)", a == 255, "a=" + std::to_string(a));
    }

    // --- 6: los colores primarios apuntan al canal correcto (U/V no invertidos) ---
    {
        // U alta y V baja tira a AZUL; U baja y V alta tira a ROJO. Si U y V estuvieran invertidos,
        // esta comprobacion los detecta: el azul saldria rojo.
        Imagen azul, rojo;
        azul.Construir(80, 200, 60);
        azul.Convertir(true);
        rojo.Construir(80, 60, 200);
        rojo.Convertir(true);
        int b1, g1, r1, a1, b2, g2, r2, a2;
        PixelEn(azul.bgra420, azul.w, 0, 0, b1, g1, r1, a1);
        PixelEn(rojo.bgra420, rojo.w, 0, 0, b2, g2, r2, a2);
        Check("U alta / V baja da AZUL (B > R)", b1 > r1,
              "r=" + std::to_string(r1) + " b=" + std::to_string(b1));
        Check("U baja / V alta da ROJO (R > B)", r2 > b2,
              "r=" + std::to_string(r2) + " b=" + std::to_string(b2));
    }

    std::printf("\n");
    if (g_fallos == 0) {
        std::printf("    Todas las comprobaciones del conversor de color pasaron\n\n");
        return 0;
    }
    std::printf("    FALLARON %d de %d comprobaciones del conversor de color\n\n", g_fallos, g_total);
    return 1;
}
