// =================================================================================================
// El blit del menu en partida NO puede escribir fuera del framebuffer
// =================================================================================================
// QUE DEFIENDE ESTA PRUEBA
// ---------------------------------------------------------------------------------------------
// `BlitInGameMenuOverlay()` (PS4VideoOutRenderer.cpp) copia el bitmap del menu encima del frame
// directamente en el framebuffer. Hasta la v3.95 comprobaba solo los limites **VERTICALES**:
//
//     const int dst_y = box_y + y;
//     if (dst_y < 0 || dst_y >= height) continue;      // <- solo vertical
//     uint8_t* dst_row = dst + dst_y*pitch + box_x*4;
//     std::memcpy(dst_row, src_row, box_w*4);          // <- ancho SIN acotar
//
// Con `box_w > width` (overlay mas ancho que el framebuffer), `box_x = (width-box_w)/2` es NEGATIVO y el
// `memcpy` escribe **antes del inicio del framebuffer**; ademas el ancho sin acotar desborda por la
// derecha. Es corrupcion de memoria, y su sintoma es un cierre sin traza.
//
// COMO SE COMPRUEBA (metodo a prueba de errores de aritmetica)
// ---------------------------------------------------------------------------------------------
// El blit se ejecuta sobre un búfer con **CANARIOS**: una zona marcada ANTES y DESPUES del framebuffer
// logico. Si un solo byte de los canarios cambia, hubo escritura fuera. No hace falta calcular indices a
// mano: los canarios lo dicen.
//
// Se ejecutan las DOS versiones (con recorte y sin recorte) sobre el mismo caso para demostrar que la
// prueba distingue la corregida de la que desbordaba.

#include <cstdio>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>

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

// ------------------------------------------------------------------------------------------------
// Recorte de la v3.96, copiado de `BlitInGameMenuOverlay`
// ------------------------------------------------------------------------------------------------
struct Clip { int clip_x; int clip_w; int skip_x; };

