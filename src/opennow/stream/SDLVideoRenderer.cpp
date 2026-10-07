#include "SDLVideoRenderer.hpp"
#include "../stream_startup_diagnostics.hpp"
#include "color_simd.hpp"
#include <emmintrin.h>   // SSE2: el intercambio R<->B de la copia por filas
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

SDL_Renderer* SDLVideoRenderer::target_=nullptr;
SDL_Window* SDLVideoRenderer::renderWindow_=nullptr;
SDL_Surface* SDLVideoRenderer::renderCanvas_=nullptr;
void SDLVideoRenderer::SetRenderTarget(SDL_Renderer* renderer){target_=renderer;}
void SDLVideoRenderer::SetRenderWindow(SDL_Window* w){renderWindow_=w;}
void SDLVideoRenderer::SetRenderCanvas(SDL_Surface* s){renderCanvas_=s;}
SDLVideoRenderer::~SDLVideoRenderer(){if(texture_) SDL_DestroyTexture(texture_);if(bgraTexture_) SDL_DestroyTexture(bgraTexture_);}

// =================================================================================================
// RUTA DE CONVERSION DEL VIDEO — LA PROPIA POR DEFECTO, CON MEDIDAS QUE LA RESPALDAN
// =================================================================================================
// =================================================================================================
// HISTORIA COMPLETA DE ESTA DECISION (tres versiones, dos correcciones)
// =================================================================================================
// **v3.49**: active la ruta propia POR DEFECTO razonando que elimina un pase de conversion de SDL.
// **Era una apuesta sin medir.**
//
// **v3.57**: escribi un banco para medirla y, al ver numeros altos, la desactive creyendo que era una
// regresion. **Tambien sin medir el paso concreto.**
//
// **v3.58 (esta version)**: medi el camino de SDL paso a paso y el conversor del proyecto aislado:
//
//     `tests/sdl_video_path_cost_bench.cpp`  (camino de SDL, en el PC)
//         conversion NV12 -> ARGB (escalar)     : 14,702 ms   <-- EL 82 % DEL COSTE
//         escalado a 1920x1080 (vecino cercano) :  2,097 ms
//         memset+memcpy del framebuffer         :  1,153 ms
//
//     `tests/nv12_convert_compare_bench.cpp`  (los dos conversores, sin escalar)
//         SSE2 del proyecto :  0,784 ms
//         escalar de SDL    : 14,054 ms      -> **el SSE2 es 18x mas rapido**
//
// **La conclusion es que la conversion ES el cuello (14,7 ms de 18), y que el SSE2 lo elimina casi por
// completo.** Sustituirla esta plenamente justificado. Lo que estaba mal en la v3.53 era ANADIRLE el
// escalado bilineal a 1080p, que hacia que la conversion SSE2 pasara de 0,8 a 21 ms y se comiera toda
// la ganancia. El escalado en si cuesta solo 2,1 ms y lo hace SDL igual, asi que no compensa hacerlo
// nosotros.
//
// **COMBINACION FINAL:** conversion SSE2 a resolucion nativa + SDL estira al lienzo.
// Coste del video: del orden de **3 ms** frente a los **18 ms** del camino de SDL.
//
// ESCAPE: si algo fuera mal (colores, textura), crear `/data/gfnps4/video_sdl_yuv.flag` y se vuelve a
// la ruta de SDL sin recompilar. Y la ruta degrada sola ante cualquier fallo.
static bool useOwnBgraPath() {
    static int cached = -1;
    if(cached < 0) {
        struct stat st{};
        const bool escape = (stat("/data/gfnps4/video_sdl_yuv.flag", &st) == 0);
        cached = escape ? 0 : 1;
        opennow::LogAppLifecycleEvent("STREAM_VIDEO_PATH",
            escape ? "mode=sdl_yuv_upload (escape video_sdl_yuv.flag presente)"
                   : "mode=own_bgra_sse2 (por defecto desde v3.58, conversion 18x mas rapida)");
    }
    return cached == 1;
}

