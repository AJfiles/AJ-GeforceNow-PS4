// =================================================================================================
// SONDA DEL ESCALADOR DE HARDWARE DE VideoOut (v3.56)
// =================================================================================================
// POR QUE EXISTE ESTA SONDA
// ---------------------------------------------------------------------------------------------
// El cliente esta clavado en ~19 fps porque el renderizador SOFTWARE de SDL tiene que:
//
//     memset 8,3 MB  +  memcpy 8,3 MB   (SDL_ps4video.c:504 y :519)
//
// **tres veces por frame**, mas el reescalado. Con un display de 1080p y un stream de 720p, no hay
// forma de evitarlo mientras SDL sea el dueno del framebuffer.
//
// LA IDEA DEL ESCALADOR DE HARDWARE: si el framebuffer que se registra en VideoOut es de 960x540 (o
// de 1280x720) en lugar de 1920x1080, **la GPU de la PS4 escala a la resolucion del display**. El
// trabajo de escalado sale de la CPU por completo.
//
// ---------------------------------------------------------------------------------------------
// QUE IMPIDE HACERLO EN EL CLIENTE HOY
// ---------------------------------------------------------------------------------------------
// El driver de SDL fija el tamano del framebuffer de VideoOut al del DISPLAY, y **ignora lo que le
// diga la aplicacion**:
//
//     // build/research/SDL-PS4/src/video/ps4/SDL_ps4video.c:212-223
//     if (0 == sceVideoOutGetResolutionStatus(handle, &res)) {
//         VData->width  = res.fullWidth;      // <-- SIEMPRE la resolucion del display
//         VData->height = res.fullHeight;
//     }
//     VData->attr.width = VData->width;
//
// Asi que la unica via es el camino de VideoOut DIRECTO (PS4VideoOutRenderer). Pero **ese camino
// tampoco deja de escalar**: su propio `needs_scaling` hace una conversion bilineal en CPU
// (`VIDEOOUT_SCALE_BILINEAR_US avg=15496`), porque el framebuffer que registra es del tamano del
// stream, no de la salida.
//
// ---------------------------------------------------------------------------------------------
// LA PREGUNTA QUE ESTA SONDA RESPONDE, Y POR QUE NO SE PUEDE RESPONDER LEYENDO CODIGO
// ---------------------------------------------------------------------------------------------
//     **Si se registra un framebuffer de 960x540, ¿lo escala la GPU a 1080p en pantalla?**
//
// No se puede deducir del codigo: depende del comportamiento de VideoOut en ESTE firmware con
// GoldHEN. Hay dos desenlaces posibles y son muy distintos:
//
//   (a) El framebuffer se presenta ESCALADO a pantalla completa -> el escalador de hardware existe y
//       funciona. Es la via para que el cliente presente 720p con coste de CPU casi nulo.
//   (b) Se presenta en una ventana de 960x540 en una esquina (o no se presenta) -> no hay escalado
//       automatico y habria que llamar a `sceVideoOutSysUpdateScalerParameters()`, cuya firma NO
//       conocemos (en este SDK las 37 funciones `Sys*` estan declaradas sin parametros).
//
// SABER CUAL DE LAS DOS ES **CAMBIA POR COMPLETO** lo que hay que hacer despues.
//
// ---------------------------------------------------------------------------------------------
// QUE DIBUJA
// ---------------------------------------------------------------------------------------------
// Un patron inequivoco pensado para que se vea de un vistazo si esta escalado:
//
//   - CUATRO ESQUINAS de un color distinto cada una. Si el framebuffer se presentara sin escalar en
//     una esquina de la pantalla, las esquinas se verian todas juntas y pequenas.
//   - UN BORDE EXTERIOR de 8 px. Si esta escalado, el borde toca los cuatro limites de la pantalla.
//   - UNAS BARRAS VERTICALES de 12 franjas de colores. Al escalar 2x desde 540p, cada barra se ve
//     el doble de ancha; sirve para confirmar que el escalado es uniforme en todo el ancho.
//   - UN CUADRADO CENTRAL blanco con marco negro, para ver si el escalado respeta la geometria.
//
// COSTE: reserva 2 buffers de 960x540x4 = 4,1 MB, pinta en CPU y hace flip a 60 Hz. No toca el
// cliente ni comparte estado con el.
//
// LOG: escribe a `/data/aj_videoout_scaler_probe.log`, abriendo y cerrando en cada linea, para que un
// cierre duro deje la ultima escrita. Mismo criterio que las sondas de Piglet.
// =================================================================================================

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>