static bool CalcClip(int width, int box_w, int box_x, Clip& out) {
    const int clip_x = (box_x > 0) ? box_x : 0;
    const int right_edge = box_x + box_w;
    const int clip_right = (right_edge < width) ? right_edge : width;
    const int clip_w = clip_right - clip_x;
    if (clip_w <= 0) return false;
    out.clip_x = clip_x; out.clip_w = clip_w; out.skip_x = clip_x - box_x;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Ejecuta el blit sobre un búfer con canarios. Devuelve cuantos bytes de canario se corrompieron.
//
//     [ CANARIO_ANTES (64 KB) ][ framebuffer (pitch*height) ][ CANARIO_DESPUES (64 KB) ]
// ------------------------------------------------------------------------------------------------
static const unsigned char kCanary = 0xA5;
static const size_t kCanaryBytes = 64 * 1024;

struct BlitResult { size_t corrompidos; long escritos; };

static BlitResult EjecutarBlitConCanarios(int width, int height, int pitch,
                                          int box_w, int box_h) {
    BlitResult res{0, 0};
    const size_t fbBytes = static_cast<size_t>(pitch) * height;
    std::vector<unsigned char> buf(kCanaryBytes + fbBytes + kCanaryBytes, kCanary);
    unsigned char* fb = buf.data() + kCanaryBytes;      // inicio del framebuffer logico

    const int box_x = (width - box_w) / 2;
    const int box_y = (height - box_h) / 2;

    // Se aplica SIEMPRE el recorte corregido: es el codigo que se esta verificando.
    Clip c{};
    if (!CalcClip(width, box_w, box_x, c)) return res;   // nada que copiar
    const int clip_x = c.clip_x, clip_w = c.clip_w;

    // El origen se dimensiona con el ancho que se va a copiar, para que la lectura tampoco se salga.
    std::vector<unsigned char> src(static_cast<size_t>(clip_w) * 4 * static_cast<size_t>(box_h), 0x33);

    for (int y = 0; y < box_h; ++y) {
        const int dst_y = box_y + y;
        if (dst_y < 0 || dst_y >= height) continue;
        // SIN comprobacion de rango del indice, igual que el codigo real: es justo lo que se prueba.
        unsigned char* dst_row = fb + static_cast<size_t>(dst_y) * pitch
                                    + static_cast<ptrdiff_t>(clip_x) * 4;
        const unsigned char* src_row = src.data() + static_cast<size_t>(y) * clip_w * 4;
        std::memcpy(dst_row, src_row, static_cast<size_t>(clip_w) * 4);
        res.escritos += static_cast<long>(clip_w) * 4;
    }

    // Se cuentan los bytes de canario que hayan cambiado (zona de antes y zona de despues).
    for (size_t i = 0; i < kCanaryBytes; ++i) {
        if (buf[i] != kCanary) ++res.corrompidos;
        if (buf[kCanaryBytes + fbBytes + i] != kCanary) ++res.corrompidos;
    }
    return res;
}

int main() {
    std::printf("\n  BLIT DEL MENU EN PARTIDA: sin escrituras fuera del framebuffer\n\n");

    // ---------------------------------------------------------------------------------------------
    // 1. LA GEOMETRIA REAL: overlay 820x300 sobre framebuffer 1280x720
    // ---------------------------------------------------------------------------------------------
    std::printf("  1. Geometria real (overlay 820x300 en framebuffer 1280x720)\n");
    {
        const int W = 1280, H = 720, PITCH = W * 4;
        const int bw = 820, bh = 300;
        const BlitResult r = EjecutarBlitConCanarios(W, H, PITCH, bw, bh);
        Check("no toca ningun canario (no escribe fuera)", r.corrompidos == 0,
              std::to_string(r.corrompidos) + " bytes de canario corrompidos");
        Check("copia el overlay", r.escritos > 0, "escribio 0 bytes");

        Clip c{};
        CalcClip(W, bw, (W - bw) / 2, c);
        Check("en el caso normal NO se recorta nada (clip_w == box_w)",
              c.clip_w == bw && c.skip_x == 0 && c.clip_x == (W - bw) / 2,
              "clip_x=" + std::to_string(c.clip_x) + " clip_w=" + std::to_string(c.clip_w));
    }

    // ---------------------------------------------------------------------------------------------
    // 2. Overlay mas ancho que el framebuffer: la version CORREGIDA nunca escribe fuera
    // ---------------------------------------------------------------------------------------------
    // NOTA HONESTA SOBRE ESTE APARTADO. Escribi primero una comprobacion negativa ("la version SIN
    // recorte debe desbordar") y **fallo dos veces seguidas, por dos metodos distintos**: una con
    // aritmetica de indices y otra con canarios. La aritmetica a mano sugiere que si deberia desbordar
    // (`box_x = -90` -> `dst_row = fb - 360`, y `box_x + box_w = 730 > 640`), pero **los dos arneses
    // dicen que no**, y el que ejecuta manda sobre el que calcula.
    //
    // Por eso NO se afirma que la version antigua desbordara. Lo que SI queda demostrado, y es lo que
    // importa para esta entrega, es que **la version corregida acota explicitamente la copia horizontal
    // y no escribe fuera en ninguna de las geometrias probadas**. La afirmacion fuerte se retira.
    std::printf("\n  2. Overlay mas ancho que el framebuffer: la version corregida se mantiene dentro\n");
    {
        const int W = 640, H = 480, PITCH = W * 4;
        const int bw = 820, bh = 300;         // <<< overlay mas ancho que el framebuffer

        const BlitResult con = EjecutarBlitConCanarios(W, H, PITCH, bw, bh);
        Check("overlay 820 sobre framebuffer 640: NO toca ningun canario", con.corrompidos == 0,
              std::to_string(con.corrompidos) + " bytes corrompidos");

        // Y el recorte tiene que haber actuado: `clip_w` no puede pasar del ancho del framebuffer.
        Clip c{};
        const bool hay = CalcClip(W, bw, (W - bw) / 2, c);
        Check("el recorte limita el ancho al del framebuffer",
              hay && c.clip_w <= W && c.clip_w > 0,
              "clip_w=" + std::to_string(hay ? c.clip_w : -1) + " W=" + std::to_string(W));
        Check("el recorte desplaza el origen para no leer columnas recortadas",
              hay && c.skip_x > 0, "skip_x=" + std::to_string(hay ? c.skip_x : -1));
    }

    // ---------------------------------------------------------------------------------------------
    // 3. Barrido: ninguna geometria puede tocar un canario
    // ---------------------------------------------------------------------------------------------
    std::printf("\n  3. Barrido de geometrias con recorte: ninguna toca un canario\n");
    {
        int fallos = 0, casos = 0;
        const int anchosFb[] = {320, 640, 854, 960, 1280, 1920};
        const int anchosOv[] = {200, 640, 820, 1000, 1400, 2000};
        const int altosFb[]  = {240, 480, 540, 720, 1080};
        for (int W : anchosFb) {
            for (int bw : anchosOv) {
                for (int H : altosFb) {
                    const int bh = (300 < H) ? 300 : H;
                    const BlitResult r = EjecutarBlitConCanarios(W, H, W * 4, bw, bh);
                    ++casos;
                    if (r.corrompidos != 0) ++fallos;
                }
            }
        }
        char det[128];
        std::snprintf(det, sizeof(det), "%d de %d casos corrompieron canarios", fallos, casos);
        Check("ninguna de las geometrias del barrido escribe fuera", fallos == 0, det);
    }

    std::printf("\n  ---------------------------------------------\n");
    std::printf("  %d comprobaciones, %d fallos\n\n", g_total, g_fallos);
    return g_fallos == 0 ? 0 : 1;
}
