#include "PS4VideoOutRenderer.hpp"

#include "../session_recorder.hpp"
#include "color_simd.hpp"
#include "../stream_startup_diagnostics.hpp"

#ifdef __ORBIS__
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#endif

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>

namespace opennow {

std::atomic<int> PS4VideoOutRenderer::g_net_fps{60};
std::atomic<int> PS4VideoOutRenderer::g_net_decode_fps{60};
std::atomic<int> PS4VideoOutRenderer::g_net_kbps{0};
std::atomic<int> PS4VideoOutRenderer::g_net_rtt{0};
std::atomic<int> PS4VideoOutRenderer::g_net_res_w{1280};
std::atomic<int> PS4VideoOutRenderer::g_net_res_h{720};

namespace {

// Four buffers, not three.
//
// Field evidence that forced this: with kNumBuffers=3 and the stream running at
// 38-42 FPS on a 60 Hz panel, the flip queue sat at pending=1 in 243 of 243
// samples and the same physical buffer was rewritten frame after frame, which is
// the horizontal tear (top half stale, bottom half current) seen in the user's
// screenshots. Writing a 1280x720 frame costs ~14 ms of CPU, i.e. most of a 60 Hz
// period, so a three-buffer ring leaves no margin: the DCE reaches the buffer being
// written before the write finishes.
//
// A fourth buffer adds one full frame of slack, which is what triple buffering is
// supposed to provide but cannot here because the present rate tracks the display
// rate. Memory is not a constraint: BOOT_MEMORY reports 4608 MB of direct memory
// and each 1080p BGRA buffer costs 7.91 MB (3.52 MB at 720p).
constexpr int kNumBuffers = 4;
constexpr size_t kDmemAlign = 0x200000; // 2MB superpage align

#ifdef __ORBIS__
static int32_t s_video_handle = -1;
static off_t s_dmem_offset = 0;
static size_t s_dmem_total_size = 0;
static void* s_dmem_cpu_addr = nullptr;
static void* s_framebuffers[kNumBuffers] = {};
static int s_current_buffer_idx = 0;
static int s_last_flip_idx = -1;
static uint32_t s_consecutive_flip_fails = 0;
static uint32_t s_presented_frames = 0;
static uint32_t s_dropped_frames = 0;
static uint64_t s_flip_vblank_wait_us = 0;
// Set once the display pipe delivered an unrecoverable flip failure. No amount
// of extra presents can revive it: the VideoOut handle and its buffers must be
// destroyed and recreated from scratch.
static std::atomic<bool> s_request_full_recreate{false};

// Flip-completion synchronisation.
//
// The tearing could not be fixed by reasoning about sceVideoOutGetFlipStatus():
// the fields are only a snapshot, and between reading currentBuffer and finishing
// the CPU conversion (14-30 ms, i.e. one to two 60 Hz periods) the DCE advances to
// another buffer. Measured before this change: 14 of 309 flips wrote into the
// buffer that was on screen at submit time.
//
// The correct primitive is the VideoOut flip event queue: the kernel signals the
// equeue when a submitted flip has actually completed, which is the only reliable
// statement about which buffer is free. sceVideoOutWaitVblank() cannot be used for
// this - it returns in ~20 us on this firmware instead of blocking (measured in the
// client and in the standalone probe).
static OrbisKernelEqueue s_flip_equeue = 0;
static bool s_flip_equeue_ready = false;
static uint32_t s_flip_sync_waits = 0;
static uint32_t s_flip_sync_timeouts = 0;
static uint64_t s_flip_sync_total_us = 0;

// Monotonic flip argument. See NextFlipArg() below.
static int64_t s_flip_arg = 0;

static int s_width = 1920;
static int s_height = 1080;
static size_t s_pitch = 1920 * 4;
static size_t s_per_frame_bytes = 1920 * 1080 * 4;

static std::atomic<bool> s_shutting_down{false};
static std::atomic<bool> s_in_game_menu_active{false};
static uint32_t s_menu_draw_count = 0;
static uint32_t s_menu_bitmap_serial = 0;
static std::atomic<bool> s_stats_hud_active{false};

static uint64_t s_perf_sample_start_ms = 0;
static uint32_t s_perf_frames_in_sample = 0;
static uint32_t s_perf_dropped_in_sample = 0;
static uint64_t s_total_present_us = 0;
static uint64_t s_max_present_us = 0;
// Per-stage breakdown of Present(). Added because VIDEOOUT_PRESENT_US measured
// avg_us=600393 (600 ms!) while every component that had its own instrumentation
// (direct conversion 1.4 ms, flip sync 1.2 ms) was fast, so the cost had to be in a
// stage that was never timed. Without this the only way forward was guessing.
static uint64_t s_stage_pick_us = 0;
static uint64_t s_stage_convert_us = 0;
static uint64_t s_stage_flip_us = 0;
static uint64_t s_stage_vblank_us = 0;
static uint32_t s_stage_samples = 0;
#endif

static bool s_is_ready = false;

inline uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Monotonic flip argument.
//
// sceVideoOutSubmitFlip()'s last parameter is the flip argument, and the kernel
// reports it back in the flip event's data field. Every call site used to pass a
// constant 0, which makes all events indistinguishable - so sceKernelWaitEqueue
// simply drained already-completed events instead of waiting for the flip we care
// about. Measured consequence: VIDEOOUT_FLIP_SYNC last_us=6, i.e. it never blocked.
static int64_t NextFlipArg() { return ++s_flip_arg; }

// Waits for one specific flip argument to complete, bounded by a timeout.
// Returns true only when that exact flip was observed; on timeout the caller
// proceeds, because a hang would be far worse than a torn frame.
static bool WaitForAnyFlip(uint32_t timeout_us, const char* stage) {
    if (!s_flip_equeue_ready) return false;
    const uint64_t t0 = now_us();
    OrbisKernelEvent ev{};
    int out_count = 0;
    OrbisKernelUseconds wait_us = static_cast<OrbisKernelUseconds>(timeout_us);
    const int rc = sceKernelWaitEqueue(s_flip_equeue, &ev, 1, &out_count, &wait_us);
    const bool got = (rc == 0 && out_count > 0);
    const uint64_t sync_us = now_us() - t0;
    s_flip_sync_total_us += sync_us;
    if (got) s_flip_sync_waits++; else s_flip_sync_timeouts++;
    const uint32_t total = s_flip_sync_waits + s_flip_sync_timeouts;
    if (total % 120 == 0) {
        char syncDetail[200];
        std::snprintf(syncDetail, sizeof(syncDetail),
                      "stage=%s waits=%u timeouts=%u last_us=%llu avg_us=%llu ev_ident=%llu ev_data=%lld",
                      stage, s_flip_sync_waits, s_flip_sync_timeouts,
                      static_cast<unsigned long long>(sync_us),
                      static_cast<unsigned long long>(s_flip_sync_total_us / (total ? total : 1)),
                      static_cast<unsigned long long>(ev.ident),
                      static_cast<long long>(ev.data));
        LogAppLifecycleEvent("VIDEOOUT_FLIP_SYNC", syncDetail);
    }
    return got;
}

// 5x7 font glyph table for menu and HUD
static const unsigned char* exit_glyph(char c) {
    static const unsigned char blank[7] = {0,0,0,0,0,0,0};
    #define EG(ch,a,b,c,d,e,f,g) case ch: { static const unsigned char v[7]={a,b,c,d,e,f,g}; return v; }
    switch (c) {
    EG('A',14,17,17,31,17,17,17) EG('B',30,17,17,30,17,17,30)
    EG('C',14,17,16,16,16,17,14) EG('D',30,17,17,17,17,17,30)
    EG('E',31,16,16,30,16,16,31) EG('F',31,16,16,30,16,16,16)
    EG('G',14,17,16,23,17,17,15) EG('H',17,17,17,31,17,17,17)
    EG('I',14,4,4,4,4,4,14)       EG('J',7,2,2,2,18,18,12)
    EG('K',17,18,20,24,20,18,17) EG('L',16,16,16,16,16,16,31)
    EG('M',17,27,21,21,17,17,17) EG('N',17,25,21,19,17,17,17)
    EG('O',14,17,17,17,17,17,14) EG('P',30,17,17,30,16,16,16)
    EG('Q',14,17,17,17,21,18,13) EG('R',30,17,17,30,20,18,17)
    EG('S',15,16,16,14,1,1,30)   EG('T',31,4,4,4,4,4,4)
    EG('U',17,17,17,17,17,17,14) EG('V',17,17,17,17,17,10,4)
    EG('W',17,17,17,21,21,21,10) EG('X',17,17,10,4,10,17,17)
    EG('Y',17,17,10,4,4,4,4)     EG('Z',31,1,2,4,8,16,31)
    EG('0',14,17,19,21,25,17,14) EG('1',4,12,4,4,4,4,14)
    EG('2',14,17,1,2,4,8,31)     EG('3',30,1,1,14,1,1,30)
    EG('4',2,6,10,18,31,2,2)     EG('5',31,16,16,30,1,1,30)
    EG('6',14,16,16,30,17,17,14) EG('7',31,1,2,4,8,8,8)
    EG('8',14,17,17,14,17,17,14) EG('9',14,17,17,15,1,1,14)
    EG(' ',0,0,0,0,0,0,0)        EG(':',0,12,12,0,12,12,0)
    EG('[',14,8,8,8,8,8,14)      EG(']',14,2,2,2,2,2,14)
    EG('|',4,4,4,4,4,4,4)        EG('-',0,0,0,31,0,0,0)
    EG('/',1,2,4,8,16,0,0)       EG('.',0,0,0,0,0,12,12)
    default: return blank;
    }
    #undef EG
}

static void draw_prompt_text(uint8_t* dst, int pitch, int dst_w, int dst_h,
                             int start_x, int start_y, const char* str, int scale,
                             uint8_t b, uint8_t g, uint8_t r) {
    int cur_x = start_x;
    for (const char* p = str; p && *p; ++p) {
        char ch = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 32) : *p;
        const unsigned char* glyph = exit_glyph(ch);
        for (int gy = 0; gy < 7; gy++) {
            for (int gx = 0; gx < 5; gx++) {
                if (glyph[gy] & (1 << (4 - gx))) {
                    for (int sy = 0; sy < scale; sy++) {
                        int py = start_y + gy * scale + sy;
                        if (py < 0 || py >= dst_h) continue;
                        uint8_t* row = dst + static_cast<size_t>(py) * pitch;
                        for (int sx = 0; sx < scale; sx++) {
                            int px = cur_x + gx * scale + sx;
                            if (px < 0 || px >= dst_w) continue;
                            uint8_t* px_ptr = row + static_cast<size_t>(px) * 4;
                            px_ptr[0] = b;
                            px_ptr[1] = g;
                            px_ptr[2] = r;
                            px_ptr[3] = 0xFF;
                        }
                    }
                }
            }
        }
        cur_x += (5 + 1) * scale;
    }
}

// Pre-rendered In-Game Menu overlay buffer (provided by UI with Gontserrat font)
static std::vector<uint8_t> s_menu_overlay_pixels;
static int s_menu_overlay_w = 0;
static int s_menu_overlay_h = 0;
static int s_menu_overlay_pitch = 0;

static void BlitInGameMenuOverlay(uint8_t* dst, int pitch, int width, int height) {
    if (!dst || width <= 0 || height <= 0) return;
    if (s_menu_overlay_pixels.empty() || s_menu_overlay_w <= 0 || s_menu_overlay_h <= 0) {
        return;
    }

    const int box_w = s_menu_overlay_w;
    const int box_h = s_menu_overlay_h;
    const int box_x = (width - box_w) / 2;
    const int box_y = (height - box_h) / 2;

    // =============================================================================================
    // COMPROBACION HORIZONTAL QUE FALTABA (v3.96). CORRUPCION DE MEMORIA POTENCIAL.
    // =============================================================================================
    // El bucle de abajo comprobaba los limites VERTICALES (`dst_y < 0 || dst_y >= height`) pero **NO los
    // horizontales**. Con `box_w > width` (overlay mas ancho que el framebuffer):
    //
    //     box_x = (width - box_w) / 2   ->  NEGATIVO
    //     dst_row = dst + dst_y*pitch + box_x*4   ->  **antes del inicio del framebuffer**
    //     memcpy(dst_row, src_row, box_w*4)
    //
    // -> **se escribe fuera del buffer**. Y el tamano de la copia tampoco se acotaba, asi que un overlay
    // mas ancho que el framebuffer desbordaba por la derecha aunque `box_x` fuera >= 0.
    //
    // Con los tamanios actuales (overlay 820x300, framebuffer 1280x720) **no ocurre**, pero es
    // exactamente la clase de fallo cuyo sintoma es un cierre sin traza: corrompe memoria de otro sitio
    // y el proceso muere lejos de la causa. Se acota la copia con `clip_x`/`clip_w`.
    const int clip_x = (box_x > 0) ? box_x : 0;
    const int right_edge = box_x + box_w;
    const int clip_right = (right_edge < width) ? right_edge : width;
    const int clip_w = clip_right - clip_x;
    if (clip_w <= 0) return;
    const int skip_x = clip_x - box_x;   // columnas del origen que quedan fuera por la izquierda

    const uint8_t* src = s_menu_overlay_pixels.data();
    const size_t copy_bytes = static_cast<size_t>(clip_w) * 4;

    for (int y = 0; y < box_h; ++y) {
        const int dst_y = box_y + y;
        if (dst_y < 0 || dst_y >= height) continue;
        uint8_t* dst_row = dst + static_cast<size_t>(dst_y) * pitch + static_cast<size_t>(clip_x) * 4;
        const uint8_t* src_row = src + static_cast<size_t>(y) * s_menu_overlay_pitch
                                     + static_cast<size_t>(skip_x) * 4;
        std::memcpy(dst_row, src_row, copy_bytes);
    }
}

struct HudSnapshot {
    int fps = 0;
    int decode_fps = 0;
    int kbps = 0;
    int rtt = 0;
    int res_w = 1280;
    int res_h = 720;
};

static std::vector<uint8_t> s_hud_overlay_pixels(260 * 90 * 4, 0);
static bool s_hud_blit_logged = false;

static void PreRenderStatsHudBuffer() {
    constexpr int hud_w = 260;
    constexpr int hud_h = 90;
    constexpr int pitch = hud_w * 4;

    HudSnapshot snapshot;
    snapshot.fps = PS4VideoOutRenderer::g_net_fps.load(std::memory_order_relaxed);
    snapshot.decode_fps = PS4VideoOutRenderer::g_net_decode_fps.load(std::memory_order_relaxed);
    snapshot.kbps = PS4VideoOutRenderer::g_net_kbps.load(std::memory_order_relaxed);
    snapshot.rtt = PS4VideoOutRenderer::g_net_rtt.load(std::memory_order_relaxed);
    snapshot.res_w = PS4VideoOutRenderer::g_net_res_w.load(std::memory_order_relaxed);
    snapshot.res_h = PS4VideoOutRenderer::g_net_res_h.load(std::memory_order_relaxed);

    uint8_t* dst = s_hud_overlay_pixels.data();

    // Solid dark Navy background (#0A0E15) with golden border
    for (int y = 0; y < hud_h; y++) {
        uint8_t* row = dst + static_cast<size_t>(y) * pitch;
        const bool is_border = (y == 0 || y == hud_h - 1);
        for (int x = 0; x < hud_w; x++) {
            uint8_t* px = row + static_cast<size_t>(x) * 4;
            if (is_border || x == 0 || x == hud_w - 1) {
                px[0] = 50; px[1] = 140; px[2] = 180; px[3] = 0xFF; // Golden border
            } else {
                px[0] = 21; px[1] = 14; px[2] = 10; px[3] = 0xFF;  // Solid Navy #0A0E15
            }
        }
    }

    constexpr int scale = 2;
    char line1[48], line2[48], line3[48];
    std::snprintf(line1, sizeof(line1), "FPS:%d DEC:%d", snapshot.fps, snapshot.decode_fps);
    std::snprintf(line2, sizeof(line2), "RTT:%dMS BIT:%dK", snapshot.rtt, snapshot.kbps);
    std::snprintf(line3, sizeof(line3), "%dx%d", snapshot.res_w, snapshot.res_h);

    draw_prompt_text(dst, pitch, hud_w, hud_h, 12, 10, line1, scale, 66, 255, 118);
    draw_prompt_text(dst, pitch, hud_w, hud_h, 12, 36, line2, scale, 255, 220, 80);
    draw_prompt_text(dst, pitch, hud_w, hud_h, 12, 62, line3, scale, 255, 255, 255);

    char snapDetail[128];
    std::snprintf(snapDetail, sizeof(snapDetail), "fps=%d decode=%d kbps=%d rtt=%d res=%dx%d",
                  snapshot.fps, snapshot.decode_fps, snapshot.kbps, snapshot.rtt, snapshot.res_w, snapshot.res_h);
    LogAppLifecycleEvent("VIDEOOUT_HUD_SNAPSHOT", snapDetail);
    LogAppLifecycleEvent("VIDEOOUT_HUD_REGENERATED", "reason=activation");
}

static void DrawStatsHudOverlay(uint8_t* dst, int pitch, int width, int height) {
    constexpr int hud_w = 260;
    constexpr int hud_h = 90;
    const int hud_x = width - hud_w - 20;
    const int hud_y = 20;

    if (!dst || s_hud_overlay_pixels.size() != static_cast<size_t>(hud_w * hud_h * 4)) {
        LogAppLifecycleEvent("VIDEOOUT_HUD_BLIT_SKIPPED", "reason=invalid_buffer");
        return;
    }

    if (hud_x < 0 || hud_y < 0 || hud_x + hud_w > width || hud_y + hud_h > height) {
        LogAppLifecycleEvent("VIDEOOUT_HUD_BLIT_SKIPPED", "reason=out_of_bounds");
        return;
    }

    if (!s_hud_blit_logged) {
        s_hud_blit_logged = true;
        char blitDetail[64];
        std::snprintf(blitDetail, sizeof(blitDetail), "offset_x=%d offset_y=%d", hud_x, hud_y);
        LogAppLifecycleEvent("VIDEOOUT_HUD_BLIT_OK", blitDetail);
    }

    const uint8_t* src = s_hud_overlay_pixels.data();
    const size_t copy_bytes = static_cast<size_t>(hud_w) * 4;

    // Fast static row memcpy
    for (int y = 0; y < hud_h; y++) {
        const int dst_y = hud_y + y;
        if (dst_y < 0 || dst_y >= height) continue;
        uint8_t* dst_row = dst + static_cast<size_t>(dst_y) * pitch + static_cast<size_t>(hud_x) * 4;
        const uint8_t* src_row = src + static_cast<size_t>(y) * copy_bytes;
        std::memcpy(dst_row, src_row, copy_bytes);
    }
}

} // namespace