// -------------------------------------------------------------------------------------------------
// TAMANO DEL FRAMEBUFFER QUE SE VA A PROBAR
// -------------------------------------------------------------------------------------------------
// 960x540 (la resolucion que el servidor de GFN entrega en muchas sesiones). Si el escalador de
// hardware existe, la GPU lo estira a los 1920x1080 del panel.
#define FB_WIDTH   960
#define FB_HEIGHT  540

// Numero de buffers. Con 2 basta para alternar sin esperar al flip anterior; mas buffers solo
// consumen memoria.
#define FB_COUNT   2

// Alineacion exigida por sceKernelAllocateDirectMemory. La misma que usa el cliente.
#define DMEM_ALIGN (2u * 1024u * 1024u)

#define PROBE_LOG "/data/aj_videoout_scaler_probe.log"

static void probe_log(const char* message) {
    sceKernelDebugOutText(0, message);
    sceKernelDebugOutText(0, "\n");
    int fd = open(PROBE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char line[256];
        const int n = snprintf(line, sizeof(line), "%s\n", message);
        if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
        fsync(fd);
        close(fd);
    }
}

static uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
    // El formato registrado es A8B8G8R8_SRGB, es decir en memoria BGRA (little-endian ARGB).
    return ((uint32_t)255u << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Pinta el patron de prueba en un framebuffer BGRA.
static void paint(uint32_t* fb, int width, int height, int phase) {
    static const uint32_t kBar[12] = {
        0xFFFFFFFFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFF00FF00u,
        0xFFFF00FFu, 0xFFFF0000u, 0xFF0000FFu, 0xFF000000u,
        0xFF808080u, 0xFF008080u, 0xFF800080u, 0xFF808000u
    };

    // Fondo: barras verticales. 12 franjas repartidas en todo el ancho.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int idx = (x * 12) / width;
            fb[(size_t)y * width + x] = kBar[idx];
        }
    }

    // Borde exterior de 8 px en BLANCO: si esta escalado, toca los cuatro limites de la pantalla.
    for (int i = 0; i < 8; ++i) {
        for (int x = 0; x < width; ++x) {
            fb[(size_t)i * width + x] = 0xFFFFFFFFu;
            fb[(size_t)(height - 1 - i) * width + x] = 0xFFFFFFFFu;
        }
        for (int y = 0; y < height; ++y) {
            fb[(size_t)y * width + i] = 0xFFFFFFFFu;
            fb[(size_t)y * width + (width - 1 - i)] = 0xFFFFFFFFu;
        }
    }

    // Cuatro esquinas de color distinto, 40x40, DENTRO del borde.
    const int cs = 40;
    const uint32_t corners[4] = { 0xFF0000FFu /*rojo*/, 0xFF00FF00u /*verde*/,
                                  0xFFFF0000u /*azul*/, 0xFF00FFFFu /*amarillo*/ };
    const int cx[4] = { 16, width - 16 - cs, 16, width - 16 - cs };
    const int cy[4] = { 16, 16, height - 16 - cs, height - 16 - cs };
    for (int c = 0; c < 4; ++c) {
        for (int y = 0; y < cs; ++y)
            for (int x = 0; x < cs; ++x)
                fb[(size_t)(cy[c] + y) * width + (cx[c] + x)] = corners[c];
    }

    // Cuadrado central blanco con marco negro, que se mueve un poco con `phase` para que se vea que
    // la presentacion esta viva y no es una imagen congelada.
    const int qs = 120;
    const int off = (phase % 20) - 10;
    const int qx = (width - qs) / 2 + off;
    const int qy = (height - qs) / 2;
    for (int y = -4; y < qs + 4; ++y) {
        for (int x = -4; x < qs + 4; ++x) {
            const int px = qx + x, py = qy + y;
            if (px < 0 || py < 0 || px >= width || py >= height) continue;
            const int inside = (x >= 0 && y >= 0 && x < qs && y < qs);
            fb[(size_t)py * width + px] = inside ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }
}

