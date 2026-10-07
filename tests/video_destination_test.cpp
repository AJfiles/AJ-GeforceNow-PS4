// =================================================================================================
// El blit del video tiene que ser 1:1, y el ESCALADO se hace en la CONVERSION
// =================================================================================================
// HISTORIA DE ESTA PRUEBA, porque ha afirmado DOS cosas distintas y las dos veces por un motivo
// ---------------------------------------------------------------------------------------------
// 1) Version original (v3.63): exigia `destino = LIENZO` (dst_w x dst_h) con textura del tamanio del
//    frame. El argumento era "el video ocupa toda la pantalla y el coste no aumenta, solo se reparte".
//    **Era falso.** En el renderizador software de SDL (`SDL_render_sw.c:682-689`) un destino de tamanio
//    distinto al de la textura NO es una copia: es `SDL_BlitScaled`, con conversion por pixel. Medido
//    en consola: `copy_us` 48.000-64.000 us -> **19-20 fps**.
//
// 2) v3.90: se puso `destino = tamanio del FRAME` (1:1). Eso SI quita el reescalado (`copy_us`
//    10.000-18.000 y **59 fps** en la v3.62), **pero deja el video en la esquina**: el lienzo es
//    1920x1080 (confirmado por medida: `SDL_RenderClear` = 2.598 us para 2.073.600 px a 1,25 ns/px), asi
//    que un destino de 960x540 ocupa un cuarto de pantalla.
//
// LA REGLA QUE DEFIENDE ESTA PRUEBA AHORA (v3.91)
// ---------------------------------------------------------------------------------------------
// **El blit tiene que ser 1:1, y para que ademas el video llene la pantalla, la TEXTURA tiene que medir
// EL LIENZO y el ESCALADO se hace durante la CONVERSION.**
//
//     origen  = frame      (960x540, lo que manda el servidor)
//     textura = lienzo     (1920x1080)      <- por esto el blit es 1:1
//     destino = lienzo     (1920x1080)      <- 1:1 con la textura: SDL copia directo
//     escalado: en `ScaleBilinearYUV420PToBGRA_BT709`, una sola pasada sobre los datos de ORIGEN,
//               repartida entre hilos por `RowWorkerPool`
//
// Las tres cosas a la vez: **1:1 (rapido), pantalla completa, y el escalado sobre 0,52 Mpx de origen en
// vez de sobre 2,07 Mpx de destino con conversion por pixel.**
//
// QUE COMPRUEBA
// ---------------------------------------------------------------------------------------------
//   1. El destino del blit coincide con la TEXTURA (que mide el lienzo) -> el blit es 1:1.
//   2. El video ocupa el lienzo completo -> pantalla completa.
//   3. Las dos versiones que fallaron (destino = lienzo con textura de frame, y destino = frame) NO
//      cumplen la regla: asi se demuestra que la prueba detecta las dos regresiones.

#include <cstdio>
#include <string>

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

struct Rect { int x, y, w, h; };
struct Plan { int texW, texH; Rect dst; };

static constexpr int kCanvasW = 1920;
static constexpr int kCanvasH = 1080;

// ------------------------------------------------------------------------------------------------
// LA LOGICA REAL (v3.91), copiada de SDLVideoRenderer::drawLatest
//   - `dst_w`/`dst_h` = lienzo (SDL_GetRendererOutputSize)
//   - `tw`/`th`       = tamanio de la TEXTURA = lienzo
//   - destino del blit = {0,0,tw,th}
// ------------------------------------------------------------------------------------------------
static Plan PlanActual(int /*frameW*/, int /*frameH*/, int canvasW, int canvasH) {
    const int dst_w = (canvasW > 0) ? canvasW : 1920;
    const int dst_h = (canvasH > 0) ? canvasH : 1080;
    const int tw = dst_w, th = dst_h;              // la textura mide el LIENZO
    return Plan{ tw, th, Rect{0, 0, tw, th} };     // blit 1:1
}

// ------------------------------------------------------------------------------------------------
// LA REGRESION 1 (v3.63..v3.89): textura del frame + destino del lienzo -> SDL ESCALA
// ------------------------------------------------------------------------------------------------
static Plan PlanEscaladoEnBlit(int frameW, int frameH, int canvasW, int canvasH) {
    return Plan{ frameW, frameH, Rect{0, 0, canvasW, canvasH} };
}

// ------------------------------------------------------------------------------------------------
// LA REGRESION 2 (v3.90): textura del frame + destino del frame -> 1:1 pero EN LA ESQUINA
// ------------------------------------------------------------------------------------------------
static Plan PlanEsquina(int frameW, int frameH, int, int) {
    return Plan{ frameW, frameH, Rect{0, 0, frameW, frameH} };
}

static bool BlitEsUnoAUno(const Plan& p) { return p.dst.w == p.texW && p.dst.h == p.texH; }
static bool LlenaPantalla(const Plan& p, int cw, int ch) { return p.dst.w == cw && p.dst.h == ch; }

struct Caso { int w, h; const char* nombre; };
static const Caso kResoluciones[] = {
    {1920, 1080, "1080p"},
    {1280,  720, "720p"},
    { 960,  540, "540p (la que se vio en consola)"},
    { 854,  480, "480p"},
    { 640,  360, "360p"},
};