PS4VideoOutRenderer::~PS4VideoOutRenderer() = default;

void PS4VideoOutRenderer::draw(NVGcontext*, int, int, AVFrame* frame, int) {
    Present(frame);
}

bool PS4VideoOutRenderer::drawLatest(NVGcontext*, int, int, AVFrame* frame, int, uint64_t) {
    if (!frame) return false;
    Present(frame);
    stats_.rendered_frames = s_presented_frames;
    return true;
}

bool PS4VideoOutRenderer::IsReady() {
    return s_is_ready;
}

bool PS4VideoOutRenderer::NeedsFullRecreate() {
#ifdef __ORBIS__
    return s_request_full_recreate.load(std::memory_order_acquire);
#else
    return false;
#endif
}

void PS4VideoOutRenderer::RequestFullRecreate() {
#ifdef __ORBIS__
    s_request_full_recreate.store(true, std::memory_order_release);
#endif
}

void PS4VideoOutRenderer::ClearFullRecreate() {
#ifdef __ORBIS__
    s_request_full_recreate.store(false, std::memory_order_release);
    s_consecutive_flip_fails = 0;
#endif
}

uint32_t PS4VideoOutRenderer::GetConsecutiveFlipFails() {
#ifdef __ORBIS__
    return s_consecutive_flip_fails;
#else
    return 0;
#endif
}

uint32_t PS4VideoOutRenderer::GetPresentedFrames() {
#ifdef __ORBIS__
    return s_presented_frames;
#else
    return 0;
#endif
}

bool PS4VideoOutRenderer::GetFramebufferSize(int& outW, int& outH) {
    // El framebuffer se fija UNA VEZ al arrancar el stream y no cambia durante la sesion (ver el bloque
    // de `Initialize`), asi que leer `s_width`/`s_height` es suficiente y no hace falta candado.
    if (!s_is_ready) return false;
    if (s_width <= 0 || s_height <= 0) return false;
    outW = s_width;
    outH = s_height;
    return true;
}

void PS4VideoOutRenderer::SetInGameMenuActive(bool active) {
#ifdef __ORBIS__
    s_in_game_menu_active.store(active, std::memory_order_release);
    s_menu_draw_count = 0;
    ++s_menu_bitmap_serial; // Reopening the menu must always re-apply the overlay.
#endif
}

bool PS4VideoOutRenderer::IsInGameMenuActive() {
#ifdef __ORBIS__
    return s_in_game_menu_active.load(std::memory_order_acquire);
#else
    return false;
#endif
}

void PS4VideoOutRenderer::ToggleInGameMenu() {
#ifdef __ORBIS__
    const bool cur = s_in_game_menu_active.load(std::memory_order_acquire);
    s_in_game_menu_active.store(!cur, std::memory_order_release);
    s_menu_draw_count = 0;
#endif
}