int main(void) {
    probe_log("===== SONDA DEL ESCALADOR DE HARDWARE DE VideoOut =====");

    char info[192];
    snprintf(info, sizeof(info),
             "framebuffer a probar: %dx%d, %d buffers, formato A8B8G8R8_SRGB",
             FB_WIDTH, FB_HEIGHT, FB_COUNT);
    probe_log(info);

    // --- 1. Cargar el modulo interno de VideoOut ---------------------------------------------
    //
    // =============================================================================================
    // BUG ENCONTRADO Y CORREGIDO (v4.01): EL ID DEL MODULO ESTABA INVENTADO.
    // =============================================================================================
    // La sonda usaba el literal **0x80000004**. El ID correcto, segun la cabecera del propio SDK
    // (`tools/openorbis/.../include/orbis/sysmodule.h`), es:
    //
    //     ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT = 0x80000022, // libSceVideoOut
    //
    // Con un ID invalido, `sceSysmoduleLoadModuleInternal` devuelve error, la sonda lo registra y
    // **sale con `return 1` antes de llegar a registrar ningun framebuffer**. Es decir: **nunca podia
    // responder a la pregunta del escalador**, que es justo para lo que existe.
    //
    // El cliente (`PS4VideoOutRenderer.cpp:828`) usa la constante **por nombre**, que es lo correcto y
    // lo que se hace aqui ahora. Si por lo que fuera la cabecera no la declarase, se cae al valor
    // numerico verificado.
#if defined(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT)
    const int kVideoOutModuleId = ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT;
    const char* kVideoOutModuleSrc = "ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT (cabecera del SDK)";
#else
    const int kVideoOutModuleId = 0x80000022;   // valor verificado en sysmodule.h
    const char* kVideoOutModuleSrc = "0x80000022 (literal verificado en sysmodule.h)";
#endif
    probe_log(kVideoOutModuleSrc);
    const int mod = sceSysmoduleLoadModuleInternal(kVideoOutModuleId);
    snprintf(info, sizeof(info), "STEP 1/7 sceSysmoduleLoadModuleInternal(0x%08X) rc=0x%08X",
             (unsigned)kVideoOutModuleId, (unsigned)mod);
    probe_log(info);
    // 0x805A1000 significa "ya cargado", que tambien vale.
    if (mod < 0 && (unsigned)mod != 0x805A1000u) {
        probe_log("STEP 1/7 FALLO: no se pudo cargar el modulo de VideoOut");
        return 1;
    }

    // --- 2. Abrir el bus principal -----------------------------------------------------------
    const int32_t handle = sceVideoOutOpen(
        ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    snprintf(info, sizeof(info), "STEP 2/7 sceVideoOutOpen rc=0x%08X", (unsigned)handle);
    probe_log(info);
    if (handle <= 0) { probe_log("STEP 2/7 FALLO: sin handle de VideoOut"); return 1; }

    // --- 3. Que resolucion tiene el display? -------------------------------------------------
    OrbisVideoOutResolutionStatus res;
    memset(&res, 0, sizeof(res));
    const int32_t resRc = sceVideoOutGetResolutionStatus(handle, &res);
    snprintf(info, sizeof(info),
             "STEP 3/7 GetResolutionStatus rc=0x%08X display=%ux%u pane=%ux%u refresh=%llu",
             (unsigned)resRc, res.width, res.height, res.paneWidth, res.paneHeight,
             (unsigned long long)res.refreshRate);
    probe_log(info);
    if (res.height > 0 && res.height <= FB_HEIGHT) {
        probe_log("AVISO: el display no es mas grande que el framebuffer; el escalado no se notaria");
    }

    // --- 4. Memoria directa para los framebuffers --------------------------------------------
    const size_t frameBytes = (size_t)FB_WIDTH * FB_HEIGHT * 4u;
    const size_t needBytes  = frameBytes * FB_COUNT;
    const size_t totalBytes = (needBytes + DMEM_ALIGN - 1u) & ~(DMEM_ALIGN - 1u);

    off_t dmemOffset = 0;
    const size_t maxDmem = sceKernelGetDirectMemorySize();
    int rc = sceKernelAllocateDirectMemory(0, maxDmem, totalBytes, DMEM_ALIGN,
                                           3 /* ORBIS_KERNEL_WC_GARLIC */, &dmemOffset);
    snprintf(info, sizeof(info),
             "STEP 4/7 sceKernelAllocateDirectMemory(%zu bytes) rc=0x%08X offset=%lld",
             totalBytes, (unsigned)rc, (long long)dmemOffset);
    probe_log(info);
    if (rc < 0) { probe_log("STEP 4/7 FALLO: sin memoria directa"); sceVideoOutClose(handle); return 1; }

    void* cpuAddr = NULL;
    rc = sceKernelMapDirectMemory(&cpuAddr, totalBytes, 0x33 /* RW */, 0, dmemOffset, DMEM_ALIGN);
    snprintf(info, sizeof(info), "STEP 5/7 sceKernelMapDirectMemory rc=0x%08X addr=%p",
             (unsigned)rc, cpuAddr);
    probe_log(info);
    if (rc < 0 || !cpuAddr) {
        probe_log("STEP 5/7 FALLO: no se pudo mapear la memoria directa");
        sceKernelReleaseDirectMemory(dmemOffset, totalBytes);
        sceVideoOutClose(handle);
        return 1;
    }

    // --- 6. Registrar los buffers CON EL TAMANO PEQUENO --------------------------------------
    // ESTE ES EL PUNTO CLAVE DE TODA LA SONDA. Si VideoOut acepta estos buffers de 960x540 y los
    // presenta escalados, el escalador de hardware existe.
    void* addresses[FB_COUNT];
    for (int i = 0; i < FB_COUNT; ++i)
        addresses[i] = (uint8_t*)cpuAddr + (size_t)i * frameBytes;

    OrbisVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr,
                                  0x80000000u /* ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB */,
                                  1 /* ORBIS_VIDEO_OUT_TILING_MODE_LINEAR */,
                                  0 /* ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9 */,
                                  FB_WIDTH, FB_HEIGHT, FB_WIDTH);

    const int regRc = sceVideoOutRegisterBuffers(handle, 0, addresses, FB_COUNT, &attr);
    snprintf(info, sizeof(info),
             "STEP 6/7 sceVideoOutRegisterBuffers(%dx%d) rc=0x%08X",
             FB_WIDTH, FB_HEIGHT, (unsigned)regRc);
    probe_log(info);
    if (regRc < 0) {
        probe_log("STEP 6/7 FALLO: VideoOut RECHAZA un framebuffer de 960x540 -> la via muere aqui");
        // NOTA: `sceKernelUnmapDirectMemory` NO existe en este toolchain (el proyecto solo llama a
        // `sceKernelReleaseDirectMemory`), asi que aqui no se desmapea. La sonda va a terminar de
        // todas formas y el sistema libera la memoria al cerrar el proceso.
        sceKernelReleaseDirectMemory(dmemOffset, totalBytes);
        sceVideoOutClose(handle);
        return 1;
    }
    probe_log("STEP 6/7 OK: VideoOut ACEPTA el framebuffer de 960x540");

    // --- 7. Presentar en bucle ---------------------------------------------------------------
    // Se alterna entre los dos buffers y se hace flip. `sceVideoOutSetFlipRate(handle, 0)` = 60 Hz.
    sceVideoOutSetFlipRate(handle, 0);
    probe_log("STEP 7/7 presentando el patron; MIRA LA PANTALLA");
    probe_log("  - Si el patron ocupa TODA la pantalla y esta estirado -> EL ESCALADOR EXISTE");
    probe_log("  - Si se ve pequeno en una esquina, sin escalar -> NO hay escalado automatico");

    for (int frame = 0;; ++frame) {
        const int idx = frame % FB_COUNT;
        paint((uint32_t*)addresses[idx], FB_WIDTH, FB_HEIGHT, frame);

        const int flipRc = sceVideoOutSubmitFlip(handle, idx, 0 /* VSYNC */, (int64_t)frame);
        if (flipRc < 0 && (frame % 60) == 0) {
            snprintf(info, sizeof(info), "sceVideoOutSubmitFlip rc=0x%08X en frame %d",
                     (unsigned)flipRc, frame);
            probe_log(info);
        }

        if ((frame % 600) == 0) {
            snprintf(info, sizeof(info), "presentando: frame %d (sigue vivo)", frame);
            probe_log(info);
        }

        // Esperar al vblank por sondeo del estado del flip: evita girar a miles de fps.
        sceKernelUsleep(16000);
    }

    // No se alcanza: la sonda esta pensada para observarse en pantalla.
}
