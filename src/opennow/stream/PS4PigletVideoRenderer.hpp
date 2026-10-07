#pragma once

#ifdef __ORBIS__

#include "IVideoRenderer.hpp"
#include <SDL2/SDL.h>
#include <vector>

// PS4 presentation path: FFmpeg frames remain CPU-decoded for now; Piglet GLES
// uploads their YUV planes and performs YUV->RGB plus scaling on the GPU.
class PS4PigletVideoRenderer final : public IVideoRenderer {
public:
    static bool Initialize();
    static void Shutdown();
    static bool IsReady();
    static bool Present(SDL_Surface* overlay, bool show_video, bool overlay_changed = true);
    static float GetSwapFps();
    static uint32_t GetSuccessfulSwaps();
    static uint64_t GetFirstSwapTimestampMs();
    static uint64_t GetLastSwapTimestampMs();

    ~PS4PigletVideoRenderer() override;
    void draw(NVGcontext*,int,int,AVFrame*,int) override;
    bool drawLatest(NVGcontext*,int,int,AVFrame*,int,uint64_t) override;
    VideoRenderStats* video_render_stats() override { return &stats_; }
    int getFrameColorspace(const AVFrame*) override;
    bool isFrameFullRange(const AVFrame*) override;

private:
    static bool UploadFrame(AVFrame*,uint64_t);
    static bool DrawVideo();
    static bool DrawOverlay(SDL_Surface*);
    static void DestroyGl();
    static bool ready_;
    static unsigned int program_;
    // Triple-buffer YUV textures so CPU uploads do not overwrite the plane
    // set that the GPU may still be sampling for the previous frame.
    static unsigned int textures_[9];
    static int width_,height_,format_,plane_count_;
    static uint64_t uploaded_generation_;
    static int texture_set_;
    static std::vector<unsigned char> planes_[3];
    static int sampler_y_,sampler_u_,sampler_v_,sampler_uv_;
    static int position_attr_,uv_attr_,range_uniform_,matrix_uniform_,offset_uniform_,three_plane_uniform_,green_v_uniform_;
    static int overlay_sampler_;
    static unsigned int overlay_texture_;
    static unsigned int overlay_program_;
    static int overlay_width_,overlay_height_;
    static bool full_range_,bt709_;
    static bool overlay_initialized_;
    static VideoRenderStats stats_;
    static unsigned int rendered_frames_;
    static uint64_t measurement_start_ms_;
    static int presentation_error_;
};

#endif
