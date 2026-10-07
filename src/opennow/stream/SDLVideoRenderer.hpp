#pragma once
#include "IVideoRenderer.hpp"
#include <SDL2/SDL.h>
#include <vector>

class SDLVideoRenderer final : public IVideoRenderer {
public:
    static void SetRenderTarget(SDL_Renderer* renderer);
    // Necesario para el ESCALADO PROPIO (v3.83): la superficie del lienzo se obtiene de la VENTANA
    // (`SDL_GetWindowSurface`). `SDL_LockTexture` no sirve para esto — espera una `SDL_Texture`, no el
    // renderer — y este SDL no tiene `SDL_RenderGetWindow`. Se guarda como estatico, igual que
    // `target_`, para no cambiar la firma de `drawLatest` (que es una interfaz comun).
    static void SetRenderWindow(SDL_Window* w);
    // =============================================================================================
    // SUPERFICIE DEL LIENZO PROPIO (v3.89)
    // =============================================================================================
    // ES LA CLAVE PARA RECUPERAR LOS 60 FPS SIN PARPADEO. Historia, con las medidas de consola:
    //
    //   v3.62  destino del blit = FRAME  -> 1:1, SIN escalar, video en la esquina -> **59 fps**
    //   v3.83  escalador propio en 4 bytes escribiendo en la superficie de la VENTANA -> **60 fps**
    //          pero **con parpadeo**: esa superficie es la misma memoria que el driver copia a VideoOut
    //   v3.84  lienzo propio + copia del lienzo a la ventana -> **20 fps**: la copia añade 8,3 MB/frame
    //   v3.80-82  SDL escalando con `SDL_RenderCopy` -> 19-20 fps: el destino nunca es 1:1, y eso mete
    //          a SDL en su camino de reescalado CON CONVERSION POR PIXEL
    //
    // LA COMBINACION QUE FALTABA: **el escalador rapido de la v3.83 escribiendo en el LIENZO PROPIO**
    // (que es memoria normal, no la que el driver esta copiando). Asi:
    //
    //   - No hay parpadeo (no se toca la memoria compartida con el driver).
    //   - No hay copia extra del lienzo (el renderizador y el escalador usan LA MISMA superficie).
    //   - No se pasa por el camino de conversion por pixel de SDL.
    static void SetRenderCanvas(SDL_Surface* s);
    static SDL_Surface* renderCanvas_;
    ~SDLVideoRenderer() override;
    void draw(NVGcontext*,int,int,AVFrame*,int) override;
    bool drawLatest(NVGcontext*,int,int,AVFrame*,int,uint64_t) override;
    VideoRenderStats* video_render_stats() override { return &stats_; }
    int getFrameColorspace(const AVFrame* frame) override;
    bool isFrameFullRange(const AVFrame* frame) override;
private:
    static SDL_Renderer* target_;
    static SDL_Window* renderWindow_;
    SDL_Texture* texture_=nullptr;
    int width_=0,height_=0;
    uint64_t uploaded_generation_=UINT64_MAX;
    std::vector<uint8_t> u_plane_,v_plane_;
    VideoRenderStats stats_{};
    bool firstUploadLogged_=false;
    bool invalidFrameLogged_=false;

    // =========================================================================================
    // RUTA BGRA PROPIA (opcional, activada por el fichero /data/gfnps4/video_bgra.flag)
    // =========================================================================================
    // MOTIVO: el renderizador SOFTWARE de SDL **no soporta texturas IYUV** (SW_CreateTexture las
    // respalda con un `SDL_Surface`, y `IsSupportedFormat` falla), asi que SDL crea una textura RGB
    // "nativa" y **convierte los planos YUV a RGB en CADA `SDL_UpdateYUVTexture`**
    // (`SDL_render.c:1149` y `:1444`, verificado leyendo el codigo de SDL-PS4).
    //
    // Eso es una conversion de imagen completa por frame, hecha con codigo generico de SDL.
    //
    // ALTERNATIVA: convertir el frame a BGRA con el conversor SSE2 del PROPIO proyecto
    // (`opennow::color::ConvertNV12ToBGRA_BT709`, en color_simd.hpp) y subirlo con
    // `SDL_UpdateTexture`. Entonces SDL recibe RGB, su formato nativo coincide y **se salta su
    // conversion**; solo hace copia de memoria.
    //
    // POR QUE DETRAS DE UN FLAG Y NO DIRECTAMENTE: si el coste real esta en el blit a 1080p y no en
    // la conversion, este cambio no aportaria nada y habria movido la ruta de video —la parte mas
    // delicada del cliente— sin beneficio. Con el flag se comparan las DOS rutas en la misma consola
    // y se elige con el numero delante.
    //
    // Para activarla: crear un fichero VACIO en /data/gfnps4/video_bgra.flag
    std::vector<uint8_t> bgra_;
    SDL_Texture* bgraTexture_=nullptr;
    int bgraWidth_=0,bgraHeight_=0;
    uint64_t bgraGeneration_=UINT64_MAX;

    // =============================================================================================
    // ESCALADO PROPIO EN 4 BYTES (v3.83)
    // =============================================================================================
    // El blit escalado de SDL cuesta **27,4 ns por pixel de destino** en la PS4 (medido: `copy_us`
    // ~59.400 us para 2.073.600 px), frente a **1,25 ns/px** que cuesta una simple limpieza de la misma
    // superficie. El motivo: el formato del lienzo (`BGR888`, 3 bytes, declarado por el driver de
    // SDL-PS4) NO coincide con el de la textura (`ARGB8888`, 4 bytes), asi que SDL cae a
    // `SDL_LowerBlit` y **convierte pixel a pixel**.
    //
    // Como el driver ademas **recorre el lienzo con paso de 32 bits** (`uint32_t* pDst`), tratarlo como
    // de 4 bytes es lo correcto. Aqui se escribe el video directamente en la superficie del lienzo con
    // `Uint32` y replicacion de columnas precalculada.
    //
    // El mapa de columnas se recalcula SOLO cuando cambia la geometria: si el servidor baja de 720p a
    // 540p (resolucion dinamica), se rehace una vez y se reutiliza.
    std::vector<int> scaleMapX_;
    int scaleMapW_=-1, scaleMapH_=-1, scaleMapDW_=-1, scaleMapDH_=-1;
    bool firstOwnScaleLogged_=false;
    // Geometria del blit ya registrada (v3.90): sirve para escribir una sola linea de log cuando
    // cambia, y asi poder comprobar si el blit es 1:1 (rapido) o escalado (lento).
    int loggedBlitW_=-1, loggedBlitH_=-1, loggedBlitSrcW_=-1, loggedBlitSrcH_=-1;
    // Marca de un solo registro para el aviso de escalado durante la conversion (v3.90b).
    bool escaladoEnConversionLogged_=false;
    // =================================================================================================
    // DESGLOSE DE `scale_us` EN SUS TRES PARTES (v4.12)
    // =================================================================================================
    // `scale_us` cubre `SDL_LockTexture` + la conversion/escalado SSE2 + `SDL_UnlockTexture`, y en el
    // renderizador software **el unlock copia el buffer bloqueado a la textura**. Medido en la v4.06:
    // `scale_us` ≈ 34.000-45.000 us sobre un presupuesto de 16.666 us. **Sin separar las tres no se
    // puede saber cual se lo lleva** ni, por tanto, que hay que atacar.
    uint64_t s_lastLockUs=0, s_lastConvUs=0, s_lastUnlockUs=0;
};