void PS4VideoOutRenderer::SetMenuOverlayBuffer(const uint8_t* bgra_pixels, int width, int height, int pitch) {
#ifdef __ORBIS__
    if (!bgra_pixels || width <= 0 || height <= 0 || pitch <= 0) {
        s_menu_overlay_pixels.clear();
        ++s_menu_bitmap_serial;
        s_menu_overlay_w = 0;
        s_menu_overlay_h = 0;
        s_menu_overlay_pitch = 0;
        return;
    }
    s_menu_overlay_w = width;
    s_menu_overlay_h = height;
    s_menu_overlay_pitch = pitch;
    const size_t total_bytes = static_cast<size_t>(pitch) * height;
    s_menu_overlay_pixels.resize(total_bytes);
    std::memcpy(s_menu_overlay_pixels.data(), bgra_pixels, total_bytes);
    ++s_menu_bitmap_serial;
#endif
}

// Fullscreen Exit Card Transition Buffer
static std::vector<uint8_t> s_exit_card_pixels;
static int s_exit_card_w = 0;
static int s_exit_card_h = 0;
static int s_exit_card_pitch = 0;

void PS4VideoOutRenderer::SetExitCardBuffer(const uint8_t* bgra_pixels, int width, int height, int pitch) {
#ifdef __ORBIS__
    if (!bgra_pixels || width <= 0 || height <= 0 || pitch <= 0) {
        s_exit_card_pixels.clear();
        s_exit_card_w = 0;
        s_exit_card_h = 0;
        s_exit_card_pitch = 0;
        return;
    }
    s_exit_card_w = width;
    s_exit_card_h = height;
    s_exit_card_pitch = pitch;
    const size_t total_bytes = static_cast<size_t>(pitch) * height;
    s_exit_card_pixels.resize(total_bytes);
    std::memcpy(s_exit_card_pixels.data(), bgra_pixels, total_bytes);
#endif
}

bool PS4VideoOutRenderer::BlitExitCardAndFlip() {
#ifdef __ORBIS__
    if (!s_is_ready || s_video_handle <= 0 || !s_framebuffers[s_current_buffer_idx]) return false;
    if (s_exit_card_pixels.empty() || s_exit_card_w <= 0 || s_exit_card_h <= 0) return false;

    uint8_t* dst = static_cast<uint8_t*>(s_framebuffers[s_current_buffer_idx]);
    const int copy_w = std::min(s_width, s_exit_card_w);
    const int copy_h = std::min(s_height, s_exit_card_h);
    const size_t line_bytes = static_cast<size_t>(copy_w) * 4;

    for (int y = 0; y < copy_h; ++y) {
        uint8_t* dst_row = dst + static_cast<size_t>(y) * s_pitch;
        const uint8_t* src_row = s_exit_card_pixels.data() + static_cast<size_t>(y) * s_exit_card_pitch;
        std::memcpy(dst_row, src_row, line_bytes);
    }

    const uint64_t t_submit = now_us();
    int rc = sceVideoOutSubmitFlip(s_video_handle, s_current_buffer_idx, 1 /* VSYNC */, NextFlipArg());
    if (rc >= 0) {
        s_last_flip_idx = s_current_buffer_idx;
        // Poll flip status to guarantee hardware scanout completion before shutdown
        OrbisVideoOutFlipStatus st{};
        const uint64_t poll_start = now_us();
        while (now_us() - poll_start < 50000) { // Max 50ms wait
            if (sceVideoOutGetFlipStatus(s_video_handle, &st) == 0) {
                if (st.currentBuffer == s_current_buffer_idx && st.numFlipPending == 0) break;
            }
            sceKernelUsleep(2000);
        }
        const uint64_t flip_elapsed = now_us() - t_submit;
        char detail[96];
        std::snprintf(detail, sizeof(detail), "buffer=%d elapsed_us=%llu status_current=%d pending=%d",
                      s_current_buffer_idx, static_cast<unsigned long long>(flip_elapsed),
                      st.currentBuffer, st.numFlipPending);
        LogAppLifecycleEvent("VIDEOOUT_CARD_SUBMIT_DONE", detail);
        return true;
    }
    return false;
#else
    return false;
#endif
}

void PS4VideoOutRenderer::SetStatsHudActive(bool active) {
#ifdef __ORBIS__
    // Was a no-op stub. The overlay renderer below already existed but was never
    // reachable, so the in-game menu's HUD entry did nothing.
    const bool was_active = s_stats_hud_active.load(std::memory_order_acquire);
    s_stats_hud_active.store(active, std::memory_order_release);
    s_hud_blit_logged = false;
    if (active && !was_active) {
        PreRenderStatsHudBuffer();
    }
#else
    (void)active;
#endif
}

bool PS4VideoOutRenderer::IsStatsHudActive() {
#ifdef __ORBIS__
    return s_stats_hud_active.load(std::memory_order_acquire);
#else
    return false;
#endif
}

void PS4VideoOutRenderer::ToggleStatsHud() {
#ifdef __ORBIS__
    SetStatsHudActive(!s_stats_hud_active.load(std::memory_order_acquire));
#endif
}

void PS4VideoOutRenderer::UpdateLiveStats(int fps, int decodeFps, int kbps, int rtt, int resW, int resH) {
    g_net_fps.store(fps, std::memory_order_relaxed);
    g_net_decode_fps.store(decodeFps, std::memory_order_relaxed);
    g_net_kbps.store(kbps, std::memory_order_relaxed);
    g_net_rtt.store(rtt, std::memory_order_relaxed);
    g_net_res_w.store(resW, std::memory_order_relaxed);
    g_net_res_h.store(resH, std::memory_order_relaxed);
}

int PS4VideoOutRenderer::PickFreeBuffer() {
#ifdef __ORBIS__
    if (s_video_handle < 0) return -1;
    OrbisVideoOutFlipStatus st{};
    if (sceVideoOutGetFlipStatus(s_video_handle, &st) != 0) {
        // Without a usable status we cannot prove any buffer is safe to write.
        // The old naive rotation could target the buffer being scanned out and
        // tear the image, so refuse instead.
        return -1;
    }

    const int currently_shown = st.currentBuffer;
    // Buffer safety. This function has been wrong in both directions, so the
    // reasoning is recorded in full.
    //
    // Measured field evidence (latest run, 77 samples):
    //     buffer=1 in 77 of 77 samples, pending=1 in 77 of 77
    // i.e. the SAME physical buffer was rewritten every frame while the DCE had a
    // flip queued. That is the horizontal tear the user sees (top half stale,
    // bottom half current).
    //
    // The cause was the previous rule, "refuse whenever numFlipPending > 0". This
    // console reports pending >= 1 almost continuously, so that rule rejected
    // nearly every pick and forced the caller onto the same buffer again and
    // again. Refusing is not the same as being safe - it just relocated the bug.
    //
    // Accounting with kNumBuffers = 4:
    //   provably occupied = {currently_shown, s_last_flip_idx}
    //   queued flips      = numFlipPending, and s_last_flip_idx is one of them
    // So two buffers remain free while pending <= 2, which is the headroom that was
    // missing with three buffers. Above 2 the accounting is no longer provable and
    // the pick is refused rather than guessed.
    auto skip = [&](int candidate) {
        if (candidate == currently_shown) return true;
        if (candidate == s_last_flip_idx) return true;
        if (st.numFlipPending > 2) return true; // accounting no longer provable
        return false;
    };

    for (int i = 0; i < kNumBuffers; i++) {
        const int candidate = (s_current_buffer_idx + i) % kNumBuffers;
        if (skip(candidate)) continue;
        s_current_buffer_idx = (candidate + 1) % kNumBuffers;
        return candidate;
    }
    // No provably free buffer. Report back-pressure; the caller yields 1 ms and
    // retries this same frame, so no content is lost, only deferred.
    static uint32_t s_pick_busy_count = 0;
    if (++s_pick_busy_count % 120 == 1) {
        char detail[112];
        std::snprintf(detail, sizeof(detail), "shown=%d pending=%d last_flip=%d busy=%u",
                      currently_shown, st.numFlipPending, s_last_flip_idx, s_pick_busy_count);
        LogAppLifecycleEvent("VIDEOOUT_QUEUE_FULL_HOLD", detail);
    }
    return -1;
#else
    return 0;
#endif
}

bool PS4VideoOutRenderer::WaitForFlipsSettled(uint32_t timeout_ms, const char* reason) {
#ifdef __ORBIS__
    if (s_video_handle <= 0) return true;
    const uint64_t wait_start_us = now_us();
    const uint64_t timeout_us = static_cast<uint64_t>(timeout_ms) * 1000ULL;
    bool settled = false;
    do {
        OrbisVideoOutFlipStatus st{};
        if (sceVideoOutGetFlipStatus(s_video_handle, &st) != 0 || st.numFlipPending == 0) {
            settled = true;
            break;
        }
        sceKernelUsleep(1000);
    } while (now_us() - wait_start_us < timeout_us);

    const uint64_t waited_us = now_us() - wait_start_us;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "reason=%s settled=%d wait_us=%llu timeout_ms=%u",
                  reason ? reason : "unspecified", settled ? 1 : 0,
                  static_cast<unsigned long long>(waited_us), timeout_ms);
    LogAppLifecycleEvent(settled ? "VIDEOOUT_FLIP_DRAIN_OK" : "VIDEOOUT_FLIP_DRAIN_TIMEOUT", detail);
    return settled;
#else
    (void)timeout_ms;
    (void)reason;
    return true;
#endif
}

bool PS4VideoOutRenderer::AllocateDirectMemory(size_t total_bytes) {
#ifdef __ORBIS__
    FreeDirectMemory();

    s_dmem_total_size = (total_bytes + kDmemAlign - 1) & ~(kDmemAlign - 1);
    off_t max_dmem = sceKernelGetDirectMemorySize();

    // Allocate WC_GARLIC (3)
    int rc = sceKernelAllocateDirectMemory(0, max_dmem, s_dmem_total_size, kDmemAlign,
                                           3 /* ORBIS_KERNEL_WC_GARLIC */, &s_dmem_offset);
    if (rc < 0) {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "size=%zu aligned_size=%zu rc=0x%08X",
                      total_bytes, s_dmem_total_size, static_cast<unsigned>(rc));
        LogAppLifecycleEvent("VIDEOOUT_DMEM_FAIL", detail);
        return false;
    }

    char allocDetail[128];
    std::snprintf(allocDetail, sizeof(allocDetail), "size=%zu aligned_size=%zu offset=0x%llX",
                  total_bytes, s_dmem_total_size, static_cast<unsigned long long>(s_dmem_offset));
    LogAppLifecycleEvent("VIDEOOUT_DMEM_ALLOC_OK", allocDetail);

    // Map memory to CPU Virtual Address Space
    rc = sceKernelMapDirectMemory(&s_dmem_cpu_addr, s_dmem_total_size, 0x33 /* RW */, 0,
                                  s_dmem_offset, kDmemAlign);
    if (rc < 0 || !s_dmem_cpu_addr) {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "rc=0x%08X", static_cast<unsigned>(rc));
        LogAppLifecycleEvent("VIDEOOUT_MAP_FAIL", detail);
        sceKernelReleaseDirectMemory(s_dmem_offset, s_dmem_total_size);
        s_dmem_offset = 0;
        s_dmem_total_size = 0;
        s_dmem_cpu_addr = nullptr;
        return false;
    }

    char mapDetail[128];
    std::snprintf(mapDetail, sizeof(mapDetail), "cpu_addr=%p gpu_addr=0x%llX",
                  s_dmem_cpu_addr, static_cast<unsigned long long>(s_dmem_offset));
    LogAppLifecycleEvent("VIDEOOUT_MAP_OK", mapDetail);

    for (int i = 0; i < kNumBuffers; i++) {
        s_framebuffers[i] = static_cast<uint8_t*>(s_dmem_cpu_addr) + (static_cast<size_t>(i) * s_per_frame_bytes);
        std::memset(s_framebuffers[i], 0, s_per_frame_bytes);
    }
    return true;