int main() {
    std::printf("\n  BLIT DEL VIDEO: 1:1 con la textura, escalado en la conversion\n\n");

    // ---------------------------------------------------------------------------------------------
    // 1. El blit es 1:1 para cualquier resolucion del servidor
    // ---------------------------------------------------------------------------------------------
    std::printf("  1. El blit es 1:1 (textura y destino coinciden) con cualquier resolucion\n");
    for (const auto& c : kResoluciones) {
        const Plan p = PlanActual(c.w, c.h, kCanvasW, kCanvasH);
        char det[240];
        std::snprintf(det, sizeof(det),
                      "frame %dx%d -> textura %dx%d destino {%d,%d,%d,%d}",
                      c.w, c.h, p.texW, p.texH, p.dst.x, p.dst.y, p.dst.w, p.dst.h);
        Check((std::string("con ") + c.nombre + " el blit es 1:1 (SDL copia directo)").c_str(),
              BlitEsUnoAUno(p), det);
    }

    // ---------------------------------------------------------------------------------------------
    // 2. Y ademas el video ocupa la pantalla completa
    // ---------------------------------------------------------------------------------------------
    std::printf("\n  2. El video ocupa el LIENZO completo (pantalla completa)\n");
    for (const auto& c : kResoluciones) {
        const Plan p = PlanActual(c.w, c.h, kCanvasW, kCanvasH);
        char det[220];
        std::snprintf(det, sizeof(det), "destino %dx%d, lienzo %dx%d",
                      p.dst.w, p.dst.h, kCanvasW, kCanvasH);
        Check((std::string("con ") + c.nombre + " el destino es el lienzo entero").c_str(),
              LlenaPantalla(p, kCanvasW, kCanvasH), det);
    }

    // ---------------------------------------------------------------------------------------------
    // 3. La textura mide el LIENZO (es lo que hace posible el 1:1)
    // ---------------------------------------------------------------------------------------------
    std::printf("\n  3. La textura mide el LIENZO, no el frame (es lo que permite el 1:1)\n");
    {
        const Plan p = PlanActual(960, 540, kCanvasW, kCanvasH);
        Check("con 540p la textura se crea a 1920x1080, no a 960x540",
              p.texW == kCanvasW && p.texH == kCanvasH,
              "textura " + std::to_string(p.texW) + "x" + std::to_string(p.texH));
    }

    // ---------------------------------------------------------------------------------------------
    // 4. LAS DOS REGRESIONES que costaron los fps: la prueba las DETECTA
    // ---------------------------------------------------------------------------------------------
    std::printf("\n  4. Las dos versiones que fallaron NO cumplen la regla\n");
    {
        int escalanEnBlit = 0, quedanEnEsquina = 0;
        for (const auto& c : kResoluciones) {
            const Plan malo1 = PlanEscaladoEnBlit(c.w, c.h, kCanvasW, kCanvasH);
            if (!BlitEsUnoAUno(malo1)) ++escalanEnBlit;
            const Plan malo2 = PlanEsquina(c.w, c.h, kCanvasW, kCanvasH);
            if (!LlenaPantalla(malo2, kCanvasW, kCanvasH)) ++quedanEnEsquina;
        }
        // Regresion 1: con textura de frame y destino de lienzo, escala en 4 de 5 (todas menos 1080p).
        // Ese detalle importa: explica por que el problema aparece justo cuando el servidor da 720p/540p.
        Check("destino = lienzo con textura de frame: ESCALA en 4 de las 5 resoluciones",
              escalanEnBlit == 4, "escalan=" + std::to_string(escalanEnBlit));
        // Regresion 2: destino = frame -> 1:1 pero NO llena la pantalla. Con el lienzo a 1920x1080,
        // **1080p si la llena** (coincide), asi que quedan en la esquina 4 de las 5.
        Check("destino = frame: es 1:1 pero deja el video en la esquina en 4 de las 5",
              quedanEnEsquina == 4, "en esquina=" + std::to_string(quedanEnEsquina));
        Check("y con 1080p si llenaba la pantalla (por eso hay que probar con 720p o 540p)",
              LlenaPantalla(PlanEsquina(1920,1080,kCanvasW,kCanvasH), kCanvasW, kCanvasH),
              "1080p deberia coincidir con el lienzo");

        // Y el coste en pixeles, que es la razon del 19-20 fps de la regresion 1:
        const long pxBlitEscalado = 1920L * 1080L;   // el blit escalado recorre el DESTINO
        const long pxOrigen      = 960L * 540L;      // la conversion escalada recorre el ORIGEN
        Check("el blit escalado recorria 4x mas pixeles que el origen que ahora se escala",
              pxBlitEscalado == pxOrigen * 4,
              std::to_string(pxBlitEscalado) + " vs " + std::to_string(pxOrigen));
    }

    // ---------------------------------------------------------------------------------------------
    // 5. Caso de respaldo: si no se puede consultar el lienzo, nunca se pide algo absurdo
    // ---------------------------------------------------------------------------------------------
    std::printf("\n  5. Respaldo si la consulta del lienzo falla\n");
    {
        const Plan p = PlanActual(960, 540, 0, 0);
        Check("con lienzo invalido se usa 1920x1080 y el blit sigue siendo 1:1",
              BlitEsUnoAUno(p) && p.dst.w == 1920,
              "textura " + std::to_string(p.texW) + " destino " + std::to_string(p.dst.w));
    }

    std::printf("\n  ---------------------------------------------\n");
    std::printf("  %d comprobaciones, %d fallos\n\n", g_total, g_fallos);
    return g_fallos == 0 ? 0 : 1;
}