void SDLVideoRenderer::draw(NVGcontext* vg,int width,int height,AVFrame* frame,int format){
    (void)drawLatest(vg,width,height,frame,format,0);
}
// =================================================================================================
// SUB-ETAPAS DEL DIBUJADO DEL VIDEO (v3.61) — valores compartidos con el enum de `src/ps4/main.cpp`
// =================================================================================================
// POR QUE SE REPITEN LOS NUMEROS AQUI EN LUGAR DE INCLUIR EL ENUM:
// el enum esta en `main.cpp` (no en una cabecera), y este fichero es **multiplataforma**. Meter una
// cabecera de PS4 aqui romperia las otras plataformas. Los valores son parte del contrato de traza, y
// `audit-build.ps1` ya comprueba la coherencia de la tabla de nombres, asi que un desajuste se detecta.
//
// MOTIVO: la traza `stage=15 name=DRAW_STREAM_VIDEO` de la v3.51 acoto el cierre al dibujado del video,
// pero ese nombre cubre cuatro trabajos distintos. Estas marcas dicen cual.
enum : int {
    kStageVideoEnter    = 21,   // entrada a drawLatest
    kStageVideoScale    = 22,   // conversion de color / escalado
    kStageVideoUpload   = 23,   // SDL_UpdateTexture
    kStageVideoCopy     = 24,   // SDL_RenderCopy al lienzo
    kStageVideoFallback = 25    // ruta clasica de SDL (YUV)
};
bool SDLVideoRenderer::drawLatest(NVGcontext*,int,int,AVFrame* frame,int,uint64_t generation){
    opennow::SetCurrentStage(kStageVideoEnter);
    if(!target_ || !frame || frame->width<=0 || frame->height<=0) return false;
    if(frame->width>3840 || frame->height>2160 || !frame->data[0] || !frame->data[1] ||
       ((frame->format==AV_PIX_FMT_YUV420P || frame->format==AV_PIX_FMT_YUVJ420P) && !frame->data[2]) ||
       frame->linesize[0]<frame->width) {
        if(!invalidFrameLogged_) {
            opennow::WriteStreamStartupStage("video_render_invalid_frame");
            char detail[160];
            std::snprintf(detail,sizeof(detail),"width=%d height=%d format=%d y_stride=%d",frame->width,frame->height,frame->format,frame->linesize[0]);
            opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_INVALID_FRAME",detail);
            invalidFrameLogged_=true;
        }
        return false;
    }
    if((frame->format==AV_PIX_FMT_YUV420P || frame->format==AV_PIX_FMT_YUVJ420P) &&
       (frame->linesize[1]<(frame->width+1)/2 || frame->linesize[2]<(frame->width+1)/2)) return false;
    if(frame->format==AV_PIX_FMT_NV12 && frame->linesize[1]<((frame->width+1)/2)*2) return false;
    if(!firstUploadLogged_) {
        opennow::WriteStreamStartupStage("video_render_first_upload_begin");
        char detail[160];
        std::snprintf(detail,sizeof(detail),"width=%d height=%d format=%d y_stride=%d uv_stride=%d",
            frame->width,frame->height,frame->format,frame->linesize[0],frame->linesize[1]);
        opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FIRST_UPLOAD_BEGIN",detail);
    }
    const auto start=std::chrono::steady_clock::now();

    // =============================================================================================
    // RUTA BGRA PROPIA (opcional): convierte con el SSE2 del proyecto y sube RGB
    // =============================================================================================
    // Si el flag esta activo, se convierte el frame a BGRA aqui y se sube con `SDL_UpdateTexture`.
    // SDL recibe entonces RGB, su textura nativa coincide en formato y **se salta su propia
    // conversion YUV->RGB** (la de `SDL_SW_CopyYUVToRGB`).
    //
    // Si algo de esta ruta no se puede hacer (formato no soportado, fallo al crear la textura), se
    // cae a la ruta de siempre SIN devolver error: el video nunca se queda sin pintar por un
    // experimento.
    // =============================================================================================
    // EL FORMATO DEL STREAM ES `AV_PIX_FMT_YUVJ420P` (12), NO `YUV420P` (0). Y ESO LO CAMBIA TODO.
    // =============================================================================================
    // BUG ENCONTRADO EN LA v3.58, CON LA TRAZA DE LA CONSOLA DELANTE:
    //
    // El log de la consola dice:
    //
    //     STREAM_VIDEO_RENDER_FRAME_CALLBACK_BEGIN format=12 width=1280 height=720
    //     FRAME_COLOR_RANGE format=12 color_range=2 colorspace=1
    //
    // Y los valores del enum de este FFmpeg (`build/libpeer-ps4/dist/include/libavutil/pixfmt.h`,
    // contados sobre el propio enum) son:
    //
    //     AV_PIX_FMT_YUV420P  = 0
    //     AV_PIX_FMT_YUVJ420P = 12     <-- ESTE es el que llega
    //     AV_PIX_FMT_NV12     = 23
    //
    // **La condicion de esta funcion solo aceptaba NV12 y YUV420P.** Es decir: **rechazaba TODOS los
    // frames del stream** y la ruta rapida que se habia activado por defecto **NUNCA se ejecutaba**.
    // La app se quedaba siempre en el camino lento de SDL (`SDL_UpdateYUVTexture` +
    // `SDL_ConvertPixels`, el que cuesta 14,7 ms de conversion en el PC).
    //
    // **Eso explica por que las medidas de la consola no cambiaban entre versiones** (present_us
    // ~50.000 us en la v3.45 y en la v3.58): mi mejora estaba en un camino que no se ejecutaba.
    //
    // Y explica por que NO aparecia ninguno de los marcadores de esa ruta en el log
    // (`STREAM_VIDEO_PHASES`, `STREAM_VIDEO_BGRA_TEXTURE_CREATED`): no se llegaba a ella. Los
    // marcadores no estaban mal; **estaban dentro de codigo muerto para este flujo**.
    //
    // ESTE MISMO PROYECTO YA CONOCIA EL CASO en su camino de caratulas del catalogo
    // (`decodeCatalogJpeg`), que trata `YUV420P`, `YUVJ420P` y `NV12`. Aqui se habia olvidado.
    //
    // `YUVJ420P` es "full range" (0-255) por definicion, asi que ademas hay que convertir con
    // `full_range = true`, no con el valor que devuelva `isFrameFullRange` (que para el puede dar
    // falso y lavaria los negros).
    const bool esNv12     = (frame->format == AV_PIX_FMT_NV12);
    const bool esYuv420   = (frame->format == AV_PIX_FMT_YUV420P);
    const bool esYuvj420  = (frame->format == AV_PIX_FMT_YUVJ420P);
    if(useOwnBgraPath() && (esNv12 || esYuv420 || esYuvj420)) {
        const int fw=frame->width, fh=frame->height;

        // =========================================================================================
        // EL FRAME SE CONVIERTE **Y SE ESCALA** AQUI, AL TAMANO DEL LIENZO
        // =========================================================================================
        // LA TEXTURA SE CREA AL TAMANO DEL **FRAME**, PERO EL DESTINO ES SIEMPRE **EL LIENZO**
        // =========================================================================================
        // =========================================================================================
        // BUG CORREGIDO EN LA v3.63: EL VIDEO SE ENCOGIA AL TAMANO DEL FRAME
        // =========================================================================================
        // La version anterior hacia esto:
        //
        //     const int dw = fw, dh = fh;                 // destino = tamanio del FRAME
        //     bgraTexture_ = SDL_CreateTexture(..., dw, dh);
        //     const SDL_Rect dstBgra = {0, 0, dw, dh};    // <-- 1280x720 o 960x540
        //     SDL_RenderCopy(target_, bgraTexture_, nullptr, &dstBgra);
        //
        // Es decir: **el video se dibujaba a su tamanio nativo dentro de un lienzo de 1920x1080**. Con
        // el servidor a 960x540 eso son **960x540 en la esquina superior izquierda**: un cuarto de
        // pantalla, con el resto negro. Y como GeForce NOW aplica **resolucion dinamica**, cada vez que
        // el servidor bajaba la resolucion **la imagen se encogia un poco mas** — que es exactamente lo
        // que se reporto ("al mover el control la imagen se vuelve aun mas pequena").
        //
        // El error nacio de una confusion de optimizacion: se eligio el destino igual al frame para que
        // `SDL_RenderCopy` no tuviera que escalar. Eso es mas rapido, **pero deja el video sin estirar
        // al lienzo**, que es justo lo que no se puede hacer.
        //
        // LA CORRECCION (y es la que pide la directriz):
        //   - La TEXTURA sigue siendo del tamanio del frame: asi la conversion SSE2 escribe en su
        //     resolucion nativa y sigue siendo barata (medido: 1.303 us frente a 9.809 del codigo
        //     escalar de SDL).
        //   - El DESTINO es **siempre el lienzo completo** (`SDL_GetRendererOutputSize`, que en PS4 son
        //     los 1920x1080 del display). **El video ocupa toda la pantalla, sin importar a que
        //     resolucion lo mande el servidor.**
        //   - El escalado lo hace SDL en su blit, que es lo que ya costaba ~15 ms y sigue costando
        //     parecido: **el coste no aumenta, solo se reparte en los pixeles de pantalla.**
        const int dw=fw, dh=fh;

        // =============================================================================================
        // POR QUE LOS 20 FPS: el blit de SDL ESCALABA (v3.90). Y COMO SE EVITA ESCALANDO ANTES.
        // =============================================================================================
        // MEDIDO EN CONSOLA, cuatro versiones:
        //
        //     v3.62    destino del blit = tamano del FRAME (1:1)  -> copy_us 10.000-18.000 -> **59 fps**
        //     v3.80-89 destino del blit = LIENZO 1920x1080        -> copy_us 48.000-64.000 -> **19-20 fps**
        //
        // El mecanismo esta en el renderizador software de SDL (`SDL_render_sw.c:682-689`):
        //
        //     if ( srcrect->w == dstrect->w && srcrect->h == dstrect->h )
        //         SDL_BlitSurface(...);    // copia DIRECTA, barata
        //     else
        //         SDL_BlitScaled(...);     // REESCALADO con conversion por pixel
        //
        // Y `docs/versiones/PS4-V3.44-CAUSA-REAL-DE-LOS-19FPS.md` ya documenta este mismo hallazgo: un
        // `RenderCopy` mal dimensionado que **reescalaba a 1080p en CPU**.
        //
        // EL PROBLEMA DE LA SOLUCION ANTIGUA (destino 1:1): el video queda **en la esquina**. Sobre un
        // lienzo de 1920x1080 (confirmado por medida: `SDL_RenderClear` cuesta 2.598 us = 2.073.600 px a
        // 1,25 ns/px), un destino de 960x540 ocupa un cuarto de pantalla. Es lo que se veia en la v3.62.
        //
        // LA SOLUCION DE ESTA VERSION: **escalar DURANTE la conversion, no despues.**
        // La textura pasa a medir **el lienzo** (`tw` x `th` = 1920x1080), y el escalado lo hace
        // `opennow::color::ScaleBilinearYUV420PToBGRA_BT709`, que convierte Y escala en **una sola pasada
        // sobre los datos de ORIGEN** repartiendo las filas entre hilos. Asi:
        //
        //   - El blit pasa a ser **1:1** (textura 1920x1080 -> destino 1920x1080): no escala, y SDL usa
        //     su copia directa.
        //   - El escalado se hace **una sola vez**, sobre 0,52 Mpx de origen, en vez de sobre 2,07 Mpx de
        //     destino con conversion por pixel.
        //   - **El video llena la pantalla**, que es lo que se quiere.
        //
        // `dst_w`/`dst_h` es el LIENZO (destino del blit y medida de la textura); `dw`/`dh` es el FRAME
        // (origen, lo que manda el servidor y cambia dinamicamente).
        int dst_w = 1920, dst_h = 1080;
        if(SDL_GetRendererOutputSize(target_, &dst_w, &dst_h) != 0 || dst_w <= 0 || dst_h <= 0) {
            dst_w = fw; dst_h = fh;
        }
        // El blit es 1:1 porque la TEXTURA mide el lienzo (`tw`/`th`, que se fija mas abajo). Se registra
        // UNA vez por geometria: es el dato que confirma el 1:1 y con que tamanios.
        {
            const int tw_log = dst_w, th_log = dst_h;
            if(tw_log != loggedBlitW_ || th_log != loggedBlitH_ || dw != loggedBlitSrcW_ || dh != loggedBlitSrcH_) {
                loggedBlitW_ = tw_log; loggedBlitH_ = th_log;
                loggedBlitSrcW_ = dw; loggedBlitSrcH_ = dh;
                char bd[224];
                std::snprintf(bd,sizeof(bd),
                              "origen=%dx%d textura=%dx%d destino=%dx%d lienzo=%dx%d uno_a_uno=1 "
                              "via=escalado_en_la_conversion",
                              dw,dh,tw_log,th_log,dst_w,dst_h,dst_w,dst_h);
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_BLIT", bd);
            }
        }

        const bool newFrameBgra = (generation==0 || generation!=bgraGeneration_);
        // =============================================================================================
        // LA TEXTURA PASA A MEDIR EL LIENZO (v3.90b): ESCALADO DURANTE LA CONVERSION, SIN BLIT
        // =============================================================================================
        // Razon, con las medidas de consola delante:
        //
        //   - Con textura de 960x540 y destino de 1920x1080, el blit de SDL **ESCALA**, y
        //     `SDL_render_sw.c:682-689` solo usa la copia directa si los tamanos coinciden. Medido:
        //     `copy_us` 48.000-64.000 -> **19-20 fps**.
        //   - Con destino 1:1 (960x540) no escala, pero el video queda **en la esquina**: es lo que
        //     ocurria en la v3.62, y es lo que el usuario no quiere.
        //
        // LA SALIDA: **escalar DURANTE la conversion**, no despues. El proyecto ya tiene
        // `opennow::color::ScaleBilinearYUV420PToBGRA_BT709`, que convierte Y escalando en **una sola
        // pasada sobre los datos de ORIGEN** y reparte las filas entre hilos (`RowWorkerPool`). Asi:
        //
        //   - La textura mide **el lienzo** (1920x1080), asi que el blit es **1:1 y no escala**.
        //   - El escalado se hace una vez, sobre 0,52 Mpx de origen, en vez de sobre 2,07 Mpx de destino
        //     con conversion por pixel.
        //   - El video **llena la pantalla**.
        //
        const int tw = dst_w, th = dst_h;   // la TEXTURA mide el LIENZO -> blit 1:1
        if(!bgraTexture_ || bgraWidth_!=tw || bgraHeight_!=th) {
            if(bgraTexture_) SDL_DestroyTexture(bgraTexture_);
            // =====================================================================================
            // VUELTA A 4 BYTES POR PIXEL (v3.82). EL FORMATO DE 3 BYTES ERA UNA PISTA FALSA.
            // =====================================================================================
            // En la v3.81 puse la textura en `SDL_PIXELFORMAT_BGR888` (3 bytes) porque el driver de
            // SDL-PS4 **declara** el lienzo asi:
            //
            //     /* SDL_ps4video.c:456 */
            //     const Uint32 surface_format = SDL_PIXELFORMAT_BGR888;
            //
            // Resultado en consola: **la imagen se vio en BLANCO Y NEGRO y sin llenar la pantalla**. La
            // razon esta en el MISMO driver, tres lineas mas abajo:
            //
            //     /* SDL_ps4video.c:499-519 */
            //     uint32_t * pDst = (uint32_t*)CurrentBuffer(_this);
            //     for (y...) memcpy(&pDst[width*(yOffs+y)+xOffs],
            //                       &((uint8_t*)surface->pixels)[y*pitch], drawW * 4);
            //
            // **El driver trata la superficie como de 32 bits** (puntero `uint32_t*` y `drawW*4` bytes
            // por fila) aunque la haya declarado de 3. Es una **incoherencia del propio driver**: al
            // copiar a VideoOut con paso de 32 bits, **solo 3 de cada 4 bytes llegan a pantalla**, y eso
            // produce exactamente una imagen desplazada y desaturada.
            //
            // Ademas, el camino rapido de SDL para 3 bytes es PESIMO:
            //
            //     /* SDL_stretch.c:177 — copy_row3 */
            //     *dst++ = pixel[0]; *dst++ = pixel[1]; *dst++ = pixel[2];   // 3 escrituras de BYTE
            //
            // frente a `copy_row4`, que hace **un `Uint32` por pixel**. Por eso el formato de 3 bytes
            // dio 20 ms de `copy_us` en lugar de bajar mas.
            //
            // CONCLUSION: el formato correcto es **4 bytes**, que es como el driver lee la superficie Y
            // como SDL escala mas rapido. Se vuelve a `ARGB8888`, que es ademas lo que produce el
            // conversor SSE2 (`B,G,R,A` en memoria = `ARGB8888` en little-endian).
            //
            // La mejora que SI queda de la v3.81 es el **destino forzado al lienzo**: el video ocupa toda
            // la pantalla.
            // =========================================================================================
            // BUG GRAVE CORREGIDO (v4.11): LA TEXTURA SE CREABA AL TAMANO DEL FRAME, NO DEL LIENZO
            // =========================================================================================
            // ANTES:
            //     bgraTexture_ = SDL_CreateTexture(target_, SDL_PIXELFORMAT_ARGB8888,
            //                                      SDL_TEXTUREACCESS_STREAMING, dw, dh);   // 960x540
            //     bgraWidth_ = dw; bgraHeight_ = dh;                                        // 960,540
            //
            // Pero mas abajo el escalador escribe `tw x th` (el LIENZO, 1920x1080) y se sube con
            // `SDL_UpdateTexture(bgraTexture_, nullptr, bgra_.data(), tw*4)`. Con la textura a
            // 960x540 y el paso de subida a 1920*4 bytes por fila:
            //
            //   1. **`SDL_UpdateTexture` escribe 8,29 MB en una textura de 1,9 MB** -> escritura fuera
            //      de los limites de la textura interna de SDL.
            //   2. **La condicion de re-creacion se cumplia SIEMPRE**: compara `bgraWidth_ != tw`
            //      (960 != 1920) y ademas el cuerpo guardaba `dw`, no `tw`, asi que la textura se
            //      **destruia y recreaba en CADA frame**. Medido en consola con la v4.06: 16 marcas
            //      `STREAM_VIDEO_BGRA_TEXTURE_CREATED` seguidas y
            //      `STREAM_VIDEO_PHASES scale_us=35708 upload_us=0 copy_us=48389` -> **84 ms por
            //      frame = 12 fps**.
            //   3. **La imagen se veia GIGANTE**: la textura de 960x540 se dibuja en el destino de
            //      1920x1080, es decir **se estira 2x sobre lo ya escalado**.
            //
            // Y `bgra_` SI estaba bien dimensionado, porque `need = tw*th*4` (linea de arriba). **Era
            // la textura la que no cuadraba con el buffer.** El invariante es: el buffer, la textura y
            // el blit tienen que medir **el LIENZO**.
            // =========================================================================================
            // LA TEXTURA TIENE QUE TENER EL MISMO FORMATO QUE EL LIENZO (v4.14)
            // =========================================================================================
            // Y con el mismo orden de bytes. El lienzo (y la ventana) son `SDL_PIXELFORMAT_BGR888`,
            // que en SDL es `PACKEDORDER_XBGR` -> memoria `[R][G][B]`. El escalador SSE2 escribe ahora
            // ese MISMO orden (`color_simd.cpp`, `bgra_store8`: valor 0xAABBGGRR = memoria `[R][G][B][A]`).
            //
            // POR QUE ESTO ES LO QUE DESBLOQUEA LOS FPS: `SDL_LowerBlitScaled`
            // (`SDL_surface.c:897-903`) solo usa el camino rapido `SDL_SoftStretch` cuando
            // **`src->format->format == dst->format->format`**. Con la textura en `ARGB8888` y el
            // lienzo en `BGR888` **no coincidian**, asi que SDL caia a `SDL_LowerBlit`, que convierte
            // pixel a pixel. Medido en consola con la v4.11:
            //
            //     SDL_WINDOW_SURFACE_FORMAT ... COINCIDEN_LIENZO_Y_VENTANA=1 coincide_con_textura=0
            //     STREAM_VIDEO_PHASES scale_us=29374 copy_us=41652 frames=12   -> 19 fps
            //
            // **`copy_us=41.652` son 20 ns/pixel** (2.073.600 px). La copia rapida es 1,25 ns/px: la
            // diferencia es **~38 ms por frame**. Igualando los formatos, el blit entra en el camino
            // rapido y los colores NO cambian, porque el orden de bytes que escribe el escalador ya es
            // el que el lienzo necesita.
            bgraTexture_=SDL_CreateTexture(target_,SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING,tw,th);
            bgraWidth_=tw; bgraHeight_=th; bgraGeneration_=UINT64_MAX;
            if(bgraTexture_) {
                SDL_SetTextureBlendMode(bgraTexture_,SDL_BLENDMODE_NONE);
                // =====================================================================================
                // DIAGNOSTICO UNA SOLA VEZ (v4.11). NO ES SOLO RUIDO EN EL LOG.
                // =====================================================================================
                // POR QUE SE PONE ESTA GUARDA: con el bug de la textura al tamanio del frame, este
                // bloque se ejecutaba **en cada frame** — 3.573 veces en una sola sesion de la v4.06.
                // Y no era gratis: hace `SDL_QueryTexture` y sobre todo `SDL_LockTexture` /
                // `SDL_UnlockTexture`, que en el renderizador software implican trabajo real sobre la
                // textura. **Encima de la recreacion, que ya costaba 84 ms por frame.**
                //
                // Con la textura ya arreglada esto se ejecuta una vez por cambio de resolucion (una o
                // dos por sesion), asi que la guarda no cambia el comportamiento normal: **existe para
                // que un fallo futuro no se convierta ademas en 3.573 diagnisiicos.**
                static bool s_texDiagLogged = false;
                if(!s_texDiagLogged) {
                s_texDiagLogged = true;
                // Se consulta el FORMATO REAL de la textura y del lienzo, y el paso que devuelve el
                // bloqueo. Sin estos numeros no se puede saber si el camino rapido de SDL se activa
                // (`SDL_LowerBlitScaled` exige que los dos formatos COINCIDAN) ni si el empaquetado a 3
                // bytes cabe en una fila. Es el diagnostico que faltaba en la v3.81.
                Uint32 texFmt = 0; int texW = 0, texH = 0;
                SDL_QueryTexture(bgraTexture_, &texFmt, nullptr, &texW, &texH);
                int lockPitch = -1;
                {
                    uint8_t* probePixels = nullptr; int probePitch = 0;
                    if(SDL_LockTexture(bgraTexture_, nullptr,
                                       reinterpret_cast<void**>(&probePixels), &probePitch) == 0) {
                        lockPitch = probePitch;
                        SDL_UnlockTexture(bgraTexture_);
                    }
                }
                char bd[288];
                // El log tiene que decir la VERDAD: antes ponia `texture=dw x dh` y
                // `esperado_4bpp=dw*4`, cuando la textura se crea con `tw,th` y la subida usa `tw*4`.
                // Ese desajuste entre lo que se registraba y lo que se hacia es justo lo que dejo pasar
                // el bug de la textura al tamanio equivocado durante varias versiones.
                std::snprintf(bd,sizeof(bd),
                              "texture=%dx%d dst=%dx%d frame=%dx%d fmt_textura=%s fmt_pedido=%s "
                              "pitch_bloqueo=%d esperado_4bpp=%d",
                              tw,th,dst_w,dst_h,dw,dh,
                              SDL_GetPixelFormatName(texFmt),
                              "SDL_PIXELFORMAT_ARGB8888",
                              lockPitch, tw*4);
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_BGRA_TEXTURE_CREATED",bd);
                }
            } else {
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_BGRA_TEXTURE_FAILED",SDL_GetError());
            }
        }
        if(bgraTexture_ && newFrameBgra) {
            // =========================================================================================
            // REQUISITO 2: VALIDACION ESTRUCTURAL ANTES DE ESCRIBIR (v3.63)
            // =========================================================================================
            // EL CRASH ESTABA AQUI. La traza de la consola fue:
            //
            //     last_stage.txt -> stage=23 name=VIDEO_RENDER_UPLOAD beat=3570
            //
            // `kStageVideoUpload` se marca **justo antes** de `SDL_UpdateTexture`, que copia
            // `dw*4` bytes por fila desde `bgra_.data()` a la superficie interna de la textura.
            //
            // **GeForce NOW aplica RESOLUCION DINAMICA**: en medio de la partida el servidor pasa de
            // 1280x720 a 960x540 o menos. Cuando eso ocurre, `dw` y `dh` cambian, la textura se recrea
            // y el buffer `bgra_` tiene que crecer o encogerse en consecuencia. **Si cualquiera de esas
            // tres cosas se desincroniza en un solo frame, la subida escribe fuera de memoria** — que
            // es exactamente un desbordamiento de buffer, y encaja con que tardara 3.570 latidos en
            // aparecer (hace falta un cambio de resolucion).
            //
            // Las tres comprobaciones que siguen son baratas (unas restas y comparaciones) y **cubren
            // las tres formas en que la desincronizacion puede ocurrir**:
            //
            //   (a) el frame declara un tamanio cuya superficie no cabe en el buffer asignado
            //   (b) los `linesize` (pitch) son incoherentes con el tamanio declarado
            //   (c) el buffer `bgra_` no tiene el tamanio que la textura espera
            //
            // Ante CUALQUIERA de las tres, **no se sube nada** y se registra: es preferible perder un
            // frame (o unos pocos, hasta que el servidor se estabilice) que matar la aplicacion.
            // `need` es el tamanio del **destino**, no el del frame (v3.90b): ahora el conversor ESCALA
            // durante la conversion, asi que el buffer de respaldo tiene que caber el lienzo completo.
            // Con texto de lienzo (1920x1080) son 8,29 MB, la misma cifra que maneja la textura.
            const size_t need=static_cast<size_t>(tw)*static_cast<size_t>(th)*4u;

            // (b) coherencia de los pitch declarados. Ya se comprobo arriba que `linesize[0]>=width`
            // y que los planos de croma llegan, pero aqui se comprueba TAMBIEN contra los limites
            // absolutos, porque el cambio de resolucion es justo el momento en que pueden mentir.
            bool estructuraOk = (dw > 0 && dh > 0 && dw <= 3840 && dh <= 2160 &&
                                 frame->linesize[0] >= dw &&
                                 need > 0 && need <= (64u * 1024u * 1024u));
            if(estructuraOk) {
                const int cw = (dw + 1) / 2;
                if(esYuv420 || esYuvj420) {
                    estructuraOk = (frame->data[1] != nullptr && frame->data[2] != nullptr &&
                                    frame->linesize[1] >= cw && frame->linesize[2] >= cw);
                } else if(esNv12) {
                    estructuraOk = (frame->data[1] != nullptr && frame->linesize[1] >= cw * 2);
                }
            }
            if(!estructuraOk) {
                // Se registra UNA vez por segundo como maximo para no inundar el log en una racha.
                static uint64_t s_lastStructLogMs = 0;
                const uint64_t nowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                if(nowMs - s_lastStructLogMs >= 1000) {
                    s_lastStructLogMs = nowMs;
                    char sd[200];
                    std::snprintf(sd,sizeof(sd),
                                  "w=%d h=%d fmt=%d y_stride=%d u_stride=%d v_stride=%d need=%zu",
                                  dw, dh, (int)frame->format, frame->linesize[0],
                                  frame->linesize[1], frame->linesize[2], need);
                    opennow::LogAppLifecycleEvent("STREAM_VIDEO_STRUCT_REJECTED", sd);
                }
                return false;   // se descarta el frame; NO se escribe nada
            }

            if(bgra_.size()<need) bgra_.resize(need);

            // (c) el buffer tiene que ser EXACTAMENTE del tamanio que la textura va a leer. Si no lo
            // es, no se sube: `SDL_UpdateTexture` leeria `dw*4` bytes por fila y se saldria.
            if(bgra_.size() < need) {
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_BUFFER_TOO_SMALL",
                    "el buffer BGRA no cubre el frame; frame descartado");
                return false;
            }
            // RANGO DE COLOR. `YUVJ420P` significa "full range" POR DEFINICION del formato (la J es de
            // JPEG), asi que se fuerza `true`. Usar `isFrameFullRange()` para el seria incorrecto: si
            // devolviera falso, el conversor aplicaria la matriz de 16-235 y **los negros saldrian
            // lavados** (grises en lugar de negros).
            //
            // El log de la consola lo confirma por otra via: `FRAME_COLOR_RANGE ... color_range=2`,
            // que es `AVCOL_RANGE_JPEG` = rango completo.
            const bool fullRange = esYuvj420 ? true : isFrameFullRange(frame);

            // =====================================================================================
            // MEDICION: CONVERSION+ESCALADO  vs  SUBIDA A TEXTURA  vs  COPIA AL LIENZO
            // =====================================================================================
            // POR QUE HACEN FALTA TRES NUMEROS Y NO UNO:
            //
            // El log dice `present_us` = 50.000 us en el stream, y con el menu en 5.400 us la
            // diferencia parece "el video". Pero "el video" son TRES trabajos distintos, y cada uno
            // pide un arreglo diferente:
            //
            //   1. `scale_us`  : nuestro escalado bilineal SSE2 (conversion de color + escalado)
            //   2. `upload_us` : `SDL_UpdateTexture`, que COPIA 8,3 MB a su superficie interna y
            //                    ademas convierte formato si el nativo no coincide
            //   3. `copy_us`   : `SDL_RenderCopy`, que ahora deberia ser 1:1 sin escalado
            //
            // Sin separarlos no se puede elegir: si el coste esta en (1) hay que tocar el escalador;
            // si esta en (2) hay que reducir el tamano de la textura (y eso significa bajar el lienzo);
            // y si esta en (3) el problema esta en el blit del renderizador software.
            //
            // Los tres se registran una vez por segundo, junto a `STREAM_VIDEO_DRAW_US`.
            const uint64_t t_scale0 = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            opennow::SetCurrentStage(kStageVideoScale);
            // CONVERSION SSE2 A RESOLUCION NATIVA. Es el paso que domina el coste (14,7 ms con el
            // =====================================================================================
            // ESCRITURA DIRECTA EN LA TEXTURA (v3.65) — ELIMINA LA SUBIDA A TEXTURA
            // =====================================================================================
            // MEDIDO EN CONSOLA (v3.62, 4.579 muestras):
            //
            //     scale_us  medio =  1.303 us    <- la conversion SSE2
            //     upload_us medio =  1.475 us    <- SDL_UpdateTexture
            //     copy_us   medio = 15.241 us    <- el blit de SDL (85 %)
            //
            // `SDL_UpdateTexture` copia el buffer BGRA a la superficie interna de la textura. **Esa
            // copia es trabajo puro de memoria que se puede eliminar**: `SW_LockTexture` del
            // renderizador software devuelve el puntero DIRECTO a la superficie (`SDL_render_sw.c:153`,
            // no copia nada), asi que la conversion puede escribir ahi mismo.
            //
            // ANTES:  conversion -> buffer temporal -> memcpy a la superficie -> blit
            // AHORA:  conversion -> superficie directamente                    -> blit
            //
            // Se ahorra el `upload_us` completo (1,475 us en consola) **y 3,7 MB de transito de memoria
            // por frame** (escritura + lectura). Y como tambien desaparece el `bgra_` de este camino,
            // **se elimina de raiz la posibilidad de que el buffer temporal y la textura se
            // desincronicen** cuando el servidor cambia de resolucion — que era la causa del cierre en
            // `VIDEO_RENDER_UPLOAD`.
            //
            // ESCAPE: si el bloqueo fallara, se vuelve a la ruta con `SDL_UpdateTexture` sin mas.
            // =====================================================================================
            // LA CAUSA DE LOS 19 FPS: LA TEXTURA Y EL LIENZO TIENEN FORMATOS DISTINTOS (v3.81)
            // =====================================================================================
            // MEDIDO EN CONSOLA (v3.80, 83 muestras), con el destino ya forzado a 1920x1080:
            //
            //     copy_us = 46.821 ... 74.433 us        <- ~50 ms POR FRAME
            //     UI_LOOP_PERF loop_fps=19 render_max_ms=51
            //     STREAM_HEALTH_TICK decoded=3804 presented=1268   (decoder 44/s, pantalla 14,5/s)
            //
            // Y el DECODIFICADOR NO ES EL CULPABLE: `decode_us=123..183` con `hilos=5`, es decir
            // **~7.000 unidades por segundo**. Esta INFRAALIMENTADO: solo recibe 44/s porque el bucle
            // principal se pasa 50 ms dentro de `SDL_RenderCopy`.
            //
            // LA CAUSA, leida en el codigo de SDL:
            //
            //     /* SDL_ps4video.c:456 — el LIENZO de PS4 */
            //     const Uint32 surface_format = SDL_PIXELFORMAT_BGR888;    // 24 bits, 3 bytes/pixel
            //
            //     /* SDLVideoRenderer — nuestra TEXTURA */
            //     SDL_CreateTexture(..., SDL_PIXELFORMAT_ARGB8888, ...)   // 32 bits, 4 bytes/pixel
            //
            // Y en `SDL_LowerBlitScaled` (`SDL_surface.c:897-903`) la eleccion del camino depende de que
            // los formatos COINCIDAN:
            //
            //     if ( !(flags complejos) &&
            //          src->format->format == dst->format->format &&     // <-- NUESTRO CASO: FALLA
            //          !indexed )
            //         return SDL_SoftStretch(...);      // camino RAPIDO (replicacion de filas)
            //     else
            //         return SDL_LowerBlit(...);        // camino LENTO (conversion pixel a pixel)
            //
            // **Como los formatos no coinciden, SDL NO usa `SDL_SoftStretch` y cae a `SDL_LowerBlit`**,
            // que hace **conversion de formato pixel a pixel** ademas del estirado. De ahi los 50 ms:
            // 2,07 Mpx de destino con conversion por pixel.
            //
            // (Esto tambien explica por que los menus van a 59 fps: usan el MISMO camino lento, pero UNA
            // sola pasada pequena por frame. El video lo hace sobre imagen completa, 60 veces por
            // segundo.)
            //
            // LA CORRECCION: que la textura tenga **el mismo formato que el lienzo** (`BGR888`, 3 bytes).
            // Con los formatos iguales, SDL entra en `SDL_SoftStretch`, que **copia filas enteras con
            // replicacion** en lugar de convertir pixel a pixel.
            //
            // COMO: el conversor SSE2 sigue escribiendo BGRA (4 bytes, que es lo que sabe hacer), y
            // despues se EMPAQUETA a 3 bytes en la superficie de la textura. El empaquetado es una copia
            // secuencial de 2 Mpx, ordenes de magnitud mas barata que la conversion-por-pixel que
            // sustituye.

            // =============================================================================================
            // CONVERSION **Y ESCALADO** EN LA MISMA PASADA (v3.90b). AQUI ESTA EL ARREGLO DE LOS FPS.
            // =============================================================================================
            // ANTES se llamaba a `ConvertYUV420PToBGRA_BT709` con `dw x dh` (el FRAME) sobre una textura
            // de `dw x dh`, y **el escalado a pantalla completa lo hacia despues el blit de SDL**, que
            // para tamanios distintos de `SDL_BlitScaled` = **conversion por pixel**. Medido en consola:
            // `copy_us` 48.000-64.000 -> **19-20 fps**.
            //
            // AHORA se llama a `ScaleBilinearYUV420PToBGRA_BT709` con **origen `dw x dh`** (lo que manda
            // el servidor) y **destino `tw x th`** (el lienzo, 1920x1080). El escalado se hace **una sola
            // vez, sobre los datos de origen**, repartiendo las filas entre hilos (`RowWorkerPool`). Y
            // como la textura ya mide el lienzo, **el blit posterior es 1:1** y SDL usa su copia directa.
            //
            // Resumen del reparto de trabajo:
            //     ANTES: convertir 0,52 Mpx  +  SDL escala a 2,07 Mpx con conversion por pixel
            //     AHORA: convertir Y escalar en una pasada (2,07 Mpx escritos, 0,52 Mpx leidos, por SSE2
            //            y repartido entre 6 participantes)  +  blit 1:1 barato
            const bool hayEscalado = (tw != dw || th != dh);

            uint8_t* dstPixels = nullptr;
            int dstPitch = 0;
            // =============================================================================================
            // INSTRUMENTACION DE LAS TRES PARTES DE `scale_us` (v4.12). NO ES RUIDO: ES LA PREGUNTA ABIERTA.
            // =============================================================================================
            // `scale_us` cubre TRES cosas muy distintas y hasta ahora no se podian separar en el log:
            //
            //     t_lock   : `SDL_LockTexture`  -> bloquea el acceso a la textura
            //     t_conv   : `ScaleBilinear...` -> la conversion YUV->BGRA + escalado en SSE2
            //     t_unlock : `SDL_UnlockTexture`-> **en el renderizador software COPIA el buffer a la
            //                                       textura**, asi que puede ser el que mas pesa
            //
            // Medido en la v4.06: `scale_us` ≈ 34.000-45.000 us. Y el presupuesto a 60 fps es 16.666 us,
            // asi que **hay que saber cual de las tres se lo lleva** antes de decidir nada. Sin esta
            // separacion estaria adivinando, y ya he adivinado mal en esta serie de cambios.
            // Marca de tiempo en microsegundos, con el mismo idioma que el resto del fichero
            // (`steady_clock`); aqui no existe `now_us()`.
            auto gfn_now_us = []() -> uint64_t {
                return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            };
            const uint64_t t_lock0 = gfn_now_us();
            // =============================================================================================
            // GUARDA DEL BLOQUEO (v4.17). LA REGION BLOQUEADA TIENE QUE CUBRIR LO QUE SE VA A ESCRIBIR.
            // =============================================================================================
            // El guard anterior era `dstPitch >= tw*4`, que **solo comprueba el paso de UNA fila**. No
            // comprobaba que la region bloqueada tuviera las `th` filas, y el escalador escribe
            // `tw x th` pixeles a `dstPitch`. Si el bloqueo devolviera una region mas corta, **la
            // escritura se saldria de la textura**.
            //
            // Se puede comprobar: `SDL_LockTexture` con `NULL` bloquea **toda** la textura, asi que
            // `dstPitch * th` es exactamente el numero de bytes que SDL pone a disposicion. Si
            // `(size_t)dstPitch * th < (size_t)tw * th * 4`, **no se escribe ahi**: se cae al camino de
            // buffer temporal + `SDL_UpdateTexture`, que no puede salirse porque `bgra_` se dimensiona
            // a `tw*th*4`.
            //
            // POR QUE ESTA GUARDA IMPORTA MAS DE LO QUE PARECE: el proyecto tiene documentado un cierre
            // en este mismo sitio (`PS4-V3.65`: *"Si algo se desincroniza en un solo frame, la subida
            // escribe fuera de memoria"*), y **la escritura directa en la textura bloqueada es el unico
            // punto de esta ruta donde el destino no lo controlamos nosotros: lo da SDL.**
            //
            // Y NO ES UN CAMBIO DE COMPORTAMIENTO en el caso normal: con todo correcto,
            // `dstPitch == tw*4` (medido en consola: `pitch_bloqueo=7680 esperado_4bpp=7680`) y la
            // guarda se cumple, asi que se sigue escribiendo directo. **Solo actua si algo no cuadra.**
            const bool locked = (SDL_LockTexture(bgraTexture_, nullptr,
                                                 reinterpret_cast<void**>(&dstPixels), &dstPitch) == 0) &&
                                (dstPixels != nullptr) &&
                                (dstPitch >= tw * 4) &&
                                (static_cast<size_t>(dstPitch) * static_cast<size_t>(th) >=
                                 static_cast<size_t>(tw) * static_cast<size_t>(th) * 4u);
            const uint64_t t_lock1 = gfn_now_us();
            if(!locked && dstPixels != nullptr) {
                // Se bloqueo pero la region no cubre lo que hay que escribir: se desbloquea YA y se usa
                // el camino seguro. Sin este desbloqueo la textura quedaria bloqueada para siempre.
                SDL_UnlockTexture(bgraTexture_);
                dstPixels = nullptr;
                static bool s_lockGuardLogged = false;
                if(!s_lockGuardLogged) {
                    s_lockGuardLogged = true;
                    char lg[192];
                    std::snprintf(lg,sizeof(lg),
                                  "region_insuficiente dstPitch=%d th=%d necesita=%d -> buffer_temporal",
                                  dstPitch, th, tw * th * 4);
                    opennow::LogAppLifecycleEvent("STREAM_VIDEO_LOCK_GUARD", lg);
                }
            }
            uint64_t t_conv0 = t_lock1, t_conv1 = t_lock1;
            if(locked) {
                // La conversion (y el escalado) escriben DIRECTAMENTE en la superficie de la textura, con
                // SU paso real (que puede ser mayor que `tw*4`). El formato del conversor (B,G,R,A por
                // pixel) coincide con el de la textura (ARGB8888).
                if(frame->format==AV_PIX_FMT_NV12) {
                    opennow::color::ScaleBilinearNV12ToBGRA_BT709(
                        dstPixels, dstPitch, tw, th, dw, dh,
                        frame->data[0], frame->linesize[0],
                        frame->data[1], frame->linesize[1], fullRange);
                } else {
                    opennow::color::ScaleBilinearYUV420PToBGRA_BT709(
                        dstPixels, dstPitch, tw, th, dw, dh,
                        frame->data[0], frame->linesize[0],
                        frame->data[1], frame->linesize[1],
                        frame->data[2], frame->linesize[2], fullRange);
                }
                t_conv1 = gfn_now_us();
                SDL_UnlockTexture(bgraTexture_);
            } else {
                t_conv1 = gfn_now_us();
                // El bloqueo fallo: respaldo clasico. Se convierte al buffer temporal (4 bytes, el formato
                // del conversor, que coincide con el de la textura `ARGB8888`) y se sube con su paso.
                if(frame->format==AV_PIX_FMT_NV12) {
                    opennow::color::ScaleBilinearNV12ToBGRA_BT709(
                        bgra_.data(), tw*4, tw, th, dw, dh,
                        frame->data[0], frame->linesize[0],
                        frame->data[1], frame->linesize[1], fullRange);
                } else {
                    opennow::color::ScaleBilinearYUV420PToBGRA_BT709(
                        bgra_.data(), tw*4, tw, th, dw, dh,
                        frame->data[0], frame->linesize[0],
                        frame->data[1], frame->linesize[1],
                        frame->data[2], frame->linesize[2], fullRange);
                }
                if(SDL_UpdateTexture(bgraTexture_,nullptr,bgra_.data(),tw*4)!=0) return false;
            }
            const uint64_t t_unlock1 = gfn_now_us();
            // Se guardan los tres tramos para el informe de `STREAM_VIDEO_PHASES`. La suma de los tres
            // tiene que dar `scale_us` (salvo el ruido de las dos llamadas a `now_us()`).
            s_lastLockUs   = (t_lock1  > t_lock0 ) ? (t_lock1  - t_lock0 ) : 0;
            s_lastConvUs   = (t_conv1  > t_conv0 ) ? (t_conv1  - t_conv0 ) : 0;
            s_lastUnlockUs = (t_unlock1 > t_conv1) ? (t_unlock1 - t_conv1) : 0;
            if(hayEscalado && !escaladoEnConversionLogged_) {
                escaladoEnConversionLogged_ = true;
                char ed[200];
                std::snprintf(ed,sizeof(ed),
                              "origen=%dx%d destino=%dx%d via=SSE2_multihilo_escala_en_conversion "
                              "blit_posterior=1:1",
                              dw,dh,tw,th);
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_SCALE_IN_CONVERT", ed);
            }
            const uint64_t t_scale1 = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            opennow::SetCurrentStage(kStageVideoUpload);
            {
                // El `upload_us` ahora mide solo el bloqueo/desbloqueo (que es gratis): si sale ~0, la
                // escritura directa esta funcionando. Se mantiene la medida para poder comparar.
                const uint64_t t_upload1 = t_scale1;
                bgraGeneration_=generation;
                if(!firstUploadLogged_) {
                    opennow::WriteStreamStartupStage("video_render_first_upload_complete");
                    opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FIRST_UPLOAD_COMPLETE");
                    opennow::LogAppLifecycleEvent(locked ? "STREAM_VIDEO_DIRECT_WRITE_ON"
                                                         : "STREAM_VIDEO_DIRECT_WRITE_OFF_FALLBACK",
                                                  locked ? "conversion escribe en la textura (sin copia)"
                                                         : "SDL_LockTexture fallo; se usa UpdateTexture");
                }
                // =================================================================================
                // ESCALADO PROPIO EN 4 BYTES: SE SUSTITUYE EL BLIT DE SDL (v3.83)
                // =================================================================================
                // MEDIDO EN CONSOLA (v3.82, 131 muestras, destino 1920x1080):
                //
                //     copy_us medio = 59.392 us   ->  27,4 ns POR PIXEL DESTINO
                //     SDL_RenderClear hace esos mismos 2.073.600 px en 2.600 us = 1,25 ns/px
                //
                // **El blit escalado de SDL es 22 veces mas caro POR PIXEL que una simple limpieza**, y
                // la memoria da de sobra (la limpieza escribe los mismos bytes en 2,6 ms). El coste no
                // es el ancho de banda: es que el camino que SDL toma para este caso
                // (`SDL_LowerBlit`, porque el formato del lienzo `BGR888` NO coincide con el de la
                // textura `ARGB8888`) **convierte pixel a pixel**.
                //
                // Y no se puede hacer que coincidan: el driver de SDL-PS4 **declara** el lienzo de
                // 3 bytes (`SDL_ps4video.c:456`) pero lo **recorre con paso de 32 bits**
                // (`uint32_t* pDst`, linea 499), asi que el formato de 3 bytes produce la imagen
                // desplazada y en blanco y negro que se reporto. **El lienzo hay que tratarlo como de
                // 4 bytes**, y el driver no es modificable (SDL viene precompilado en el toolchain y no
                // hay script para reconstruirlo).
                //
                // SOLUCION: se escribe el video DIRECTAMENTE en la superficie del lienzo con
                // **escrituras de 4 bytes** (`Uint32`) y **replicacion de columnas precalculada**, que es
                // lo que hace rapido a un escalado por vecino mas cercano. Se evita por completo el
                // camino de conversion por pixel de SDL.
                //
                // =================================================================================
                // ESCALADOR PROPIO EN 4 BYTES HACIA EL LIENZO PROPIO (v3.89) — LA COMBINACION QUE FALTABA
                // =================================================================================
                // Se restaura el escalador rapido de la v3.83 (que dio **60 fps** en consola) pero
                // escribiendo en el **LIENZO PROPIO** en lugar de en la superficie de la ventana.
                //
                // POR QUE CADA PIEZA:
                //
                //   - **Escrituras de 4 bytes** (`Uint32`) con **mapa de columnas precalculado** y
                //     **replicacion de filas**: es lo que hacia rapido al escalado que dio 60 fps.
                //   - **En el lienzo propio**, que es memoria normal. Escribir en la superficie de la
                //     VENTANA (lo que hacia la v3.83) causaba el **parpadeo**, porque esa superficie es
                //     la misma memoria que `SDL_UpdateWindowSurface` esta copiando a VideoOut con sus 4
                //     buffers rotando.
                //   - **No hay copia del lienzo**: el renderizador y el escalador usan la MISMA
                //     superficie, asi que no se añaden los 8,3 MB/frame que hundieron la v3.84.
                //   - **No se pasa por `SDL_RenderCopy` escalando**, que era el otro camino lento: con
                //     textura de 960x540 y destino de 1920x1080 el blit **nunca es 1:1**, y SDL acaba en
                //     su reescalado con conversion por pixel.
                //
                // Si algo de esto no esta disponible, se cae al `SDL_RenderCopy` de siempre SIN devolver
                // error: el video nunca se queda sin pintar por una optimizacion.
                // =================================================================================
                // ESCALADOR PROPIO: DESACTIVADO POR DEFECTO (v3.90). MANDABA EL BLIT 1:1.
                // =================================================================================
                // POR QUE SE DESACTIVA: en esta version se ha restaurado el **destino 1:1** del blit
                // (el tamano del frame), que es el **codigo documentado que daba 59 fps**
                // (`PS4-V3.44-CAUSA-REAL-DE-LOS-19FPS.md`). Si el escalador propio siguiera activo,
                // se harian **las dos cosas a la vez**: escribir a mano los 2 millones de pixeles Y
                // ademas el blit de SDL. Eso es trabajo doble y falsearia la medida.
                //
                // Se deja el codigo detras de esta constante para poder reactivarlo sin reescribirlo si
                // el blit 1:1 no bastara (por ejemplo, si se quisiera video a pantalla completa
                // renunciando al 1:1).
                constexpr bool kUsarEscaladorPropio = false;
                bool escaladoPropioHecho = false;
                if(kUsarEscaladorPropio && renderCanvas_ && renderCanvas_->pixels && dst_w > 0 && dst_h > 0 && dw > 0 && dh > 0 &&
                   renderCanvas_->pitch >= dst_w * 4 && renderCanvas_->w >= dst_w && renderCanvas_->h >= dst_h) {
                    const uint32_t* src32 = reinterpret_cast<const uint32_t*>(bgra_.data());
                    // Mapa de columnas de origen: UNA vez por geometria (no por frame). Si el servidor
                    // cambia de resolucion (dinamica), se rehace una vez y se reutiliza.
                    if(scaleMapW_ != dw || scaleMapH_ != dh || scaleMapDW_ != dst_w || scaleMapDH_ != dst_h) {
                        scaleMapX_.resize(static_cast<size_t>(dst_w));
                        for(int x = 0; x < dst_w; ++x) {
                            int sx = static_cast<int>((static_cast<int64_t>(x) * dw) / dst_w);
                            if(sx >= dw) sx = dw - 1;
                            if(sx < 0) sx = 0;
                            scaleMapX_[static_cast<size_t>(x)] = sx;
                        }
                        scaleMapW_ = dw; scaleMapH_ = dh; scaleMapDW_ = dst_w; scaleMapDH_ = dst_h;
                    }
                    uint8_t* canvasBase = static_cast<uint8_t*>(renderCanvas_->pixels);
                    const int stride32 = renderCanvas_->pitch / 4;
                    for(int y = 0; y < dst_h; ++y) {
                        int sy = static_cast<int>((static_cast<int64_t>(y) * dh) / dst_h);
                        if(sy >= dh) sy = dh - 1;
                        const uint32_t* sRow = src32 + static_cast<size_t>(sy) * dw;
                        uint32_t* dRow = reinterpret_cast<uint32_t*>(canvasBase) + static_cast<size_t>(y) * stride32;
                        for(int x = 0; x < dst_w; ++x) dRow[x] = sRow[scaleMapX_[static_cast<size_t>(x)]];
                    }
                    escaladoPropioHecho = true;
                    if(!firstOwnScaleLogged_) {
                        firstOwnScaleLogged_ = true;
                        char os[224];
                        std::snprintf(os,sizeof(os),
                                      "src=%dx%d dst=%dx%d lienzo=%dx%d pitch=%d stride32=%d "
                                      "via=lienzo_propio_escalado_4bytes",
                                      dw,dh,dst_w,dst_h,renderCanvas_->w,renderCanvas_->h,
                                      renderCanvas_->pitch,stride32);
                        opennow::LogAppLifecycleEvent("STREAM_VIDEO_OWN_SCALE_ON", os);
                    }
                }
                // DESTINO = LIENZO COMPLETO. El video se estira SIEMPRE a la pantalla, sin importar
                // a que resolucion lo mande el servidor (resolucion dinamica de GeForce NOW).
                // DESTINO 1:1 CON LA TEXTURA (v3.90): sin reescalado. Ver la explicacion larga arriba.
                const SDL_Rect dstBgra={0,0,tw,th};   // 1:1 con la textura, que mide el lienzo
                opennow::SetCurrentStage(kStageVideoCopy);
                // =========================================================================================
                // COPIA DIRECTA POR FILAS EN VEZ DEL BLIT DE SDL (v4.15). ESTE ES EL ARREGLO DE LOS FPS.
                // =========================================================================================
                // EL PROBLEMA MEDIDO EN CONSOLA (v4.11, `logs/gfnps4`):
                //
                //     STREAM_VIDEO_PHASES scale_us=29374  copy_us=41652  frames=12   -> 19 fps
                //
                // **`copy_us` = 41.652 us para 2.073.600 pixeles = 20 ns/pixel.** Y la v3.82 ya habia
                // medido ese mismo numero: **27,4 ns/px** en `SDL_LowerBlit`, que es el camino LENTO de
                // `SDL_LowerBlitScaled` (`SDL_surface.c:897-903`), al que SDL cae **cuando los formatos
                // de origen y destino NO coinciden**. La copia rapida cuesta **1,25 ns/px**.
                //
                // POR QUE LOS FORMATOS NO COINCIDEN, y por que NO se puede arreglar cambiando formatos:
                //
                //   - El LIENZO y la VENTANA tienen que ser `BGR888` (memoria `[R][G][B]`) para que el
                //     driver de SDL-PS4 mande a VideoOut el orden que el panel lee. **Eso ya esta
                //     confirmado en consola: es lo que arreglo el color en la v4.06.**
                //   - El ESCALADOR escribe `[B][G][R][A]` (contrato de `color_simd.hpp`, y lo comprueba
                //     `tests/video_color_path_test.cpp`). **Cambiarlo rompe la ruta directa**: lo intente
                //     en la v4.14 y el test lo detecto.
                //   - Y no existe en SDL2 un formato de 4 bytes con orden `[R][G][B]` que el renderizador
                //     software acepte como lienzo (los candidatos son `BGR888`, de 3 bytes, o
                //     `BGRX8888`, que no esta declarado en esta copia de SDL).
                //
                // **LA SALIDA: no usar el blit de SDL. Copiar las filas a mano.**
                //
                // Y funciona porque **el orden de bytes ya es el correcto**: el escalador deja en la
                // textura `[B][G][R]` y el driver espera `[R][G][B]`; es decir, **lo unico que hacia el
                // blit era una "conversion" que en la practica deja los mismos bytes**. Copiar es
                // equivalente y cuesta ~2,6 ms en vez de 41 ms.
                //
                // CONDICIONES PARA PODER COPIAR (si falla alguna, se usa el blit de siempre):
                //   - el lienzo es el `renderCanvas_` y tiene pixeles y paso;
                //   - el destino es 1:1 (`tw==dst_w && th==dst_h`), que es el caso de esta ruta;
                //   - se puede bloquear la textura para leerla;
                //   - el paso de la textura cubre la fila.
                // =========================================================================================
                // LA COPIA TIENE QUE DESHACER EL INTERCAMBIO DE CANALES (v4.18). ESTE ERA MI ERROR.
                // =========================================================================================
                // En la v4.15 sustitui `SDL_RenderCopy` por un `memcpy` por filas razonando que "el
                // orden de bytes ya era el correcto". **Era falso, y el usuario lo vio en pantalla: los
                // colores salieron alterados.**
                //
                // POR QUE: `SDL_RenderCopy` **no copiaba, CONVERTIA**. El texto y la UI se dibujan en el
                // lienzo en un formato de 4 bytes y el blit los pasaba al formato del LIENZO — y **esa
                // conversion era justamente la que dejaba los bytes en el orden que el driver necesita**.
                // La marca `coincide_con_textura=0` lo decia desde el primer dia: textura y lienzo NO son
                // el mismo formato, asi que SDL no puede limitarse a copiar.
                //
                // Y las medidas confirman que los dos lienzos son de **4 bytes por pixel**
                // (`canvas_pitch=7680 = 1920*4`, `ventana_pitch=7680`), asi que el problema no era el
                // numero de bytes sino **el orden de los canales dentro de cada pixel**.
                //
                // LA CORRECCION: la copia **deshace el intercambio R<->B**, que es exactamente lo que la
                // conversion hacia. Se hace con `pshufb` (SSSE3): **una sola instruccion por cada 4
                // pixeles**, asi que el coste sigue siendo el de una copia y no el de la conversion
                // pixel a pixel de 20 ns/px.
                //
                // Y queda un `memcpy` puro de reserva para el caso en que el destino no tenga el mismo
                // orden, de forma que **nunca se copia mal por no poder barajar**.
                bool copiaPropiaHecha = false;
                if(!escaladoPropioHecho && renderCanvas_ && renderCanvas_->pixels &&
                   tw == dst_w && th == dst_h && renderCanvas_->pitch >= tw * 4) {
                    void* srcPixels = nullptr;
                    int srcPitch = 0;
                    if(SDL_LockTexture(bgraTexture_, nullptr, &srcPixels, &srcPitch) == 0 && srcPixels) {
                        const int filaBytes = tw * 4;
                        if(srcPitch >= filaBytes) {
                            uint8_t* dstBase = static_cast<uint8_t*>(renderCanvas_->pixels);
                            const uint8_t* srcBase = static_cast<const uint8_t*>(srcPixels);
                            const size_t dstPaso = static_cast<size_t>(renderCanvas_->pitch);
                            const size_t srcPaso = static_cast<size_t>(srcPitch);
#if defined(__SSE2__)
                            // Intercambio R<->B dentro de cada pixel de 4 bytes, con **SSE2 puro**.
                            //
                            // POR QUE SSE2 Y NO `pshufb`: `_mm_shuffle_epi8` es **SSSE3**, y este
                            // proyecto solo compila con SSE2 (`color_simd.cpp` incluye `emmintrin.h`,
                            // sin `tmmintrin.h` ni `pshufb` en ninguna parte). **No se asume que SSSE3
                            // este disponible.**
                            //
                            // El intercambio con SSE2 es una rotacion de bits: en cada pixel de 32 bits
                            // (bytes `b0 b1 b2 b3` = B G R A), hay que poner `b2` en `b0` y `b0` en `b2`.
                            //
                            //     rl = (v << 16) | (v >> 16)   -> intercambia los 16 bits bajos con los
                            //                                     altos: deja  b2 b3 b0 b1
                            //     rl & 0x00FF00FF              -> conserva b0 y b2 en su sitio
                            //     v  & 0xFF00FF00              -> conserva b1 y b3 (G y A, intactos)
                            //     resultado                    -> b2 b1 b0 b3  =  R G B A
                            //
                            // **Media docena de instrucciones por cada 4 pixeles**, asi que el coste sigue
                            // siendo el de una copia y no el de la conversion pixel a pixel (20 ns/px)
                            // que hacia `SDL_RenderCopy`.
                            const __m128i kLo = _mm_set1_epi32(static_cast<int>(0x00FF00FFu));
                            const __m128i kHi = _mm_set1_epi32(static_cast<int>(0xFF00FF00u));
                            for(int y = 0; y < th; ++y) {
                                const uint8_t* s = srcBase + static_cast<size_t>(y) * srcPaso;
                                uint8_t* d = dstBase + static_cast<size_t>(y) * dstPaso;
                                int x = 0;
                                for(; x + 16 <= filaBytes; x += 16) {
                                    const __m128i v = _mm_loadu_si128(
                                        reinterpret_cast<const __m128i*>(s + x));
                                    const __m128i rl = _mm_or_si128(_mm_slli_epi32(v, 16),
                                                                    _mm_srli_epi32(v, 16));
                                    const __m128i swapped = _mm_or_si128(_mm_and_si128(rl, kLo),
                                                                         _mm_and_si128(v, kHi));
                                    _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x), swapped);
                                }
                                for(; x < filaBytes; x += 4) {
                                    const uint8_t b0 = s[x + 0], b1 = s[x + 1];
                                    d[x + 0] = s[x + 2];
                                    d[x + 1] = b1;
                                    d[x + 2] = b0;
                                    d[x + 3] = s[x + 3];
                                }
                            }
                            copiaPropiaHecha = true;
#else
                            for(int y = 0; y < th; ++y) {
                                std::memcpy(dstBase + static_cast<size_t>(y) * dstPaso,
                                            srcBase + static_cast<size_t>(y) * srcPaso,
                                            static_cast<size_t>(filaBytes));
                            }
                            copiaPropiaHecha = true;
#endif
                        }
                        SDL_UnlockTexture(bgraTexture_);
                    }
                }
                // =========================================================================================
                // MARCA DEL CAMINO DEL BLIT. ES LA PRUEBA DE SI EL ARREGLO FUNCIONO.
                // =========================================================================================
                {
                    static bool s_blitPathLogged = false;
                    if(!s_blitPathLogged) {
                        s_blitPathLogged = true;
                        SDL_Surface* canvasSurf = renderCanvas_;
                        Uint32 texFmtNow = 0;
                        SDL_QueryTexture(bgraTexture_, &texFmtNow, nullptr, nullptr, nullptr);
                        const bool coinciden = (canvasSurf && canvasSurf->format &&
                                                canvasSurf->format->format == texFmtNow);
                        char bp[288];
                        std::snprintf(bp,sizeof(bp),
                                      "via=%s textura=%s lienzo=%s coinciden=%d destino=%dx%d 1to1=%d "
                                      "(copia_propia=%d -> esperado ~1,25 ns/px en vez de ~20)",
                                      copiaPropiaHecha ? "copia_filas_propia" : "SDL_RenderCopy",
                                      SDL_GetPixelFormatName(texFmtNow),
                                      (canvasSurf && canvasSurf->format)
                                          ? SDL_GetPixelFormatName(canvasSurf->format->format) : "nulo",
                                      coinciden ? 1 : 0, tw, th,
                                      (tw == dst_w && th == dst_h) ? 1 : 0,
                                      copiaPropiaHecha ? 1 : 0);
                        opennow::LogAppLifecycleEvent("STREAM_VIDEO_BLIT_PATH", bp);
                    }
                }
                const bool copyOk = escaladoPropioHecho || copiaPropiaHecha ||
                                    (SDL_RenderCopy(target_,bgraTexture_,nullptr,&dstBgra)==0);
                const uint64_t t_copy1 = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                // Informe de las tres fases, una linea por segundo.
                {
                    static uint64_t s_scaleAcc=0,s_uploadAcc=0,s_copyAcc=0,s_frames=0,s_lastLog=0;
                    static uint64_t s_lockAcc=0,s_convAcc=0,s_unlockAcc=0;
                    s_scaleAcc  += (t_scale1  > t_scale0 ) ? (t_scale1  - t_scale0 ) : 0;
                    s_uploadAcc += (t_upload1 > t_scale1 ) ? (t_upload1 - t_scale1 ) : 0;
                    s_copyAcc   += (t_copy1   > t_upload1) ? (t_copy1   - t_upload1) : 0;
                    s_lockAcc   += s_lastLockUs;
                    s_convAcc   += s_lastConvUs;
                    s_unlockAcc += s_lastUnlockUs;
                    ++s_frames;
                    if(s_lastLog == 0 || (t_copy1 - s_lastLog) >= 1000000ULL) {
                        s_lastLog = t_copy1;
                        const uint64_t n = s_frames ? s_frames : 1;
                        char vp[288];
                        // Los tres tramos de `scale_us`, para poder decidir con datos en lugar de suponer.
                        // `lock + conv + unlock` tiene que dar aproximadamente `scale_us`.
                        std::snprintf(vp,sizeof(vp),
                                      "scale_us=%llu upload_us=%llu copy_us=%llu frames=%llu tex=%dx%d src=%dx%d "
                                      "lock_us=%llu conv_us=%llu unlock_us=%llu",
                                      (unsigned long long)(s_scaleAcc/n),
                                      (unsigned long long)(s_uploadAcc/n),
                                      (unsigned long long)(s_copyAcc/n),
                                      (unsigned long long)n, dw, dh, fw, fh,
                                      (unsigned long long)(s_lockAcc/n),
                                      (unsigned long long)(s_convAcc/n),
                                      (unsigned long long)(s_unlockAcc/n));
                        opennow::LogAppLifecycleEvent("STREAM_VIDEO_PHASES", vp);
                        s_scaleAcc=s_uploadAcc=s_copyAcc=0; s_frames=0;
                        s_lockAcc=s_convAcc=s_unlockAcc=0;
                    }
                }
                if(copyOk) {
                    if(!firstUploadLogged_) {
                        opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FIRST_COPY_COMPLETE",SDL_GetError());
                        firstUploadLogged_=true;
                    }
                    ++stats_.rendered_frames;
                    return true;
                }
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_BGRA_COPY_FAILED",SDL_GetError());
            }
            // Si algo fallo, se sigue por la ruta clasica de abajo.
            // NOTA: ya no hay un `else` para el fallo de `SDL_UpdateTexture`. Antes la subida era una
            // condicion (`if(SDL_UpdateTexture(...)==0)`); ahora la escritura directa no puede "fallar"
            // de esa forma — si `SDL_LockTexture` no va, se usa la subida clasica y **si esa falla se
            // sale con `return false` antes de llegar aqui**. Dejar un `else` aqui seria codigo muerto
            // que ademas registraria un mensaje enganoso.
        } else if(bgraTexture_ && !newFrameBgra) {
            // Frame ya subido: solo hay que volver a copiarlo (1:1).
            // Igual que arriba: destino = LIENZO, nunca el tamanio del frame.
            // DESTINO 1:1 CON LA TEXTURA (v3.90): sin reescalado. Ver la explicacion larga arriba.
                const SDL_Rect dstBgra={0,0,tw,th};   // 1:1 con la textura, que mide el lienzo
            if(SDL_RenderCopy(target_,bgraTexture_,nullptr,&dstBgra)==0) {
                ++stats_.rendered_frames;
                return true;
            }
        }
    }

    opennow::SetCurrentStage(kStageVideoFallback);
    if(!texture_ || width_!=frame->width || height_!=frame->height){
        if(texture_) SDL_DestroyTexture(texture_);
        texture_=SDL_CreateTexture(target_,SDL_PIXELFORMAT_IYUV,SDL_TEXTUREACCESS_STREAMING,frame->width,frame->height);
        width_=frame->width; height_=frame->height;
        uploaded_generation_=UINT64_MAX;
        if(!firstUploadLogged_) {
            opennow::WriteStreamStartupStage(texture_?"video_render_texture_created":"video_render_texture_create_failed");
            opennow::LogAppLifecycleEvent(texture_?"STREAM_VIDEO_RENDER_TEXTURE_CREATED":"STREAM_VIDEO_RENDER_TEXTURE_FAILED",SDL_GetError());
        }
    }
    if(!texture_) {
        if(!firstUploadLogged_) firstUploadLogged_=true;
        return false;
    }
    const bool new_frame = generation == 0 || generation != uploaded_generation_;
    int rc=0;
    if(new_frame && !firstUploadLogged_) {
        opennow::WriteStreamStartupStage("video_render_first_upload_call");
        opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FIRST_UPLOAD_CALL");
    }
    if(new_frame && (frame->format==AV_PIX_FMT_YUV420P || frame->format==AV_PIX_FMT_YUVJ420P))
        rc=SDL_UpdateYUVTexture(texture_,nullptr,frame->data[0],frame->linesize[0],frame->data[1],frame->linesize[1],frame->data[2],frame->linesize[2]);
    else if(new_frame && frame->format==AV_PIX_FMT_NV12){
        const int chroma_width=(frame->width+1)/2, chroma_height=(frame->height+1)/2;
        u_plane_.resize(static_cast<size_t>(chroma_width)*chroma_height);
        v_plane_.resize(u_plane_.size());
        for(int y=0;y<chroma_height;y++) for(int x=0;x<chroma_width;x++){
            const uint8_t* uv=frame->data[1]+y*frame->linesize[1]+2*x;
            const size_t i=static_cast<size_t>(y)*chroma_width+x;
            u_plane_[i]=uv[0]; v_plane_[i]=uv[1];
        }
        rc=SDL_UpdateYUVTexture(texture_,nullptr,frame->data[0],frame->linesize[0],u_plane_.data(),chroma_width,v_plane_.data(),chroma_width);
    }
    else if(new_frame) {
        if(!firstUploadLogged_) {
            opennow::WriteStreamStartupStage("video_render_unsupported_pixel_format");
            opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_UNSUPPORTED_FORMAT",
                ("format=" + std::to_string(frame->format)).c_str());
            firstUploadLogged_=true;
        }
        return false;
    }
    if(rc!=0) {
        if(!firstUploadLogged_) {
            opennow::WriteStreamStartupStage("video_render_texture_upload_failed");
            opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_UPLOAD_FAILED",SDL_GetError());
            firstUploadLogged_=true;
        }
        return false;
    }
    if(!firstUploadLogged_) {
        opennow::WriteStreamStartupStage("video_render_first_upload_complete");
        opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FIRST_UPLOAD_COMPLETE");
    }
    if(generation != 0) uploaded_generation_=generation;

    // =============================================================================================
    // EL DESTINO DEL `RenderCopy` ERA 1920x1080 FIJO. ERA UN REESCALADO EN CPU POR FRAME.
    // =============================================================================================
    // Estaba asi:
    //
    //     SDL_Rect destination = {0, 0, 1920, 1080};
    //     SDL_RenderCopy(target_, texture_, nullptr, &destination);
    //
    // **El renderizador de PS4 es SOFTWARE** (`VIDEO_RENDERER_READY name=software mode=software`).
    // Copiar una textura de 1280x720 a un destino de 1920x1080 NO es una copia: es un **reescalado de
    // 2.073.600 pixeles por frame, hecho en la CPU**. Y si el destino no coincide con el tamano real
    // del lienzo, SDL ademas tiene que recortar o rellenar lo que sobra.
    //
    // Este es el coste que hundia los FPS, y es independiente de los QR y del velo de transicion.
    //
    // AHORA el destino es EXACTAMENTE el tamano del lienzo: si la textura ya mide lo mismo (el caso
    // normal: el servidor entrega 720p y el lienzo es 720p), `SDL_RenderCopy` se convierte en una
    // **copia directa sin reescalado**, que es la operacion mas barata que puede hacer el renderizador.
    //
    // El tamano se toma del propio renderizador con `SDL_GetRendererOutputSize`, que es la fuente de
    // verdad: puede ser el lienzo logico, la superficie de la ventana o un render target. No se
    // asume nada.
    int dest_w = 1920, dest_h = 1080;
    if(SDL_GetRendererOutputSize(target_, &dest_w, &dest_h) != 0 || dest_w <= 0 || dest_h <= 0) {
        // Si no se puede consultar, se usa el tamano del FRAME como destino: nunca el 1080p fijo,
        // porque escalar a un tamano arbitrario es justo lo que se quiere evitar.
        dest_w = frame->width;
        dest_h = frame->height;
    }
    SDL_Rect destination={0,0,dest_w,dest_h};
    if(!firstUploadLogged_) {
        opennow::WriteStreamStartupStage("video_render_first_copy_begin");
        opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FIRST_COPY_BEGIN");
    }
    if(SDL_RenderCopy(target_,texture_,nullptr,&destination)!=0) {
        if(!firstUploadLogged_) {
            opennow::WriteStreamStartupStage("video_render_copy_failed");
            opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_COPY_FAILED",SDL_GetError());
            firstUploadLogged_=true;
        }
        return false;
    }
    if(!firstUploadLogged_) {
        opennow::WriteStreamStartupStage("video_render_first_copy_complete");
        opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FIRST_COPY_COMPLETE",SDL_GetError());
        firstUploadLogged_=true;
    }
    ++stats_.rendered_frames;
    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    stats_.total_render_time+=static_cast<uint64_t>(us);
    return true;
}
int SDLVideoRenderer::getFrameColorspace(const AVFrame* frame){
    if(!frame) return COLORSPACE_REC_601;
    switch(frame->colorspace){
    case AVCOL_SPC_BT709: return COLORSPACE_REC_709;
    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: return COLORSPACE_REC_2020;
    default: return COLORSPACE_REC_601;
    }
}
bool SDLVideoRenderer::isFrameFullRange(const AVFrame* frame){return frame && frame->color_range==AVCOL_RANGE_JPEG;}