#else
    return false;
#endif
}

void PS4VideoOutRenderer::FreeDirectMemory() {
#ifdef __ORBIS__
    if (s_dmem_cpu_addr && s_dmem_total_size > 0) {
        s_dmem_cpu_addr = nullptr;
    }
    if (s_dmem_total_size > 0 && s_dmem_offset != 0) {
        sceKernelReleaseDirectMemory(s_dmem_offset, s_dmem_total_size);
        s_dmem_offset = 0;
        s_dmem_total_size = 0;
    }
    for (int i = 0; i < kNumBuffers; i++) {
        s_framebuffers[i] = nullptr;
    }
#endif
}

bool PS4VideoOutRenderer::Initialize(int width, int height) {
#ifdef __ORBIS__
    // ---------------------------------------------------------------------
    // RESOLUCION DEL FRAMEBUFFER: se acepta la del stream si es una de las que
    // `sceVideoOut` sabe escalar a la pantalla.
    //
    // BUG QUE ESTO CORRIGE (medido en consola, 433 adopciones en una sesion):
    //   Antes habia dos unicos destinos, 1920x1080 y 1280x720:
    //       int target_w = (width >= 1920) ? 1920 : 1280;
    //       int target_h = (width >= 1920) ? 1080 : 720;
    //   Con eso, `Initialize(960, 540)` se convertia en **1280x720**. El modo AUTO de adopcion
    //   pedia adoptar 960x540, Initialize lo ignoraba y dejaba s_width=1280, asi que
    //   `needs_scaling` seguia siendo true en el frame siguiente y **se volvia a adoptar**:
    //   433 adopciones (una por frame) y el escalado por CPU nunca desaparecia
    //   (VIDEOOUT_SCALE_BILINEAR_US avg=15496). Es decir: el modo AUTO hacia exactamente lo
    //   contrario de lo que pretendia.
    //
    // Ahora se acepta cualquier resolucion de la tabla de forma EXACTA. Esto es necesario para el
    // objetivo v3.28: la resolucion configurada por el usuario (1280x720 o 1920x1080) debe
    // aplicarse TAL CUAL, sin que ningun redondeo la altere.
    //
    // IMPORTANTE: la resolucion del framebuffer se fija UNA VEZ al arrancar el stream y **NO cambia
    // durante la sesion**. No hay ningun camino que llame a Initialize() con otra resolucion en
    // caliente, asi que no puede haber reconfiguracion de buffers, ni bucle, ni parpadeo por ese
    // motivo. Si el servidor entrega 960x540 sobre un framebuffer de 1280x720, se convierte con el
    // escalador SSE2 — pero el framebuffer NO se toca.
    // ---------------------------------------------------------------------
    // Tabla de resoluciones aceptadas como framebuffer (todas 16:9 y multiplos de 16).
    struct SupportedSize { int w; int h; };
    static const SupportedSize kSupported[] = {
        {1920, 1080},   // 1080P FIJO
        {1280, 720},    // 720P FIJO
        {960,  540},    // 540p, por si en el futuro se expone como opcion fija
        {854,  480},    // 480p
    };

    int target_w = 1280;
    int target_h = 720;
    bool matched = false;
    for (const auto& s : kSupported) {
        if (s.w == width && s.h == height) {
            target_w = s.w;
            target_h = s.h;
            matched = true;
            break;
        }
    }
    if (!matched) {
        // Resolucion no listada: se elige la soportada mas cercana por area, para no caer siempre
        // en 720p y provocar el bucle de adopcion que ya se midio.
        const long long want = static_cast<long long>(width) * static_cast<long long>(height);
        long long best = -1;
        for (const auto& s : kSupported) {
            const long long area = static_cast<long long>(s.w) * static_cast<long long>(s.h);
            const long long diff = area > want ? area - want : want - area;
            if (best < 0 || diff < best) {
                best = diff;
                target_w = s.w;
                target_h = s.h;
            }
        }
        char d[128];
        std::snprintf(d, sizeof(d), "requested=%dx%d chosen=%dx%d reason=not_in_table",
                      width, height, target_w, target_h);
        LogAppLifecycleEvent("VIDEOOUT_SIZE_SNAPPED", d);
    }

    // =================================================================================================
    // TECHO DE 720p: NO SE REGISTRA UN FRAMEBUFFER DE 1080p (v4.22)
    // =================================================================================================
    // POR QUE, y es una decision del usuario con datos detras:
    //
    //   *"a 720p, sin 1080 ya que es dificil"*
    //
    // Y los datos le dan la razon, medidos en consola:
    //
    //   | Framebuffer | Evidencia |
    //   |---|---|
    //   | **1280x720** | **119 flips estables**; y con el stream a 720p da **`frames=60`,
    //   |             | `iter_ms=16`, `render_max_ms=32`** — el presupuesto de 16.666 us se cumple |
    //   | 1920x1080 | **1 solo flip y la sesion se cerro** (medido) |
    //
    // **Y el caso de 1080p no aporta nada que 720p no tenga:** sobre un panel de 1080p, un framebuffer
    // de 720p **se estira y llena la pantalla** (comprobado: se ve a pantalla completa), y VideoOut
    // hace ese escalado **en hardware**. Es decir, **1080p solo anadiria 4 veces mas pixeles que
    // escribir, a cambio de nada visible** — porque la fuente del stream es 720p de todas formas.
    //
    // SE MANTIENE LA TABLA (960x540 y 854x480 siguen siendo validos: son MAS pequenios y por tanto mas
    // baratos), y **se rechaza cualquier peticion de 1080p bajandola a 720p**, que es el mayor tamano
    // con evidencia de estabilidad. Asi no hay ningun camino —ni una opcion de configuracion futura,
    // ni una adopcion de resolucion— que pueda volver a registrar un framebuffer de 1080p.
    if (target_w > 1280 || target_h > 720) {
        char d[160];
        std::snprintf(d, sizeof(d),
                      "pedido=%dx%d limitado_a=1280x720 motivo=techo_720p_sin_evidencia_de_1080p",
                      target_w, target_h);
        LogAppLifecycleEvent("VIDEOOUT_SIZE_CAPPED_720P", d);
        target_w = 1280;
        target_h = 720;
    }

    if (s_is_ready && s_width == target_w && s_height == target_h) return true;
    if (s_is_ready) {
        // IMPORTANTE antes de liberar los buffers: esperar a que terminen los flips pendientes.
        // Liberar con flips en vuelo puede colgar el kernel de VideoOut. La funcion ya existia
        // (WaitForFlipsSettled) y estaba implementada pero no se llamaba en este punto.
        (void)WaitForFlipsSettled(120, "reinitialize_before_shutdown");
        Shutdown();
    }

    s_width = target_w;
    s_height = target_h;
    s_pitch = static_cast<size_t>(target_w) * 4;
    s_per_frame_bytes = s_pitch * static_cast<size_t>(target_h);

    char initBegin[128];
    std::snprintf(initBegin, sizeof(initBegin), "width=%d height=%d buffers=%d format=BGRA req=%dx%d",
                  s_width, s_height, kNumBuffers, width, height);
    LogAppLifecycleEvent("VIDEOOUT_INIT_BEGIN", initBegin);

    char pitchDetail[128];
    std::snprintf(pitchDetail, sizeof(pitchDetail), "width=%d height=%d pitch=%zu",
                  s_width, s_height, s_pitch);
    LogAppLifecycleEvent("VIDEOOUT_PITCH_OK", pitchDetail);

    int mod_rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    if (mod_rc < 0 && mod_rc != static_cast<int>(0x805A1000)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "rc=0x%08X", static_cast<unsigned>(mod_rc));
        LogAppLifecycleEvent("VIDEOOUT_MODULE_LOAD_FAIL", detail);
        return false;
    }
    LogAppLifecycleEvent("VIDEOOUT_MODULE_LOAD_OK", "module=libSceVideoOut.sprx");

    // Open VideoOut Bus Main
    s_video_handle = sceVideoOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (s_video_handle <= 0) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "rc=0x%08X", static_cast<unsigned>(s_video_handle));
        LogAppLifecycleEvent("VIDEOOUT_OPEN_FAIL", detail);
        return false;
    }

    char openDetail[64];
    std::snprintf(openDetail, sizeof(openDetail), "handle=0x%08X bus=MAIN", static_cast<unsigned>(s_video_handle));
    LogAppLifecycleEvent("VIDEOOUT_OPEN_OK", openDetail);

    // Set flip rate to 60Hz
    sceVideoOutSetFlipRate(s_video_handle, 0);

    // Allocate Garlic memory for triple buffering
    size_t total_fb_bytes = s_per_frame_bytes * kNumBuffers;
    if (!AllocateDirectMemory(total_fb_bytes)) {
        sceVideoOutClose(s_video_handle);
        s_video_handle = -1;
        return false;
    }

    // Register Buffers in VideoOut
    OrbisVideoOutBufferAttribute attr{};
    //
    // =================================================================================================
    // EL PIXEL FORMAT: EL VALOR FUNCIONA, PERO EL COMENTARIO ESTABA MAL. LEER ESTO ANTES DE TOCARLO.
    // =================================================================================================
    // Aqui ponia `0x80000000u /* ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB */`. **Ese nombre no
    // existe en el SDK.** La tabla real es:
    //
    //     OpenOrbis (el SDK de este proyecto):
    //         ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB = 0x80002200     <- el UNICO declarado
    //
    //     shadPS4 (emulador de referencia del proyecto):
    //         SCE_VIDEO_OUT_PIXEL_FORMAT_A8R8G8B8_SRGB = 0x80000000
    //         SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB = 0x80002200
    //
    // O sea: **0x80000000 es el formato A8R8G8B8_SRGB** segun la referencia, no el A8B8G8R8 del SDK.
    //
    // POR QUE **NO** SE HA CAMBIADO A 0x80002200, pese a la discrepancia:
    //
    //   1. **La ruta directa FUNCIONA con 0x80000000.** El proyecto midio **119 flips estables** con
    //      esta configuracion y el video se ha visto. Si el valor fuera invalido,
    //      `sceVideoOutRegisterBuffers` habria devuelto error y la ruta directa **no presentaria nada**
    //      (se degradaria a SDL por `VIDEOOUT_HANDOFF_FAIL`). Registra y presenta.
    //
    //   2. **A8R8G8B8 y A8B8G8R8 pueden ser el MISMO byte order con dos nombres.** Los dos son 4 bytes
    //      por pixel con el canal A en el byte alto; "R8G8B8" y "B8G8R8" describen el orden de lectura
    //      del valor de 32 bits en un sentido o en el otro. El proyecto escribe **B,G,R,A** en memoria
    //      (byte bajo = B), que es coherente con las dos descripciones.
    //
    //   3. **El riesgo es asimetrico.** Si el valor actual fuera correcto y se cambiara, se perderia la
    //      ruta de 60 fps por completo. Si el valor actual fuera incorrecto, el sintoma seria **colores
    //      con R y B intercambiados**, no una perdida de rendimiento. **No tengo forma de comprobar cual
    //      de las dos cosas pasa sin consola.**
    //
    // CONCLUSION: se mantiene `0x80000000` (probado, presenta, 119 flips) y se corrige la ETIQUETA, que
    // era lo unico demostrablemente erroneo. **Si algun dia se ve el video con R y B intercambiados
    // (el rojo sale azul), el cambio a probar es `0x80002200`, y es esta linea.**
    sceVideoOutSetBufferAttribute(&attr,
                                  0x80000000u /* A8R8G8B8_SRGB segun la referencia; el SDK solo declara
                                                 A8B8G8R8_SRGB = 0x80002200. Ver el bloque de arriba
                                                 antes de cambiarlo: la ruta directa funciona con este. */,
                                  1 /* ORBIS_VIDEO_OUT_TILING_MODE_LINEAR */,
                                  0 /* ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9 */,
                                  s_width, s_height, s_width);

    int reg_rc = sceVideoOutRegisterBuffers(s_video_handle, 0, s_framebuffers, kNumBuffers, &attr);
    if (reg_rc < 0) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "rc=0x%08X", static_cast<unsigned>(reg_rc));
        LogAppLifecycleEvent("VIDEOOUT_REGISTER_FAIL", detail);
        FreeDirectMemory();
        sceVideoOutClose(s_video_handle);
        s_video_handle = -1;
        return false;
    }

    char bufDetail[64];
    std::snprintf(bufDetail, sizeof(bufDetail), "count=%d", kNumBuffers);
    LogAppLifecycleEvent("VIDEOOUT_REGISTER_BUFFERS_OK", bufDetail);

    // Flip-completion event queue. sceVideoOutAddFlipEvent signals this equeue when
    // a submitted flip completes, which is the only trustworthy source of truth for
    // "is this buffer free". Failure is NOT fatal: without it Present() falls back
    // to the snapshot-based pick, which is what the previous builds did.
    s_flip_equeue = 0;
    s_flip_equeue_ready = false;
    if (sceKernelCreateEqueue(&s_flip_equeue, "gfn-flip") == 0 && s_flip_equeue != 0) {
        const int add_rc = sceVideoOutAddFlipEvent(s_flip_equeue, s_video_handle, nullptr);
        char detail[112];
        std::snprintf(detail, sizeof(detail), "equeue=0x%llX add_flip_rc=0x%08X",
                      static_cast<unsigned long long>(s_flip_equeue),
                      static_cast<unsigned>(add_rc));
        LogAppLifecycleEvent(add_rc == 0 ? "VIDEOOUT_FLIP_EQUEUE_OK" : "VIDEOOUT_FLIP_EQUEUE_FAIL", detail);
        s_flip_equeue_ready = (add_rc == 0);
    } else {
        LogAppLifecycleEvent("VIDEOOUT_FLIP_EQUEUE_FAIL", "stage=sceKernelCreateEqueue");
    }

    s_shutting_down.store(false, std::memory_order_release);
    s_is_ready = true;
    s_consecutive_flip_fails = 0;
    s_presented_frames = 0;
    s_dropped_frames = 0;
    s_perf_sample_start_ms = now_ms();
    s_perf_frames_in_sample = 0;
    s_perf_dropped_in_sample = 0;
    s_total_present_us = 0;
    s_max_present_us = 0;
    return true;
