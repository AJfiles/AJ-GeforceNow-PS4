#pragma once

#include "IVideoRenderer.hpp"
#include <cstdint>
#include <cstddef>
#include <atomic>

#ifdef __ORBIS__
#include <vector>
#endif

namespace opennow {

class PS4VideoOutRenderer final : public IVideoRenderer {
public:
    static std::atomic<int> g_net_fps;
    static std::atomic<int> g_net_decode_fps;
    static std::atomic<int> g_net_kbps;
    static std::atomic<int> g_net_rtt;
    static std::atomic<int> g_net_res_w;
    static std::atomic<int> g_net_res_h;

    // El valor por defecto es 1280x720 (no 1920x1080) a proposito: 720p es la resolucion base del
    // stream de GeForce NOW y la que menos escalado exige. Las llamadas reales pasan siempre la
    // resolucion configurada explicitamente (ver el handoff en main.cpp).
    static bool Initialize(int width = 1280, int height = 720);
    static bool Present(AVFrame* frame);
    static void PresentTestPattern();
    static void Shutdown();
    static bool IsReady();
    static bool WaitForFlipsSettled(uint32_t timeout_ms, const char* reason);
    // A failed flip resubmit cycle means the display pipe is gone; the caller
    // must recreate VideoOut and its framebuffers instead of retrying forever.
    static bool NeedsFullRecreate();
    static void RequestFullRecreate();
    static void ClearFullRecreate();
    static uint32_t GetConsecutiveFlipFails();
    static uint32_t GetPresentedFrames();
    // Tamano REAL del framebuffer registrado en VideoOut (v4.25).
    //
    // Existe para la traza detallada: sin esto, la fila por frame no puede decir si hubo escalado en
    // CPU, porque el destino de la conversion es el framebuffer y **no coincide necesariamente con el
    // tamano del frame del servidor**. Y `scaled=1` es justo lo que hay que detectar: el escalado en
    // CPU es el enemigo de los 60 fps (31.000 us medidos escalando a 1920x1080).
    //
    // Devuelve `false` si VideoOut no esta listo, y en ese caso deja los valores como estaban.
    static bool GetFramebufferSize(int& outW, int& outH);
    static void SetInGameMenuActive(bool active);
    static bool IsInGameMenuActive();
    static void ToggleInGameMenu();
    static void SetMenuOverlayBuffer(const uint8_t* bgra_pixels, int width, int height, int pitch);
    static void SetExitCardBuffer(const uint8_t* bgra_pixels, int width, int height, int pitch);
    static bool BlitExitCardAndFlip();
    static void SetStatsHudActive(bool active);
    static bool IsStatsHudActive();
    static void ToggleStatsHud();
    static void UpdateLiveStats(int fps, int decodeFps, int kbps, int rtt, int resW, int resH);

    ~PS4VideoOutRenderer() override;
    void draw(NVGcontext*, int, int, AVFrame*, int) override;
    bool drawLatest(NVGcontext*, int, int, AVFrame*, int, uint64_t) override;
    VideoRenderStats* video_render_stats() override { return &stats_; }
    int getFrameColorspace(const AVFrame*) override { return COLORSPACE_REC_709; }
    bool isFrameFullRange(const AVFrame*) override { return false; }

private:
    static bool AllocateDirectMemory(size_t total_bytes);
    static void FreeDirectMemory();
    static int PickFreeBuffer();

    VideoRenderStats stats_{};
};

} // namespace opennow
