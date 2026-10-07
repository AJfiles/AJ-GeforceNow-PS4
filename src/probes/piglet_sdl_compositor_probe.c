/*
 * AJ SDL-to-Piglet compositor probe.
 * SDL's software renderer paints a small UI layer off-screen. Piglet GLES2
 * uploads that layer as a texture and owns the only on-screen EGL swap.
 * This deliberately does not include GeForce NOW signaling or video decode.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <orbis/Pigletv2VSH.h>
#include <orbis/libkernel.h>

enum { kWidth = 1920, kHeight = 1080 };

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;
static SDL_Surface *g_ui_surface;
static SDL_Renderer *g_ui_renderer;
static uint8_t *g_ui_pixels;
static GLuint g_ui_texture;
static GLuint g_ui_program;
static GLuint g_video_program;
static GLuint g_luma_texture;
static GLuint g_chroma_texture;
static GLint g_position;
static GLint g_texcoord;
static GLint g_ui_sampler;
static GLint g_luma_sampler;
static GLint g_chroma_sampler;
static uint8_t g_luma[640 * 360];
static uint8_t g_chroma[320 * 180 * 2];

// =============================================================================================
// LOG A FICHERO — ver la nota larga en `piglet_gles_probe.c`
// =============================================================================================
// `sceKernelDebugOutText` escribe en el canal de depuracion del kernel, que en una consola con
// GoldHEN NO llega a ningun fichero que se pueda mirar. Por eso esta sonda fallaba sin dejar rastro.
// Ahora se escribe SIEMPRE en `/data/aj_piglet_compositor_probe.log`, abriendo y cerrando en cada
// linea: si el proceso muere en la llamada siguiente, la ultima linea escrita es la que sobrevive.
#define PROBE_LOG "/data/aj_piglet_compositor_probe.log"

static void probe_log(const char *message) {
    sceKernelDebugOutText(0, message);
    sceKernelDebugOutText(0, "\n");

    FILE *f = fopen(PROBE_LOG, "a");
    if (!f) return;
    fprintf(f, "%s\n", message);
    fclose(f);
}

static void debug_line(const char *message) {
    probe_log(message);
}

static int load_runtime_module(const char *name) {
    char path[256];
    const char *sandbox = sceKernelGetFsSandboxRandomWord();
    if (!sandbox || snprintf(path, sizeof(path), "/%s/common/lib/%s", sandbox, name) <= 0)
        return -1;
    int module_start_result = 0;
    return (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &module_start_result);
}

static void fail_visible(int stage) {
    char message[96];
    snprintf(message, sizeof(message), "AJ SDL+GLES COMPOSITOR FAILED STAGE %d", stage);
    debug_line(message);
    if (g_display != EGL_NO_DISPLAY && g_surface != EGL_NO_SURFACE &&
        g_context != EGL_NO_CONTEXT && eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        glViewport(0, 0, kWidth, kHeight);
        glClearColor(0.75f, 0.02f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(g_display, g_surface);
    }
    for (;;) (void)sceKernelSleep(1);
}

static bool create_gles_context(void) {
    OrbisPglConfig piglet = {0};
    piglet.size = sizeof(piglet);
    piglet.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    piglet.processOrder = 1;
    piglet.systemSharedMemorySize = 250ULL * 1024 * 1024;
    piglet.videoSharedMemorySize = 512ULL * 1024 * 1024;
    piglet.maxMappedFlexibleMemory = 170ULL * 1024 * 1024;
    piglet.drawCommandBufferSize = 1U * 1024 * 1024;
    piglet.lcueResourceBufferSize = 1U * 1024 * 1024;
    piglet.dbgPosCmd_0x40 = kWidth;
    piglet.dbgPosCmd_0x44 = kHeight;
    piglet.unk_0x5C = 2;
    probe_log("STEP 1/8 scePigletSetConfigurationVSH");
    if (!scePigletSetConfigurationVSH(&piglet)) { probe_log("STEP 1/8 FAILED"); return false; }

    probe_log("STEP 2/8 eglGetDisplay");
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        char b[128]; snprintf(b, sizeof(b), "STEP 2/8 FAILED: EGL_NO_DISPLAY eglGetError=0x%04X", (unsigned)eglGetError());
        probe_log(b); return false;
    }
    EGLint major = 0, minor = 0;
    probe_log("STEP 3/8 eglInitialize");
    if (!eglInitialize(g_display, &major, &minor)) {
        char b[128]; snprintf(b, sizeof(b), "STEP 3/8 FAILED eglGetError=0x%04X", (unsigned)eglGetError());
        probe_log(b); return false;
    }
    {
        char b[96]; snprintf(b, sizeof(b), "STEP 3/8 OK: EGL %d.%d", (int)major, (int)minor); probe_log(b);
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    EGLConfig config = 0;
    EGLint count = 0;
    probe_log("STEP 4/8 eglChooseConfig");
    if (!eglChooseConfig(g_display, config_attributes, &config, 1, &count) || count < 1) {
        char b[128]; snprintf(b, sizeof(b), "STEP 4/8 FAILED count=%d eglGetError=0x%04X", (int)count, (unsigned)eglGetError());
        probe_log(b); return false;
    }
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const EGLint surface_attributes[] = { EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE };
    OrbisPglWindow window = { 0, kWidth, kHeight, 0 };
    probe_log("STEP 5/8 eglCreateWindowSurface");
    g_surface = eglCreateWindowSurface(g_display, config, &window, surface_attributes);
    if (g_surface == EGL_NO_SURFACE) {
        char b[128]; snprintf(b, sizeof(b), "STEP 5/8 FAILED eglGetError=0x%04X", (unsigned)eglGetError());
        probe_log(b); return false;
    }
    probe_log("STEP 6/8 eglCreateContext");
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, context_attributes);
    if (g_context == EGL_NO_CONTEXT) {
        char b[128]; snprintf(b, sizeof(b), "STEP 6/8 FAILED eglGetError=0x%04X", (unsigned)eglGetError());
        probe_log(b); return false;
    }
    probe_log("STEP 7/8 eglMakeCurrent");
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        char b[128]; snprintf(b, sizeof(b), "STEP 7/8 FAILED eglGetError=0x%04X", (unsigned)eglGetError());
        probe_log(b); return false;
    }
    (void)eglSwapInterval(g_display, 1);
    probe_log("STEP 8/8 glGetString(GL_VERSION)");
    if (glGetString(GL_VERSION) == NULL) { probe_log("STEP 8/8 FAILED: glGetString es NULL"); return false; }
    {
        char b[160]; snprintf(b, sizeof(b), "STEP 8/8 OK: GL_VERSION=%s", (const char*)glGetString(GL_VERSION));
        probe_log(b);
    }
    return true;
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(shader); return 0; }
    return shader;
}

static bool create_compositor(void) {
    static const char *vs =
        "attribute vec2 a_position; attribute vec2 a_texcoord; varying vec2 v_texcoord;"
        "void main(){v_texcoord=a_texcoord;gl_Position=vec4(a_position,0.0,1.0);}";
    static const char *ui_fs =
        "precision mediump float; uniform sampler2D u_texture; varying vec2 v_texcoord;"
        "void main(){gl_FragColor=texture2D(u_texture,v_texcoord);} ";
    static const char *video_fs =
        "precision mediump float; uniform sampler2D u_luma; uniform sampler2D u_chroma;"
        "varying vec2 v_texcoord; void main(){"
        "float y=texture2D(u_luma,v_texcoord).r; vec2 uv=texture2D(u_chroma,v_texcoord).ra-vec2(0.5);"
        "float c=max(0.0,y-0.0625); float r=1.164*c+1.596*uv.y;"
        "float g=1.164*c-0.392*uv.x-0.813*uv.y; float b=1.164*c+2.017*uv.x;"
        "gl_FragColor=vec4(r,g,b,1.0);} ";
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint ui_f = compile_shader(GL_FRAGMENT_SHADER, ui_fs);
    GLuint video_f = compile_shader(GL_FRAGMENT_SHADER, video_fs);
    if (!v || !ui_f || !video_f) { probe_log("STEP 9: FAILED compile_shader (v/ui_f/video_f)"); return false; }
    g_ui_program = glCreateProgram();
    g_video_program = glCreateProgram();
    if (!g_ui_program || !g_video_program) { probe_log("STEP 10: FAILED glCreateProgram"); return false; }
    GLuint programs[] = { g_ui_program, g_video_program };
    GLuint fragments[] = { ui_f, video_f };
    for (int i = 0; i < 2; ++i) {
        glAttachShader(programs[i], v);
        glAttachShader(programs[i], fragments[i]);
        glBindAttribLocation(programs[i], 0, "a_position");
        glBindAttribLocation(programs[i], 1, "a_texcoord");
        glLinkProgram(programs[i]);
        GLint linked = GL_FALSE;
        glGetProgramiv(programs[i], GL_LINK_STATUS, &linked);
        if (!linked) { probe_log("STEP 11: FAILED glLinkProgram"); return false; }
    }
    glDeleteShader(v);
    glDeleteShader(ui_f);
    glDeleteShader(video_f);
    g_position = 0;
    g_texcoord = 1;
    g_ui_sampler = glGetUniformLocation(g_ui_program, "u_texture");
    g_luma_sampler = glGetUniformLocation(g_video_program, "u_luma");
    g_chroma_sampler = glGetUniformLocation(g_video_program, "u_chroma");
    glGenTextures(1, &g_ui_texture);
    glBindTexture(GL_TEXTURE_2D, g_ui_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    for (int y = 0; y < 360; ++y) for (int x = 0; x < 640; ++x)
        g_luma[y * 640 + x] = (uint8_t)(16 + (x * 219 / 640));
    for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) {
        g_chroma[(y * 320 + x) * 2] = (uint8_t)(96 + (y * 64 / 180));
        g_chroma[(y * 320 + x) * 2 + 1] = (uint8_t)(96 + (x * 64 / 320));
    }
    glGenTextures(1, &g_luma_texture);
    glBindTexture(GL_TEXTURE_2D, g_luma_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 640, 360, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, g_luma);
    glGenTextures(1, &g_chroma_texture);
    glBindTexture(GL_TEXTURE_2D, g_chroma_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 320, 180, 0,
                 GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, g_chroma);
    return glGetError() == GL_NO_ERROR;
}

static bool create_software_ui_layer(void) {
    g_ui_surface = SDL_CreateRGBSurfaceWithFormat(0, kWidth, kHeight, 32, SDL_PIXELFORMAT_RGBA32);
    if (!g_ui_surface) { probe_log("STEP 12: FAILED SDL_CreateRGBSurfaceWithFormat"); return false; }
    g_ui_renderer = SDL_CreateSoftwareRenderer(g_ui_surface);
    if (!g_ui_renderer) { probe_log("STEP 13: FAILED SDL_CreateSoftwareRenderer"); return false; }
    if (g_ui_surface->pitch != kWidth * 4) { probe_log("STEP 14: FAILED pitch != 1920*4"); return false; }
    g_ui_pixels = (uint8_t *)g_ui_surface->pixels;
    return g_ui_pixels != NULL;
}

static bool draw_frame(unsigned frame) {
    static const GLfloat vertices[] = {
        -1.f,-1.f, 0.f,1.f,  1.f,-1.f, 1.f,1.f,
        -1.f, 1.f, 0.f,0.f,  1.f, 1.f, 1.f,0.f
    };
    if (frame % 30U == 0U) {
        SDL_SetRenderDrawBlendMode(g_ui_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(g_ui_renderer, 0, 0, 0, 0);
        if (SDL_RenderClear(g_ui_renderer) != 0) { probe_log("STEP 15: FAILED SDL_RenderClear"); return false; }
        SDL_SetRenderDrawBlendMode(g_ui_renderer, SDL_BLENDMODE_BLEND);
        SDL_Rect panel = { 110, 90, 1700, 900 };
        SDL_SetRenderDrawColor(g_ui_renderer, 23, 31, 49, 230);
        if (SDL_RenderFillRect(g_ui_renderer, &panel) != 0) { probe_log("STEP 16: FAILED fill panel"); return false; }
        SDL_Rect header = { 110, 90, 1700, 12 };
        SDL_SetRenderDrawColor(g_ui_renderer, 102, 255, 44, 255);
        if (SDL_RenderFillRect(g_ui_renderer, &header) != 0) { probe_log("STEP 17: FAILED fill header"); return false; }
        SDL_Rect status = { 160, 220, 34, 34 };
        SDL_SetRenderDrawColor(g_ui_renderer, 102, 255, 44, 255);
        if (SDL_RenderFillRect(g_ui_renderer, &status) != 0) { probe_log("STEP 18: FAILED fill status"); return false; }
        SDL_Rect meter = { 160, 830, 1480, 16 };
        SDL_SetRenderDrawColor(g_ui_renderer, 55, 66, 84, 255);
        if (SDL_RenderFillRect(g_ui_renderer, &meter) != 0) { probe_log("STEP 19: FAILED fill meter"); return false; }
        meter.x = 160 + (int)((frame * 7U) % 1330U);
        meter.w = 150;
        SDL_SetRenderDrawColor(g_ui_renderer, 102, 255, 44, 255);
        if (SDL_RenderFillRect(g_ui_renderer, &meter) != 0) { probe_log("STEP 19b: FAILED fill meter2"); return false; }
        SDL_RenderPresent(g_ui_renderer); /* off-screen renderer: no VideoOut flip */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_ui_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kWidth, kHeight,
                        GL_RGBA, GL_UNSIGNED_BYTE, g_ui_pixels);
        if (glGetError() != GL_NO_ERROR) { probe_log("STEP 21: FAILED glGetError tras subir texturas"); return false; }
    }
    glViewport(0, 0, kWidth, kHeight);
    glUseProgram(g_video_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_luma_texture);
    glUniform1i(g_luma_sampler, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_chroma_texture);
    glUniform1i(g_chroma_sampler, 1);
    glVertexAttribPointer((GLuint)g_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer((GLuint)g_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glEnableVertexAttribArray((GLuint)g_position);
    glEnableVertexAttribArray((GLuint)g_texcoord);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_ui_texture);
    glUseProgram(g_ui_program);
    glVertexAttribPointer((GLuint)g_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer((GLuint)g_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glUniform1i(g_ui_sampler, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    return glGetError() == GL_NO_ERROR && eglSwapBuffers(g_display, g_surface) == EGL_TRUE;
}

int main(void) {
    probe_log("===== AJ SDL+GLES COMPOSITOR PROBE START (PAID 0x3100000000000002) =====");
    probe_log("AJ SDL+GLES COMPOSITOR: loading runtimes");
    if (load_runtime_module("libScePigletv2VSH.sprx") < 0) fail_visible(1);
    probe_log("OK: libScePigletv2VSH.sprx cargado");
    if (load_runtime_module("libScePrecompiledShaders.sprx") < 0) fail_visible(2);
    probe_log("OK: libScePrecompiledShaders.sprx cargado");
    if (!create_gles_context()) fail_visible(3);
    probe_log("AJ SDL+GLES COMPOSITOR: initializing off-screen SDL surface");
    if (SDL_Init(0) != 0 || !create_software_ui_layer()) fail_visible(4);
    probe_log("OK: capa SDL software lista");
    if (!create_compositor()) fail_visible(5);
    probe_log("OK: compositor GLES listo");
    probe_log("AJ SDL+GLES COMPOSITOR READY: SDL paints UI, Piglet presents it");

    // =========================================================================================
    // TRAZA DE PROGRESO
    // =========================================================================================
    // Antes, si el bucle se quedaba colgado en `eglSwapBuffers`, la sonda no decia nada mas: la
    // pantalla se quedaba negra y el log no existia. Ahora se escribe una linea cada 60 fotogramas
    // (una por segundo a 60 Hz), ANTES de presentar. Si el fichero termina en "frame N", el cuelgue
    // esta en el propio `eglSwapBuffers` de ese fotograma; si termina en "OK: compositor GLES
    // listo", el cuelgue esta ya en el primer swap.
    unsigned frame = 0;
    for (;;) {
        if (frame == 0) probe_log("bucle: primer draw_frame");
        if (!draw_frame(frame)) fail_visible(6);
        ++frame;
        if (frame % 60 == 0) {
            char b[96];
            snprintf(b, sizeof(b), "frame %u presentado OK (eglSwapBuffers no se bloquea)", frame);
            probe_log(b);
        }
    }
}