#else
    return false;
#endif
}

void PS4VideoOutRenderer::PresentTestPattern() {
#ifdef __ORBIS__
    if (!s_is_ready || s_video_handle <= 0 || !s_framebuffers[s_current_buffer_idx]) return;

    LogAppLifecycleEvent("VIDEOOUT_TEST_PATTERN_BEGIN", "pattern=rgb_bars_corners_diagonal");

    uint8_t* dst = static_cast<uint8_t*>(s_framebuffers[s_current_buffer_idx]);

    for (int y = 0; y < s_height; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(dst + (static_cast<size_t>(y) * s_pitch));
        for (int x = 0; x < s_width; x++) {
            uint8_t* px = reinterpret_cast<uint8_t*>(&row[x]);
            if (y < s_height / 3) {
                px[0] = 0;   px[1] = 0;   px[2] = 255; px[3] = 255; // Red
            } else if (y < (2 * s_height) / 3) {
                px[0] = 0;   px[1] = 255; px[2] = 0;   px[3] = 255; // Green
            } else {
                px[0] = 255; px[1] = 0;   px[2] = 0;   px[3] = 255; // Blue
            }

            // Top-left white corner 120x120
            if (x < 120 && y < 120) {
                px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
            }
            // Top-right black corner 120x120
            else if (x >= (s_width - 120) && y < 120) {
                px[0] = 0; px[1] = 0; px[2] = 0; px[3] = 255;
            }
            // Diagonal line (Yellow: B=0, G=255, R=255)
            int diag_x = (y * s_width) / s_height;
            if (x >= diag_x - 3 && x <= diag_x + 3) {
                px[0] = 0; px[1] = 255; px[2] = 255; px[3] = 255;
            }
        }
    }

    // =================================================================================================
    // BORDE EXTERIOR BLANCO DE 10 px (v3.99)
    // =================================================================================================
    // POR QUE SE ANADE, y es lo que decide un experimento pendiente desde la v3.56:
    //
    // La pregunta abierta del proyecto es si **un framebuffer de VideoOut MAS PEQUENO que el panel se
    // estira o se presenta a su tamanio**. El proyecto la declaro indedducible del codigo
    // (`PS4-V3.56` §2) y su sonda nunca llego a ejecutarse.
    //
    // Con un borde blanco pegado a los cuatro limites del framebuffer, la respuesta se ve **de un
    // vistazo y sin leer ningun log**:
    //
    //   - Si el borde blanco TOCA los cuatro limites de la pantalla -> **el escalador EXISTE** y se
    //     puede registrar el framebuffer al tamanio del stream (escalado de CPU = 0).
    //   - Si el borde blanco enmarca un rectangulo MAS PEQUENO que la pantalla, con marco negro
    //     alrededor -> **NO hay escalado automatico**, y el framebuffer tiene que ser del tamanio del
    //     panel (que es lo que hace el cliente).
    //
    // Se dibuja DESPUES de las esquinas y la diagonal para que quede encima de todo, y solo en el
    // contorno, asi que no tapa la informacion que ya delataba el patron anterior.
    {
        const int kBorder = 10;
        auto put_white = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= s_width || y >= s_height) return;
            uint8_t* px = reinterpret_cast<uint8_t*>(dst + static_cast<size_t>(y) * s_pitch)
                              + static_cast<size_t>(x) * 4;
            px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
        };
        for (int x = 0; x < s_width; ++x) {
            for (int t = 0; t < kBorder; ++t) {
                put_white(x, t);                       // borde superior
                put_white(x, s_height - 1 - t);        // borde inferior
            }
        }
        for (int y = 0; y < s_height; ++y) {
            for (int t = 0; t < kBorder; ++t) {
                put_white(t, y);                       // borde izquierdo
                put_white(s_width - 1 - t, y);         // borde derecho
            }
        }
    }

    // Pass a unique flip argument so the completion event can be identified.
    // (The previous code passed 0 here, which is why the event queue could not be
    // used to synchronise: every event looked identical.)
    int rc = sceVideoOutSubmitFlip(s_video_handle, s_current_buffer_idx, 1 /* VSYNC */, NextFlipArg());
    if (rc < 0) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "rc=0x%08X buffer=%d", static_cast<unsigned>(rc), s_current_buffer_idx);
        LogAppLifecycleEvent("VIDEOOUT_TEST_PATTERN_SUBMIT_FAIL", detail);
    } else {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "buffer=%d", s_current_buffer_idx);
        LogAppLifecycleEvent("VIDEOOUT_TEST_PATTERN_SUBMIT_OK", detail);
        LogAppLifecycleEvent("VIDEOOUT_READY", "test_pattern_visible");
    }

    s_last_flip_idx = s_current_buffer_idx;
    s_current_buffer_idx = (s_current_buffer_idx + 1) % kNumBuffers;
#endif
}

bool PS4VideoOutRenderer::Present(AVFrame* frame) {
    opennow::SetCurrentStage(7 /* STAGE_VIDEO_PICK */);
#ifdef __ORBIS__
    if (s_shutting_down.load(std::memory_order_acquire) || !s_is_ready) return false;
    if (!frame) return false;

    if (frame->width <= 0 || frame->height <= 0 || !frame->data[0]) {
        LogAppLifecycleEvent("VIDEOOUT_SIMD_FAIL", "invalid_frame_dimensions");
        return false;
    }

    if (!s_is_ready || s_video_handle <= 0) return false;

    // =====================================================================
    // MODO AUTO DE ESCALADO: adoptar la resolucion que entrega el servidor.
    //
    // PROBLEMA MEDIDO (3.25, con el pool ya en 6 hilos):
    //   El escalado 540p->720p cuesta dispatch_avg_us=13766 y el presupuesto a 60 FPS es 16.666 us:
    //   se come el 83% y la tasa se vuelve fragil (FPS observados: 51-61 con caidas).
    //   Y el servidor baja a 960x540 en TODAS las sesiones, con la red limpia (gaps=0, nack=0).
    //
    // SOLUCION: en lugar de convertir 540p a 720p en la CPU, se adopta 960x540 como tamano de
    // framebuffer y **sceVideoOut lo escala a la pantalla POR HARDWARE**. Es gratis y el escalador
    // de salida de la consola tiene mas taps que una interpolacion bilineal de 4 puntos.
    //
    // El cambio ocurre UNA sola vez por sesion (el servidor adapta una o dos veces), asi que el
    // coste de reconfigurar los buffers es despreciable frente a los 13,7 ms por frame que ahorra.
    //
    // Solo se activa cuando el llamante lo pide (modo AUTO o 540P FIJO). En modo NATIVO se conserva
    // exactamente el comportamiento anterior.
    // =====================================================================
    // SIN ADOPCION DINAMICA DE RESOLUCION (objetivo v3.28).
    //
    // HISTORIA: en la 3.26 el modo AUTO adoptaba la resolucion que entregaba el servidor. Estaba
    // roto —la tabla de resoluciones de Initialize() forzaba 720p, asi que s_width nunca cambiaba y
    // el intento se repetia: **433 adopciones en una sesion, una por frame**, con el escalado por
    // CPU todavia activo (VIDEOOUT_SCALE_BILINEAR_US avg=15496).
    //
    // DECISION: se elimina por completo. El framebuffer se fija EXCLUSIVAMENTE a la resolucion
    // configurada por el usuario (1280x720 o 1920x1080) y NO cambia durante la sesion. Si el
    // servidor entrega otra cosa, se convierte al tamano fijo con el escalador, pero **nunca se
    // reconfiguran los buffers** y por tanto **no puede haber bucle ni parpadeo por reconfiguracion**.
    //
    // Esto tambien elimina el riesgo de colgar el kernel de VideoOut al recrear buffers con flips
    // en vuelo, que era el unico riesgo real del modo automatico.
    //
    // Nota tecnica: `sceVideoOutSubmitFlip(handle, index, mode, timestamp)` NO acepta un rectangulo
    // de origen, asi que VideoOut no puede escalar la imagen en el flip. El escalado por CPU cuando
    // el servidor baja de resolucion es, por tanto, inevitable si se quiere llenar el framebuffer.
    // A cambio, el pool de 6 participantes lo deja en ~13,8 ms, dentro del presupuesto de 16.666 us.

    const bool needs_scaling = (frame->width != s_width || frame->height != s_height);

    // REGISTRO DE SESION: se anota cada frame presentado en un anillo en memoria (no en disco, para
    // no penalizar la sesion). Permite reconstruir la secuencia completa alrededor de cualquier
    // evento. Ver src/opennow/session_recorder.hpp para el porque.
    {
        opennow::diag::FrameRecord rec;
        rec.frame_index = s_presented_frames;
        rec.time_ms = now_us() / 1000;
        rec.src_w = frame->width;
        rec.src_h = frame->height;
        rec.dst_w = s_width;
        rec.dst_h = s_height;
        rec.path_scaled = needs_scaling ? 1 : 0;
        // Tamano del frame DECODIFICADO, calculado de las lineas. Es un indicador indirecto de la
        // compresion: si la imagen llega muy comprimida, el contenido decodificado tiene menos
        // detalle, pero el tamano en bytes es el mismo. Lo que SI aporta es confirmar que el
        // tamano del frame no cambia, y por tanto que la degradacion no viene de ahi.
        //
        // NOTA: el tamano real de la unidad de acceso (lo que ocupa el frame COMPRIMIDO) vive en
        // AVPacket, no en AVFrame, y no esta disponible en este punto sin tocar el decodificador.
        // Queda como mejora pendiente: seria el dato mas directo para ver la cuantizacion.
        rec.access_unit_bytes = frame->linesize[0] * frame->height;
        rec.is_keyframe = 0;
        rec.scale_us = 0;
        rec.present_us = 0;
        opennow::diag::RecordFrame(rec);
    }
    opennow::SetCurrentStage(8 /* STAGE_VIDEO_CONVERT */);
    const uint64_t stage_convert_t0 = now_us();
    static int last_frame_w = 0, last_frame_h = 0;
    if (frame->width != last_frame_w || frame->height != last_frame_h) {
        last_frame_w = frame->width;
        last_frame_h = frame->height;
        if (needs_scaling) {
            const float ratio_w = static_cast<float>(s_width) / frame->width;
            const float ratio_h = static_cast<float>(s_height) / frame->height;
            char adaptDetail[160];
            std::snprintf(adaptDetail, sizeof(adaptDetail),
                          "from=%dx%d stream=%dx%d ratio=%.2f,%.2f mode=bilinear_scaled",
                          s_width, s_height, frame->width, frame->height, ratio_w, ratio_h);
            LogAppLifecycleEvent("VIDEOOUT_RESOLUTION_ADAPTED", adaptDetail);

            char pathDetail[96];
            std::snprintf(pathDetail, sizeof(pathDetail),
                          "path=bilinear_scaled in=%dx%d out=%dx%d",
                          frame->width, frame->height, s_width, s_height);
            LogAppLifecycleEvent("VIDEOOUT_PRESENT_PATH", pathDetail);
        } else {
            char pathDetail[96];
            std::snprintf(pathDetail, sizeof(pathDetail),
                          "path=direct_1to1 resolution=%dx%d",
                          s_width, s_height);
            LogAppLifecycleEvent("VIDEOOUT_PRESENT_PATH", pathDetail);
        }
    }

    // Wait for the DCE to complete a flip before writing.
    //
    // This runs immediately before the CPU conversion writes into dst, which is the
    // only moment that matters: the conversion costs ~14 ms, so any decision made
    // earlier is stale by the time the first pixel lands.
    //
    // Matching a SPECIFIC flip argument was tried and failed: waits=0 timeouts=600
    // with avg_us=31409, meaning ev.data never carried the argument back and every
    // call burned the full budget, collapsing presentation from 38-42 to 24 FPS.
    // Matching was never required - this only needs proof that the DCE finished
    // *some* flip, which is what frees a buffer. Any event is sufficient.
    //
    // Timeout is 20 ms, roughly one 60 Hz period: enough to catch a real flip, small
    // enough that a missed event cannot dominate the frame.
    if (s_flip_equeue_ready && s_last_flip_idx >= 0) {
        WaitForAnyFlip(20000, "before_write");
    }

    // Detect color range: GFN delivers JPEG full range (color_range == 2 or AVCOL_RANGE_JPEG)
    const bool is_full_range = (frame->color_range == AVCOL_RANGE_JPEG ||
                                frame->color_range == 2 ||
                                frame->format == AV_PIX_FMT_YUVJ420P);
    static int s_logged_color_range = -1;
    if (s_logged_color_range != static_cast<int>(is_full_range)) {
        s_logged_color_range = static_cast<int>(is_full_range);
        LogAppLifecycleEvent("VIDEOOUT_COLOR_MODE",
                             is_full_range ? "mode=full_bt709" : "mode=limited_bt709");
    }

    const uint64_t t0_us = now_us();

    // Check VSYNC and buffer availability: Drop frame if DCE is busy
    const uint64_t stage_t0 = now_us();
    int free_fb_idx = PickFreeBuffer();
    s_stage_pick_us += now_us() - stage_t0;
    if (free_fb_idx < 0) {
        s_dropped_frames++;
        s_perf_dropped_in_sample++;
        return false;
    }
    s_current_buffer_idx = free_fb_idx;

    // Pacing lives in ONE place, not three.
    //
    // There used to be a 60 Hz deadline here as well, on top of the main loop's
    // frame budget and the deadline slots in draw(). Three independent pacers
    // fighting each other is what produced the wild FPS oscillation the user
    // reported (30 -> 100-200 within the same second): whichever one happened to
    // win decided that frame's timing. The main loop is now the single authority,
    // and this function just presents as soon as a buffer is free.
    //
    // Note that sceVideoOutWaitVblank() cannot pace this: it returns in ~22 us on
    // this firmware instead of blocking for the 16666 us frame period (measured
    // three times, in the client and in the standalone probe).

    uint8_t* dst = static_cast<uint8_t*>(s_framebuffers[s_current_buffer_idx]);
    if (!dst) return false;

    // =================================================================================================
    // SEGUNDA COMPROBACION DE `s_shutting_down` (v4.04, JUSTIFICACION CORREGIDA EN v4.05)
    // =================================================================================================
    // **AVISO: la justificacion original de esta comprobacion era FALSA, y aqui queda corregida.**
    //
    // Lo que escribi en la v4.04: que `Shutdown()` podia correr **concurrentemente** con `Present()`,
    // liberar la memoria directa y dejar a `Present()` escribiendo en memoria liberada.
    //
    // **Eso NO puede ocurrir, y lo verifique despues:** `Present()` (main.cpp:5650) y el `Shutdown()` del
    // fallo persistente (main.cpp:5692) estan **los dos dentro del mismo callback**
    // (`AVFrameHolder::instance().get(...)`, main.cpp:5342-5782), que corre en el **hilo principal**.
    // `Present()` **retorna** antes de que se llame a `Shutdown()`. **Es una secuencia estrictamente
    // ordenada, no una carrera.** Las otras tres llamadas a `Shutdown()` (al terminar la sesion, en el
    // modo de prueba y al salir de la app) tambien estan en el hilo principal y tampoco son concurrentes.
    //
    // ENTONCES, POR QUE SE MANTIENE LA COMPROBACION: **fijar el invariante en el codigo.**
    // `Present()` no tiene ninguna otra proteccion contra que se le llame con el renderizador ya cerrado,
    // y hoy eso solo se cumple **por construccion**: hay exactamente cuatro llamadas a `Shutdown()` y todas
    // estan en el hilo principal. Esa es una propiedad **facil de romper sin darse cuenta** (moviendo el
    // fallo persistente a un hilo de red para no bloquear el bucle, o llamando a `Shutdown()` desde un
    // manejador de senal).
    //
    // Con estas comprobaciones, una llamada asi **degrada a "frame descartado" en vez de a escritura en
    // memoria liberada**. El coste es una lectura atomica por frame; lo que se compra es que el invariante
    // deje de depender de que nadie mueva nunca una llamada de hilo.
    if (s_shutting_down.load(std::memory_order_acquire)) {
        s_dropped_frames++;
        s_perf_dropped_in_sample++;
        return false;
    }

    // Scaling telemetry state
    static uint32_t s_scale_count = 0;
    static uint64_t s_scale_total_us = 0;
    static uint64_t s_scale_max_us = 0;
    // Direct 1:1 conversion telemetry (previously missing entirely).
    static uint32_t s_direct_count = 0;
    static uint64_t s_direct_total_us = 0;

    // SIMD BT.709 conversion into GPU Garlic buffer (native or bilinear scaled)
    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
        if (!frame->data[1] || !frame->data[2]) {
            LogAppLifecycleEvent("VIDEOOUT_SIMD_FAIL", "format=yuv420p_missing_chroma");
            return false;
        }
        if (needs_scaling) {
            const uint64_t scale_t0 = now_us();
            color::ScaleBilinearYUV420PToBGRA_BT709(
                dst, static_cast<int>(s_pitch),
                s_width, s_height,
                frame->width, frame->height,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2],
                is_full_range
            );
            const uint64_t scale_elapsed = now_us() - scale_t0;
            // Report the FIRST scaled frame immediately. The previous code only logged
            // every 100th frame, so if the scaled path is catastrophically slow the log
            // never produces a single line about it - which is exactly what happened and
            // is why the scaler's cost had to be inferred instead of read.
            static bool s_first_scale_logged = false;
            if (!s_first_scale_logged) {
                s_first_scale_logged = true;
                char firstDetail[192];
                std::snprintf(firstDetail, sizeof(firstDetail),
                              "FIRST_FRAME_MS=%.3f in=%dx%d out=%dx%d pitch=%zu",
                              scale_elapsed / 1000.0, frame->width, frame->height,
                              s_width, s_height, s_pitch);
                LogAppLifecycleEvent("VIDEOOUT_SCALE_FIRST", firstDetail);
            }
            s_scale_total_us += scale_elapsed;
            if (scale_elapsed > s_scale_max_us) s_scale_max_us = scale_elapsed;
            s_scale_count++;
            // Log every 10 scaled frames rather than every 100. With the old interval a
            // catastrophically slow scaled path never produced a single telemetry line,
            // which is exactly why its cost had to be inferred instead of read.
            if (s_scale_count % 10 == 0) {
                char scaleDetail[128];
                std::snprintf(scaleDetail, sizeof(scaleDetail), "avg=%llu max=%llu frames=%u",
                              static_cast<unsigned long long>(s_scale_total_us / 10),
                              static_cast<unsigned long long>(s_scale_max_us),
                              s_scale_count);
                LogAppLifecycleEvent("VIDEOOUT_SCALE_BILINEAR_US", scaleDetail);
                s_scale_total_us = 0;
                s_scale_max_us = 0;
            }
        } else {
            const uint64_t direct_t0 = now_us();
            color::ConvertYUV420PToBGRA_BT709(
                dst, static_cast<int>(s_pitch),
                frame->width, frame->height,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2],
                color::STORE_STREAMING,
                is_full_range
            );
            // Timing for the direct 1:1 path, which is what a 720p stream uses and
            // which previously had NO telemetry at all. It matters because the tear
            // is reported as appearing only while the controller is in use, i.e. when
            // the decoder and this conversion are working hardest.
            //
            // An accumulated average over 61 frames (coprime with the 4-buffer ring)
            // cannot alias, unlike a sampled point.
            s_direct_total_us += now_us() - direct_t0;
            s_direct_count++;
            if (s_direct_count % 61 == 0) {
                char directDetail[176];
                std::snprintf(directDetail, sizeof(directDetail),
                              "avg_us=%llu frames=%u res=%dx%d pitch=%zu",
                              static_cast<unsigned long long>(s_direct_total_us / 61),
                              s_direct_count, frame->width, frame->height, s_pitch);
                LogAppLifecycleEvent("VIDEOOUT_DIRECT_CONVERT_US", directDetail);
                s_direct_total_us = 0;
            }
        }
    } else if (frame->format == AV_PIX_FMT_NV12) {
        if (!frame->data[1]) {
            LogAppLifecycleEvent("VIDEOOUT_SIMD_FAIL", "format=nv12_missing_chroma");
            return false;
        }
        if (needs_scaling) {
            const uint64_t scale_t0 = now_us();
            color::ScaleBilinearNV12ToBGRA_BT709(
                dst, static_cast<int>(s_pitch),
                s_width, s_height,
                frame->width, frame->height,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                is_full_range
            );
            const uint64_t scale_elapsed = now_us() - scale_t0;
            s_scale_total_us += scale_elapsed;
            if (scale_elapsed > s_scale_max_us) s_scale_max_us = scale_elapsed;
            s_scale_count++;
            if (s_scale_count % 10 == 0) {
                char scaleDetail[128];
                std::snprintf(scaleDetail, sizeof(scaleDetail), "avg=%llu max=%llu frames=%u",
                              static_cast<unsigned long long>(s_scale_total_us / 10),
                              static_cast<unsigned long long>(s_scale_max_us),
                              s_scale_count);
                LogAppLifecycleEvent("VIDEOOUT_SCALE_BILINEAR_US", scaleDetail);
                s_scale_total_us = 0;
                s_scale_max_us = 0;
            }
        } else {
            color::ConvertNV12ToBGRA_BT709(
                dst, static_cast<int>(s_pitch),
                frame->width, frame->height,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                color::STORE_STREAMING,
                is_full_range
            );
        }
    } else {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "format=%d", frame->format);
        LogAppLifecycleEvent("VIDEOOUT_SIMD_FAIL", detail);
        return false;
    }

    // Render floating stats HUD if enabled. The buffer is re-rasterised at most
    // once per second so the figures reflect the current session instead of a
    // snapshot frozen at activation time.
    if (s_stats_hud_active.load(std::memory_order_acquire)) {
        static uint64_t s_hud_refresh_ms = 0;
        const uint64_t hud_now_ms = now_ms();
        if (hud_now_ms - s_hud_refresh_ms >= 1000) {
            s_hud_refresh_ms = hud_now_ms;
            PreRenderStatsHudBuffer();
        }
        DrawStatsHudOverlay(dst, static_cast<int>(s_pitch), s_width, s_height);
    }

    s_stage_convert_us += now_us() - stage_convert_t0;

    // Render In-Game Menu (820x360, cyan border) if enabled via fast blit
    if (s_in_game_menu_active.load(std::memory_order_acquire)) {        BlitInGameMenuOverlay(dst, static_cast<int>(s_pitch), s_width, s_height);
        s_menu_draw_count++;
        if (s_menu_draw_count % 30 == 1) {
            const bool overlay_ready = !s_menu_overlay_pixels.empty() &&
                                       s_menu_overlay_w > 0 && s_menu_overlay_h > 0 &&
                                       s_menu_overlay_w <= s_width && s_menu_overlay_h <= s_height;
            char overlayDetail[192];
            if (overlay_ready) {
                std::snprintf(overlayDetail, sizeof(overlayDetail),
                              "frame=%u buffer=%d serial=%u w=%d h=%d pitch=%d",
                              s_menu_draw_count, s_current_buffer_idx, s_menu_bitmap_serial,
                              s_menu_overlay_w, s_menu_overlay_h, s_menu_overlay_pitch);
                LogAppLifecycleEvent("VIDEOOUT_MENU_OVERLAY_APPLIED", overlayDetail);
            } else {
                std::snprintf(overlayDetail, sizeof(overlayDetail),
                              "reason=bitmap_unavailable frame=%u buffer=%d serial=%u w=%d h=%d",
                              s_menu_draw_count, s_current_buffer_idx, s_menu_bitmap_serial,
                              s_menu_overlay_w, s_menu_overlay_h);
                LogAppLifecycleEvent("VIDEOOUT_MENU_OVERLAY_MISSING", overlayDetail);
            }
        }
    }

    // Submit hardware flip
    //
    // TERCERA COMPROBACION DE `s_shutting_down` (v4.04, JUSTIFICACION CORREGIDA EN v4.05).
    //
    // Misma correccion que en la segunda: **NO es una condicion de carrera**, porque `Present()` y
    // `Shutdown()` estan en el mismo hilo y el primero retorna antes. Ver el bloque largo al principio del
    // camino, donde esta la explicacion completa.
    //
    // Aqui la comprobacion compra ademas una cosa concreta y verificable: **no contar como fallo de flip
    // algo que solo es "el renderizador ya se cerro"**. Sin ella, un `Shutdown()` que dejara `s_video_handle`
    // en -1 haria que `sceVideoOutSubmitFlip` devolviera error, y ese error **incrementaria
    // `s_consecutive_flip_fails`** — ensuciando el guardian de degradacion, la cuarentena y el histograma
    // de buffers con un fallo que no es de presentacion.
    if (s_shutting_down.load(std::memory_order_acquire) || s_video_handle <= 0) {
        s_dropped_frames++;
        s_perf_dropped_in_sample++;
        return false;
    }
    opennow::SetCurrentStage(10 /* STAGE_VIDEO_FLIP */);
    const uint64_t stage_flip_t0 = now_us();
    int rc = sceVideoOutSubmitFlip(s_video_handle, s_current_buffer_idx, 1 /* VSYNC */, NextFlipArg());
    s_stage_flip_us += now_us() - stage_flip_t0;
    const uint64_t elapsed_us = now_us() - t0_us;
    s_total_present_us += elapsed_us;
    s_stage_samples++;
    if (elapsed_us > s_max_present_us) s_max_present_us = elapsed_us;

    // Ask for the next display blank so the CPU cannot race far ahead of
    // scanout. Measured behaviour on this firmware: the call returns in ~22 us
    // instead of the ~16666 us a 60 Hz blank would take, so it does NOT pace the
    // loop by itself. The real pacing now comes from PickFreeBuffer() back-pressure
    // plus the caller's yield, so this is kept as belt-and-braces and no longer
    // reported as if it were synchronisation.
    // Requesting a full recreate means the DCE resources were just torn down:
    // there is no display pipe left to wait on, so never block in that state.
    if (rc >= 0 && !s_request_full_recreate) {
        opennow::SetCurrentStage(11 /* STAGE_VIDEO_WAIT */);
        const uint64_t vblank_t0 = now_us();
        sceVideoOutWaitVblank();
        s_flip_vblank_wait_us = now_us() - vblank_t0;
        s_stage_vblank_us += s_flip_vblank_wait_us;
    } else {
        if (rc < 0 && !s_request_full_recreate) sceKernelUsleep(1000);
        s_flip_vblank_wait_us = 0;
    }

    bool flip_ok = false;
    if (rc < 0) {
        if (static_cast<uint32_t>(rc) == 0x80290012) {
            // Flip queue full / DCE busy (VSYNC pending). Not a fatal bus failure.
            s_dropped_frames++;
            s_perf_dropped_in_sample++;
            static uint32_t s_busy_log_count = 0;
            if (++s_busy_log_count % 60 == 1) {
                char detail[64];
                std::snprintf(detail, sizeof(detail), "rc=0x%08X total_busy=%u", static_cast<unsigned>(rc), s_busy_log_count);
                LogAppLifecycleEvent("VIDEOOUT_FLIP_BUSY", detail);
            }
        } else {
            s_consecutive_flip_fails++;
            char detail[96];
            std::snprintf(detail, sizeof(detail), "rc=0x%08X count=%u stage=present",
                          static_cast<unsigned>(rc), s_consecutive_flip_fails);
            LogAppLifecycleEvent("VIDEOOUT_FLIP_FAIL", detail);
            if (s_consecutive_flip_fails >= 3) {
                // Retrying cannot help: record it once so the UI can tear down
                // and rebuild the VideoOut pipeline (black-screen recovery).
                s_request_full_recreate.store(true, std::memory_order_release);
                LogAppLifecycleEvent("VIDEOOUT_RECREATE_REQUESTED",
                                     "reason=persistent_flip_failure threshold=3");
            }
        }
    } else {
        s_consecutive_flip_fails = 0;
        s_last_flip_idx = s_current_buffer_idx;
        s_presented_frames++;
        s_perf_frames_in_sample++;
        flip_ok = true;

        // NOTE ON THE SAMPLING PERIOD: this used to be `% 60`, and with a
        // kNumBuffers ring of 4 that aliases - 60 is a multiple of 4, so every
        // logged sample could land on the same position of the ring and report the
        // same buffer index. That produced the false conclusion "the rotation is
        // broken / buffer=1 in 139 of 139 samples" across several builds. A period
        // COPRIME with kNumBuffers (61) samples each ring position in turn.
        //
        // An accumulating histogram is also emitted, because a histogram of the last
        // 61 submissions CANNOT alias: if the rotation works, every index in the
        // ring must appear the same number of times.
        constexpr uint32_t kFlipLogPeriod = 61;
        static uint32_t s_buf_hist[kNumBuffers] = {};
        ++s_buf_hist[s_current_buffer_idx];
        if (s_presented_frames % kFlipLogPeriod == 1) {
            OrbisVideoOutFlipStatus st{};
            sceVideoOutGetFlipStatus(s_video_handle, &st);
            char flipDetail[240];
            int n = std::snprintf(flipDetail, sizeof(flipDetail),
                                  "buffer=%d current=%d pending=%d ring=%d vblank_us=%llu hist=",
                                  s_current_buffer_idx, st.currentBuffer, st.numFlipPending,
                                  kNumBuffers,
                                  static_cast<unsigned long long>(s_flip_vblank_wait_us));
            for (int i = 0; i < kNumBuffers && n > 0 && n < static_cast<int>(sizeof(flipDetail)) - 8; ++i) {
                n += std::snprintf(flipDetail + n, sizeof(flipDetail) - n, "%u%s",
                                   s_buf_hist[i], (i + 1 < kNumBuffers) ? "," : "");
                s_buf_hist[i] = 0; // reset each window
            }
            LogAppLifecycleEvent("VIDEOOUT_FLIP_SUBMIT", flipDetail);
        }
    }

    // Telemetry log every 1000 ms
    const uint64_t current_ms = now_ms();
    if (current_ms - s_perf_sample_start_ms >= 1000) {
        const uint64_t sample_elapsed_ms = current_ms - s_perf_sample_start_ms;
        const uint32_t fps = static_cast<uint32_t>((static_cast<uint64_t>(s_perf_frames_in_sample) * 1000u) / sample_elapsed_ms);
        const uint64_t avg_us = s_perf_frames_in_sample > 0 ? (s_total_present_us / s_perf_frames_in_sample) : 0;

        char fpsDetail[96];
        std::snprintf(fpsDetail, sizeof(fpsDetail), "fps=%u dropped=%u", fps, s_perf_dropped_in_sample);
        LogAppLifecycleEvent("VIDEOOUT_PRESENT_FPS", fpsDetail);

        char usDetail[96];
        std::snprintf(usDetail, sizeof(usDetail), "avg_us=%llu max_us=%llu",
                      static_cast<unsigned long long>(avg_us),
                      static_cast<unsigned long long>(s_max_present_us));
        LogAppLifecycleEvent("VIDEOOUT_PRESENT_US", usDetail);

        // =============================================================================================
        // GUARDIAN DE DEGRADACION PROGRESIVA (v3.94). ES EL PATRON DOCUMENTADO DE LOS CIERRES SIN TRAZA.
        // =============================================================================================
        // POR QUE EXISTE, con la evidencia del propio proyecto:
        //
        // `docs/versiones/PS4-V2.81-CRASH-HEARTBEAT.md` describe el cierre de la 2.80 asi:
        //
        //     +56775ms  PRESENT_US avg_us=1172  max_us=1757     <- normal
        //     +72907ms  PRESENT_US avg_us=1167  max_us=1709     <- normal
        //     +73918ms  PRESENT_US avg_us=1492  max_us=14910
        //     +74918ms  PRESENT_US avg_us=7449  max_us=21020    <- x6
        //     +75928ms  PRESENT_US avg_us=10347 max_us=21295    <- x9, y crashea 0,3 s despues
        //
        // *"El coste por frame se multiplica por 9 en los ultimos 2,5 segundos... Esto apunta a
        // agotamiento de un recurso, no a un fallo puntual."* Y el log termina en seco porque el
        // proceso muere por algo que **no pasa por los manejadores de senal** (el kernel lo mata).
        //
        // El mismo sintoma aparecio en la v3.90 (`updatewindowsurface_us=14719`, `present_us=33182`
        // subiendo, y la sesion murio en `stage=6 name=PRESENT`). La causa de raiz de la v3.90 ya esta
        // corregida en esta version (se elimino la copia del driver al pasar a VideoOut directo), pero
        // **el guardian es la red de seguridad para que un caso futuro no muera en silencio**.
        //
        // QUE HACE, y que NO hace:
        //   - Cuando el coste por frame SUBE de forma sostenida en varias ventanas seguidas, lo registra
        //     con los numeros exactos. **Eso es lo que faltaba: el log decia que moria, no por que.**
        //   - Si ademas supera el presupuesto de un frame a 60 fps (16.666 us) durante varias ventanas,
        //     pide la reconstruccion completa del pipeline, que ya existe y esta probada
        //     (`s_request_full_recreate`, consumido por el bucle principal). Reconstruir es mejor que
        //     seguir degradando: la degradacion progresiva termina en cierre.
        //   - **NO corta la sesion** ni cambia de ruta por si mismo: solo registra y, en el caso grave,
        //     pide la reconstruccion que el llamante ya sabe atender.
        {
            static uint64_t s_prev_avg_us = 0;
            static uint32_t s_subidas_seguidas = 0;
            static uint32_t s_sobre_presupuesto = 0;
            const uint64_t kPresupuesto60Us = 16666ull;
            // "Subida" se define con margen: +25 % respecto a la ventana anterior. Sin margen, el ruido
            // normal de una sesion dispararia el aviso constantemente y dejaria de ser util.
            const bool sube = (s_prev_avg_us > 0 && avg_us > (s_prev_avg_us + s_prev_avg_us / 4));
            if(sube) ++s_subidas_seguidas; else s_subidas_seguidas = 0;
            if(avg_us > kPresupuesto60Us) ++s_sobre_presupuesto; else s_sobre_presupuesto = 0;

            if(s_subidas_seguidas >= 3) {
                char degDetail[224];
                std::snprintf(degDetail, sizeof(degDetail),
                              "avg_us=%llu previo_us=%llu subidas_seguidas=%u presupuesto_us=%llu "
                              "sobre_presupuesto=%u frames=%u accion=registrar",
                              static_cast<unsigned long long>(avg_us),
                              static_cast<unsigned long long>(s_prev_avg_us),
                              s_subidas_seguidas,
                              static_cast<unsigned long long>(kPresupuesto60Us),
                              s_sobre_presupuesto, s_stage_samples);
                LogAppLifecycleEvent("VIDEOOUT_DEGRADACION_SOSTENIDA", degDetail);
            }

            if(s_sobre_presupuesto >= 5 && !s_request_full_recreate.load(std::memory_order_acquire)) {
                char recDetail[192];
                std::snprintf(recDetail, sizeof(recDetail),
                              "avg_us=%llu presupuesto_us=%llu ventanas=%u "
                              "motivo=coste_sobre_presupuesto_de_forma_sostenida",
                              static_cast<unsigned long long>(avg_us),
                              static_cast<unsigned long long>(kPresupuesto60Us),
                              s_sobre_presupuesto);
                LogAppLifecycleEvent("VIDEOOUT_RECREATE_REQUESTED_DEGRADACION", recDetail);
                s_request_full_recreate.store(true, std::memory_order_release);
                s_sobre_presupuesto = 0;
            }
            s_prev_avg_us = avg_us;
        }

        // Per-stage breakdown. This is what actually localises a slow Present(): the
        // totals alone could not explain a 600 ms average when every stage that had
        // its own instrumentation measured milliseconds.
        {
            char stageDetail[224];
            std::snprintf(stageDetail, sizeof(stageDetail),
                          "frames=%u pick_us=%llu convert_us=%llu flip_us=%llu vblank_us=%llu",
                          s_stage_samples,
                          static_cast<unsigned long long>(s_stage_pick_us),
                          static_cast<unsigned long long>(s_stage_convert_us),
                          static_cast<unsigned long long>(s_stage_flip_us),
                          static_cast<unsigned long long>(s_stage_vblank_us));
            LogAppLifecycleEvent("VIDEOOUT_PRESENT_STAGES", stageDetail);
        }
        s_stage_pick_us = 0;
        s_stage_convert_us = 0;
        s_stage_flip_us = 0;
        s_stage_vblank_us = 0;
        s_stage_samples = 0;

        s_perf_sample_start_ms = current_ms;
        s_perf_frames_in_sample = 0;
        s_perf_dropped_in_sample = 0;
        s_total_present_us = 0;
        s_max_present_us = 0;
    }
    return flip_ok;
#else
    return false;
#endif
}

void PS4VideoOutRenderer::Shutdown() {
#ifdef __ORBIS__
    s_shutting_down.store(true, std::memory_order_release);

    if (!s_is_ready && s_video_handle <= 0 && s_dmem_offset == 0) {
        return;
    }

    // Allow concurrent worker threads in Present() to exit cleanly
    sceKernelUsleep(16000);

    if (s_video_handle > 0) {
        // Drain pending hardware flips with a 200ms timeout to avoid kernel panic on close
        const uint64_t wait_start_us = now_us();
        bool flip_settled = false;
        while (now_us() - wait_start_us < 200000ULL) {
            OrbisVideoOutFlipStatus status{};
            int rc = sceVideoOutGetFlipStatus(s_video_handle, &status);
            if (rc != 0 || status.numFlipPending == 0) {
                flip_settled = true;
                break;
            }
            sceKernelUsleep(4000); // Poll every 4ms
        }

        if (!flip_settled) {
            LogAppLifecycleEvent("VIDEOOUT_SHUTDOWN_WARNING", "flip_pending_timeout_expired");
        } else {
            const uint64_t drain_time_us = now_us() - wait_start_us;
            char drainDetail[64];
            std::snprintf(drainDetail, sizeof(drainDetail), "drain_us=%llu", static_cast<unsigned long long>(drain_time_us));
            LogAppLifecycleEvent("VIDEOOUT_SHUTDOWN_DRAIN_OK", drainDetail);
        }

        sceVideoOutUnregisterBuffers(s_video_handle, 0);
        sceVideoOutClose(s_video_handle);
        s_video_handle = -1;
    }
    FreeDirectMemory();
    s_is_ready = false;
    s_consecutive_flip_fails = 0;
#endif
}

} // namespace opennow
