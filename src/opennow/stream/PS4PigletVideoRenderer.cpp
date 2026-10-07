#include "PS4PigletVideoRenderer.hpp"

#ifdef __ORBIS__

#include <EGL/egl.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
#include <orbis/Pigletv2VSH.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include "../stream_startup_diagnostics.hpp"
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace {
constexpr int kScreenWidth=1920, kScreenHeight=1080;
EGLDisplay g_display=EGL_NO_DISPLAY;
EGLSurface g_surface=EGL_NO_SURFACE;
EGLContext g_context=EGL_NO_CONTEXT;
int g_piglet_module=-1, g_shaders_module=-1;
static int32_t g_video_out_handle=-1;
float g_vertices[]={-1.f,-1.f,0.f,1.f, 1.f,-1.f,1.f,1.f,
                    -1.f,1.f,0.f,0.f, 1.f,1.f,1.f,0.f};
uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
int load_module(const char* name) {
    char path[256];
    const char* sandbox=sceKernelGetFsSandboxRandomWord();
    if(!sandbox) {
        char detail[128];
        snprintf(detail,sizeof(detail),"module=%s reason=null_sandbox",name);
        opennow::LogAppLifecycleEvent("PIGLET_MODULE_LOAD_FAIL",detail);
        return -1;
    }
    snprintf(path,sizeof(path),"/%s/common/lib/%s",sandbox,name);
    int start_result=0;
    int rc=static_cast<int>(sceKernelLoadStartModule(path,0,nullptr,0,nullptr,&start_result));
    char detail[256];
    if(rc<0) {
        snprintf(detail,sizeof(detail),"module=%s path=%s rc=0x%08X start_result=%d",name,path,static_cast<unsigned>(rc),start_result);
        opennow::LogAppLifecycleEvent("PIGLET_MODULE_LOAD_FAIL",detail);
    } else {
        snprintf(detail,sizeof(detail),"module=%s handle=0x%08X start_result=%d",name,static_cast<unsigned>(rc),start_result);
        opennow::LogAppLifecycleEvent("PIGLET_MODULE_LOAD_OK",detail);
    }
    return rc;
}
GLuint compile_shader(GLenum type,const char* source) {
    GLuint shader=glCreateShader(type);
    if(!shader) return 0;
    glShaderSource(shader,1,&source,nullptr);
    glCompileShader(shader);
    GLint ok=GL_FALSE;
    glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) { glDeleteShader(shader); return 0; }
    return shader;
}
void copy_rows(std::vector<unsigned char>& destination,const uint8_t* source,
               int source_stride,int row_bytes,int rows) {
    destination.resize(static_cast<size_t>(row_bytes)*static_cast<size_t>(rows));
    for(int y=0;y<rows;y++)
        std::memcpy(destination.data()+static_cast<size_t>(y)*row_bytes,
                    source+static_cast<ptrdiff_t>(y)*source_stride,static_cast<size_t>(row_bytes));
}
void configure_texture(GLuint texture) {
    glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
}
} // namespace

bool PS4PigletVideoRenderer::ready_=false;
GLuint PS4PigletVideoRenderer::program_=0;
GLuint PS4PigletVideoRenderer::textures_[9]={0,0,0,0,0,0,0,0,0};
int PS4PigletVideoRenderer::width_=0;
int PS4PigletVideoRenderer::height_=0;
int PS4PigletVideoRenderer::format_=-1;
int PS4PigletVideoRenderer::plane_count_=0;
uint64_t PS4PigletVideoRenderer::uploaded_generation_=UINT64_MAX;
int PS4PigletVideoRenderer::texture_set_=-1;
std::vector<unsigned char> PS4PigletVideoRenderer::planes_[3];
int PS4PigletVideoRenderer::sampler_y_=-1;
int PS4PigletVideoRenderer::sampler_u_=-1;
int PS4PigletVideoRenderer::sampler_v_=-1;
int PS4PigletVideoRenderer::sampler_uv_=-1;
int PS4PigletVideoRenderer::position_attr_=-1;
int PS4PigletVideoRenderer::uv_attr_=-1;
int PS4PigletVideoRenderer::range_uniform_=-1;
int PS4PigletVideoRenderer::matrix_uniform_=-1;
int PS4PigletVideoRenderer::offset_uniform_=-1;
int PS4PigletVideoRenderer::three_plane_uniform_=-1;
int PS4PigletVideoRenderer::green_v_uniform_=-1;
int PS4PigletVideoRenderer::overlay_sampler_=-1;
GLuint PS4PigletVideoRenderer::overlay_texture_=0;
int PS4PigletVideoRenderer::overlay_width_=0;
int PS4PigletVideoRenderer::overlay_height_=0;
GLuint PS4PigletVideoRenderer::overlay_program_=0;
bool PS4PigletVideoRenderer::full_range_=false;
bool PS4PigletVideoRenderer::bt709_=false;
bool PS4PigletVideoRenderer::overlay_initialized_=false;
VideoRenderStats PS4PigletVideoRenderer::stats_{};
unsigned int PS4PigletVideoRenderer::rendered_frames_=0;
uint64_t PS4PigletVideoRenderer::measurement_start_ms_=0;
int PS4PigletVideoRenderer::presentation_error_=0;
namespace {
uint32_t g_swap_frames=0;
float g_swap_fps=0.f;
uint64_t g_swap_measurement_start_ms=0;
uint64_t g_first_swap_timestamp_ms=0;
uint32_t g_successful_swaps=0;
uint64_t g_last_swap_timestamp_ms=0;
}

bool PS4PigletVideoRenderer::Initialize() {
    if(ready_) return true;
    opennow::LogAppLifecycleEvent("PIGLET_INIT_BEGIN","target=Pigletv2VSH");
    g_piglet_module=load_module("libScePigletv2VSH.sprx");
    if(g_piglet_module<0) { Shutdown(); return false; }
    g_shaders_module=load_module("libScePrecompiledShaders.sprx");
    if(g_shaders_module<0) { Shutdown(); return false; }

    // =====================================================================
    // CONFIGURACION DE PIGLET: copiada EXACTAMENTE de la muestra oficial del SDK.
    //
    // Referencia: tools/openorbis/OpenOrbis/PS4Toolchain/samples/piglet/piglet/PigletApplication.cpp
    // (lineas 144-158). Es la unica secuencia que el propio SDK garantiza que funciona.
    //
    // QUE ESTABA MAL Y POR QUE `eglGetDisplay` DEVOLVIA EGL_NO_DISPLAY:
    // nuestros tamanos de memoria eran OCHO VECES mas pequenos que los oficiales y faltaban dos
    // campos del bloque de comandos de depuracion. `scePigletSetConfigurationVSH` los aceptaba
    // (devolvia exito) pero Piglet no podia construir el display con tan poca memoria reservada,
    // asi que `eglGetDisplay(EGL_DEFAULT_DISPLAY)` fallaba.
    //
    //     parametro                 antes       ahora (oficial)
    //     systemSharedMemorySize    32 MB       250 MB
    //     videoSharedMemorySize     64 MB       512 MB
    //     maxMappedFlexibleMemory   32 MB       170 MB
    //     dbgPosCmd_0x48            (ausente)   0
    //     dbgPosCmd_0x4C            (ausente)   0
    //
    // La PS4 reserva hasta 4608 MB de memoria directa (medido en el arranque), asi que estos
    // tamanos caben de sobra.
    // =====================================================================
    OrbisPglConfig config{};
    std::memset(&config,0,sizeof(config));      // la muestra oficial limpia antes de rellenar
    config.size=sizeof(config);
    config.flags=ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT|ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY|0x60;
    config.processOrder=1;
    config.systemSharedMemorySize=250ULL*1024*1024;
    config.videoSharedMemorySize=512ULL*1024*1024;
    config.maxMappedFlexibleMemory=170ULL*1024*1024;
    config.drawCommandBufferSize=1U*1024*1024;
    config.lcueResourceBufferSize=1U*1024*1024;
    config.dbgPosCmd_0x40=kScreenWidth;
    config.dbgPosCmd_0x44=kScreenHeight;
    config.dbgPosCmd_0x48=0;
    config.dbgPosCmd_0x4C=0;
    config.unk_0x5C=2;
    if(!scePigletSetConfigurationVSH(&config)) {
        opennow::LogAppLifecycleEvent("PIGLET_CONFIG_FAIL","stage=scePigletSetConfigurationVSH");
        Shutdown(); return false;
    }
    opennow::LogAppLifecycleEvent("PIGLET_CONFIG_OK","shared_sys=250MB shared_vid=512MB flex=170MB official_sdk_values=1");

    // =====================================================================
    // ORDEN CORREGIDO: el puerto de VideoOut se abre ANTES de crear el display EGL.
    //
    // BUG QUE ESTO CORRIGE (medido en consola durante muchas versiones):
    //     PIGLET_EGL_GET_DISPLAY_FAIL rc=EGL_NO_DISPLAY
    //     PIGLET_FALLBACK_SDL reason=piglet_init_failed
    //
    // `sceVideoOutOpen` estaba 32 lineas DESPUES de `eglGetDisplay`. Piglet construye el display
    // EGL sobre el puerto de salida de video de la consola: sin puerto abierto no hay display, y
    // `eglGetDisplay(EGL_DEFAULT_DISPLAY)` devuelve EGL_NO_DISPLAY.
    //
    // Consecuencia de no tenerlo: la UI se dibujaba por SDL en SOFTWARE, compitiendo por la CPU con
    // el decodificador y el escalado. Esa saturacion es la que hace que el servidor baje el bitrate
    // y la resolucion a 960x540 (medido: bilinear_scaled tras el fallo de Piglet).
    // =====================================================================
    g_video_out_handle=sceVideoOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,ORBIS_VIDEO_OUT_BUS_MAIN,0,nullptr);
    if(g_video_out_handle<=0) {
        char detail[96];
        snprintf(detail,sizeof(detail),"handle=%d rc=0x%08X",static_cast<int>(g_video_out_handle),static_cast<unsigned>(g_video_out_handle));
        opennow::LogAppLifecycleEvent("PIGLET_VIDEOOUT_OPEN_FAIL",detail);
        // Sin puerto de salida no puede haber display EGL: se aborta aqui en vez de seguir y fallar
        // mas adelante, para que el log diga la causa raiz.
        Shutdown(); return false;
    } else {
        char detail[64]; snprintf(detail,sizeof(detail),"handle=%d",static_cast<int>(g_video_out_handle));
        opennow::LogAppLifecycleEvent("PIGLET_VIDEOOUT_OPEN_OK",detail);
    }

    // =====================================================================
    // DISPLAY EGL: dos intentos.
    //
    // INTENTO 1: eglGetDisplay(EGL_DEFAULT_DISPLAY) — es lo que hace la muestra OFICIAL del SDK
    // (samples/piglet/piglet/PigletApplication.cpp, linea 164) y lo que hemos usado siempre. Con
    // los valores de memoria oficiales aplicados SIGUE devolviendo EGL_NO_DISPLAY, asi que el
    // problema no es la configuracion.
    //
    // INTENTO 2: eglGetDisplay((EGLNativeDisplayType)handle_del_puerto). En varias implementaciones
    // de EGL el display nativo ES el identificador del dispositivo de salida; si Piglet sigue esa
    // convencion, pasarle el handle del puerto que acabamos de abrir (y que SI es valido,
    // PIGLET_VIDEOOUT_OPEN_OK) deberia dar un display.
    //
    // Se prueban LOS DOS y se registra el resultado de cada uno, para que el log diga exactamente
    // cual funciona en vez de dejar la duda. Es un cambio de bajo riesgo: solo anade un segundo
    // intento cuando el primero falla.
    // =====================================================================
    g_display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    {
        char d[128];
        snprintf(d,sizeof(d),"attempt=1 source=EGL_DEFAULT_DISPLAY display=%p eglGetError=0x%04X",
                 reinterpret_cast<void*>(g_display),static_cast<unsigned>(eglGetError()));
        opennow::LogAppLifecycleEvent("PIGLET_EGL_DISPLAY_TRY",d);
    }
    if(g_display==EGL_NO_DISPLAY) {
        // INTENTO 2: el handle del puerto de VideoOut como display nativo.
        g_display=eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(g_video_out_handle));
        {
            char d[160];
            snprintf(d,sizeof(d),
                     "attempt=2 source=videoout_handle handle=%d display=%p eglGetError=0x%04X",
                     static_cast<int>(g_video_out_handle),reinterpret_cast<void*>(g_display),
                     static_cast<unsigned>(eglGetError()));
            opennow::LogAppLifecycleEvent("PIGLET_EGL_DISPLAY_TRY",d);
        }
    }
    if(g_display==EGL_NO_DISPLAY) {
        opennow::LogAppLifecycleEvent("PIGLET_EGL_GET_DISPLAY_FAIL",
                                      "rc=EGL_NO_DISPLAY both_attempts_failed videoout_open=1");
        Shutdown(); return false;
    }
    {
        char okDetail[96];
        snprintf(okDetail,sizeof(okDetail),"display=%p attempts_exhausted=0",reinterpret_cast<void*>(g_display));
        opennow::LogAppLifecycleEvent("PIGLET_EGL_GET_DISPLAY_OK",okDetail);
    }

    // CAMBIO 3: Capturar SIEMPRE el valor de retorno Y eglGetError() por separado
    EGLint major=0,minor=0;
    EGLBoolean init_rc=eglInitialize(g_display,&major,&minor);
    EGLint init_err=eglGetError();
    char initDetail[96];
    snprintf(initDetail,sizeof(initDetail),"value=%d eglGetError=0x%04X major=%d minor=%d",
             static_cast<int>(init_rc),static_cast<unsigned>(init_err),major,minor);
    opennow::LogAppLifecycleEvent("PIGLET_EGL_INITIALIZE_RC",initDetail);
    if(!init_rc) { Shutdown(); return false; }

    const EGLint config_attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig egl_config=0; EGLint count=0;
    EGLBoolean cfg_rc=eglChooseConfig(g_display,config_attrs,&egl_config,1,&count);
    EGLint cfg_err=eglGetError();
    char cfgDetail[96];
    snprintf(cfgDetail,sizeof(cfgDetail),"value=%d eglGetError=0x%04X count=%d",
             static_cast<int>(cfg_rc),static_cast<unsigned>(cfg_err),count);
    opennow::LogAppLifecycleEvent("PIGLET_EGL_CHOOSE_CONFIG_RC",cfgDetail);
    if(!cfg_rc||count<1) { Shutdown(); return false; }

    const EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    const EGLint surface_attrs[]={EGL_RENDER_BUFFER,EGL_BACK_BUFFER,EGL_NONE};
    // NOTA: el puerto de VideoOut YA se abrio al principio de esta funcion, antes de eglGetDisplay.
    // Aqui estaba el original y por eso el display EGL nunca se creaba.

    OrbisPglWindow native_window{
        static_cast<khronos_uint32_t>(g_video_out_handle>0?g_video_out_handle:0),
        static_cast<khronos_uint32_t>(kScreenWidth),
        static_cast<khronos_uint32_t>(kScreenHeight),
        0
    };
    g_surface=eglCreateWindowSurface(g_display,egl_config,&native_window,surface_attrs);
    EGLint surf_err=eglGetError();
    char surfDetail[96];
    snprintf(surfDetail,sizeof(surfDetail),"surface=%p eglGetError=0x%04X vout_handle=%d",
             reinterpret_cast<void*>(g_surface),static_cast<unsigned>(surf_err),static_cast<int>(g_video_out_handle));
    opennow::LogAppLifecycleEvent("PIGLET_EGL_CREATE_WINDOW_RC",surfDetail);
    if(g_surface==EGL_NO_SURFACE) { Shutdown(); return false; }

    g_context=eglCreateContext(g_display,egl_config,EGL_NO_CONTEXT,context_attrs);
    EGLint ctx_err=eglGetError();
    char ctxDetail[96];
    snprintf(ctxDetail,sizeof(ctxDetail),"context=%p eglGetError=0x%04X",
             reinterpret_cast<void*>(g_context),static_cast<unsigned>(ctx_err));
    opennow::LogAppLifecycleEvent("PIGLET_EGL_CREATE_CONTEXT_RC",ctxDetail);
    if(g_context==EGL_NO_CONTEXT) { Shutdown(); return false; }

    EGLBoolean cur_rc=eglMakeCurrent(g_display,g_surface,g_surface,g_context);
    EGLint cur_err=eglGetError();
    char curDetail[96];
    snprintf(curDetail,sizeof(curDetail),"value=%d eglGetError=0x%04X",
             static_cast<int>(cur_rc),static_cast<unsigned>(cur_err));
    opennow::LogAppLifecycleEvent("PIGLET_EGL_MAKE_CURRENT_RC",curDetail);
    if(!cur_rc) { Shutdown(); return false; }

    EGLBoolean swap_rc=eglSwapInterval(g_display,1);
    EGLint swap_err=eglGetError();
    char swapDetail[96];
    snprintf(swapDetail,sizeof(swapDetail),"value=%d eglGetError=0x%04X",
             static_cast<int>(swap_rc),static_cast<unsigned>(swap_err));
    opennow::LogAppLifecycleEvent("PIGLET_EGL_SWAP_INTERVAL_RC",swapDetail);
    if(!swap_rc) { Shutdown(); return false; }

    static const char* vertex=
        "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;"
        "void main(){gl_Position=vec4(a_pos,0.0,1.0);v_uv=a_uv;}";
    static const char* fragment=
        "precision mediump float; varying vec2 v_uv;"
        "uniform sampler2D u_y; uniform sampler2D u_u; uniform sampler2D u_v; uniform sampler2D u_uv;"
        "uniform float u_three; uniform float u_y_offset; uniform float u_y_scale;"
        "uniform vec3 u_coeff; uniform float u_gu; uniform float u_gv;"
        "void main(){float y=(texture2D(u_y,v_uv).r-u_y_offset)*u_y_scale;"
        "vec2 uv; if(u_three>0.5) uv=vec2(texture2D(u_u,v_uv).r,texture2D(u_v,v_uv).r);"
        "else uv=texture2D(u_uv,v_uv).ra; uv-=vec2(0.5);"
        "gl_FragColor=vec4(y+u_coeff.x*uv.y,y+u_gu*uv.x+u_gv*uv.y,y+u_coeff.z*uv.x,1.0);}";
    GLuint vs=compile_shader(GL_VERTEX_SHADER,vertex),fs=compile_shader(GL_FRAGMENT_SHADER,fragment);
    if(!vs||!fs) { if(vs) glDeleteShader(vs); if(fs) glDeleteShader(fs); Shutdown(); return false; }
    program_=glCreateProgram();
    glAttachShader(program_,vs); glAttachShader(program_,fs);
    glBindAttribLocation(program_,0,"a_pos"); glBindAttribLocation(program_,1,"a_uv");
    glLinkProgram(program_); glDeleteShader(vs); glDeleteShader(fs);
    GLint linked=GL_FALSE; glGetProgramiv(program_,GL_LINK_STATUS,&linked);
    if(!linked) { Shutdown(); return false; }
    position_attr_=glGetAttribLocation(program_,"a_pos");
    uv_attr_=glGetAttribLocation(program_,"a_uv");
    sampler_y_=glGetUniformLocation(program_,"u_y");
    sampler_u_=glGetUniformLocation(program_,"u_u");
    sampler_v_=glGetUniformLocation(program_,"u_v");
    sampler_uv_=glGetUniformLocation(program_,"u_uv");
    range_uniform_=glGetUniformLocation(program_,"u_y_scale");
    matrix_uniform_=glGetUniformLocation(program_,"u_coeff");
    offset_uniform_=glGetUniformLocation(program_,"u_y_offset");
    three_plane_uniform_=glGetUniformLocation(program_,"u_three");
    green_v_uniform_=glGetUniformLocation(program_,"u_gv");
    glGenTextures(9,textures_);
    glGenTextures(1,&overlay_texture_);
    configure_texture(overlay_texture_);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,kScreenWidth,kScreenHeight,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    static const char* overlay_vertex="attribute vec2 a_pos;attribute vec2 a_uv;varying vec2 v_uv;void main(){gl_Position=vec4(a_pos,0.0,1.0);v_uv=a_uv;}";
    static const char* overlay_fragment="precision mediump float;uniform sampler2D u_tex;varying vec2 v_uv;void main(){gl_FragColor=texture2D(u_tex,v_uv);}";
    GLuint ovs=compile_shader(GL_VERTEX_SHADER,overlay_vertex),ofs=compile_shader(GL_FRAGMENT_SHADER,overlay_fragment);
    if(!ovs||!ofs) { if(ovs)glDeleteShader(ovs);if(ofs)glDeleteShader(ofs);Shutdown();return false; }
    overlay_program_=glCreateProgram();glAttachShader(overlay_program_,ovs);glAttachShader(overlay_program_,ofs);
    glBindAttribLocation(overlay_program_,0,"a_pos");glBindAttribLocation(overlay_program_,1,"a_uv");glLinkProgram(overlay_program_);
    glDeleteShader(ovs);glDeleteShader(ofs);
    GLint overlay_linked=GL_FALSE;glGetProgramiv(overlay_program_,GL_LINK_STATUS,&overlay_linked);
    if(!overlay_linked) { Shutdown(); return false; }
    overlay_sampler_=glGetUniformLocation(overlay_program_,"u_tex");
    presentation_error_=glGetError();
    ready_=presentation_error_==GL_NO_ERROR;
    if(ready_) {
        // Opción A: Test frame inmediato en Initialize() (Azul PlayStation)
        glViewport(0,0,kScreenWidth,kScreenHeight);
        glClearColor(0.08f,0.18f,0.36f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if(!eglSwapBuffers(g_display,g_surface)) {
            presentation_error_=eglGetError();
            opennow::LogAppLifecycleEvent("PIGLET_INITIAL_SWAP_FAIL","stage=test_frame");
            Shutdown(); return false;
        }
        const uint64_t initial_now=now_ms();
        g_first_swap_timestamp_ms=initial_now;
        g_last_swap_timestamp_ms=initial_now;
        g_successful_swaps=1;
        opennow::LogAppLifecycleEvent("PIGLET_INITIAL_SWAP_OK","color=ps_blue");

        char detail[160];
        snprintf(detail,sizeof(detail),"handle=%d size=%dx%d mode=egl_window",
                 static_cast<int>(g_video_out_handle),kScreenWidth,kScreenHeight);
        opennow::LogAppLifecycleEvent("PIGLET_EGL_INIT_OK",detail);
    } else Shutdown();
    return ready_;
}

void PS4PigletVideoRenderer::DestroyGl() {
    if(g_display!=EGL_NO_DISPLAY && g_surface!=EGL_NO_SURFACE && g_context!=EGL_NO_CONTEXT)
        (void)eglMakeCurrent(g_display,g_surface,g_surface,g_context);
    if(program_) { glDeleteProgram(program_); program_=0; }
    if(overlay_program_) { glDeleteProgram(overlay_program_); overlay_program_=0; }
    bool any_video_texture=false;
    for(GLuint texture:textures_) any_video_texture=any_video_texture||texture!=0;
    if(any_video_texture) glDeleteTextures(9,textures_);
    for(GLuint& texture:textures_) texture=0;
    if(overlay_texture_) { glDeleteTextures(1,&overlay_texture_); overlay_texture_=0; }
    if(g_display!=EGL_NO_DISPLAY) {
        if(g_context!=EGL_NO_CONTEXT) { eglMakeCurrent(g_display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); eglDestroyContext(g_display,g_context); }
        if(g_surface!=EGL_NO_SURFACE) eglDestroySurface(g_display,g_surface);
        eglTerminate(g_display);
    }
    g_context=EGL_NO_CONTEXT; g_surface=EGL_NO_SURFACE; g_display=EGL_NO_DISPLAY;
    ready_=false; width_=height_=0; format_=-1; plane_count_=0;
    g_swap_frames=0;g_swap_fps=0.f;g_swap_measurement_start_ms=0;
    g_successful_swaps=0; g_first_swap_timestamp_ms=0; g_last_swap_timestamp_ms=0;
    overlay_initialized_=false;
    uploaded_generation_=UINT64_MAX; texture_set_=-1; overlay_width_=overlay_height_=0;
    for(auto& plane:planes_) plane.clear();
}

void PS4PigletVideoRenderer::Shutdown() {
    DestroyGl();
    int stop_result=0;
    if(g_shaders_module>=0) { (void)sceKernelStopUnloadModule(g_shaders_module,0,nullptr,0,nullptr,&stop_result);g_shaders_module=-1; }
    if(g_piglet_module>=0) { (void)sceKernelStopUnloadModule(g_piglet_module,0,nullptr,0,nullptr,&stop_result);g_piglet_module=-1; }
    if(g_video_out_handle>0) { (void)sceVideoOutClose(g_video_out_handle); g_video_out_handle=-1; }
}

bool PS4PigletVideoRenderer::IsReady(){return ready_;}
float PS4PigletVideoRenderer::GetSwapFps(){return g_swap_fps;}
uint32_t PS4PigletVideoRenderer::GetSuccessfulSwaps(){return g_successful_swaps;}
uint64_t PS4PigletVideoRenderer::GetFirstSwapTimestampMs(){return g_first_swap_timestamp_ms;}
uint64_t PS4PigletVideoRenderer::GetLastSwapTimestampMs(){return g_last_swap_timestamp_ms;}
PS4PigletVideoRenderer::~PS4PigletVideoRenderer()=default;
void PS4PigletVideoRenderer::draw(NVGcontext* vg,int width,int height,AVFrame* frame,int format){(void)drawLatest(vg,width,height,frame,format,uploaded_generation_+1);}
int PS4PigletVideoRenderer::getFrameColorspace(const AVFrame* frame){return frame&&frame->colorspace==AVCOL_SPC_BT709?COLORSPACE_REC_709:COLORSPACE_REC_601;}
bool PS4PigletVideoRenderer::isFrameFullRange(const AVFrame* frame){return frame&&frame->color_range==AVCOL_RANGE_JPEG;}

bool PS4PigletVideoRenderer::UploadFrame(AVFrame* frame,uint64_t generation) {
    if(!ready_||!frame||frame->width<16||frame->height<16||frame->width>3840||frame->height>2160||
       !frame->data[0]||frame->linesize[0]<frame->width) return false;
    const bool nv12=frame->format==AV_PIX_FMT_NV12;
    const bool planar=frame->format==AV_PIX_FMT_YUV420P||frame->format==AV_PIX_FMT_YUVJ420P;
    if(!nv12&&!planar) return false;
    const int chroma_w=(frame->width+1)/2,chroma_h=(frame->height+1)/2;
    if(!frame->data[1]||frame->linesize[1]<(nv12?chroma_w*2:chroma_w)||
       (planar&&(!frame->data[2]||frame->linesize[2]<chroma_w))) return false;
    full_range_=frame->color_range==AVCOL_RANGE_JPEG||frame->format==AV_PIX_FMT_YUVJ420P;
    bt709_=frame->colorspace==AVCOL_SPC_BT709;
    if(width_!=frame->width||height_!=frame->height||format_!=frame->format) {
        width_=frame->width;height_=frame->height;format_=frame->format;plane_count_=nv12?2:3;
        uploaded_generation_=UINT64_MAX;
        const GLenum formats[]={GL_LUMINANCE,static_cast<GLenum>(nv12?GL_LUMINANCE_ALPHA:GL_LUMINANCE),GL_LUMINANCE};
        const int widths[]={width_,chroma_w,chroma_w},heights[]={height_,chroma_h,chroma_h};
        texture_set_=-1;
        for(int set=0;set<3;++set) for(int i=0;i<plane_count_;++i) {
            const int textureIndex=set*3+i;
            configure_texture(textures_[textureIndex]);
            glTexImage2D(GL_TEXTURE_2D,0,formats[i],widths[i],heights[i],0,formats[i],GL_UNSIGNED_BYTE,nullptr);
        }
    }
    if(generation==uploaded_generation_) return true;
    // FFmpeg commonly returns tightly packed rows for the stream sizes used by
    // GFN (1280/1920 wide). In that case GLES consumes the AVFrame planes
    // directly, avoiding an extra full-frame CPU memcpy before each texture
    // upload. Keep the staging copy for padded strides; GLES2 has no portable
    // UNPACK_ROW_LENGTH support.
    const int row_bytes[]={width_,chroma_w*(nv12?2:1),chroma_w};
    const int row_counts[]={height_,chroma_h,chroma_h};
    const uint8_t* frame_planes[]={frame->data[0],frame->data[1],frame->data[2]};
    const int frame_strides[]={frame->linesize[0],frame->linesize[1],frame->linesize[2]};
    const GLenum formats[]={GL_LUMINANCE,static_cast<GLenum>(nv12?GL_LUMINANCE_ALPHA:GL_LUMINANCE),GL_LUMINANCE};
    const int target_set=(texture_set_+1)%3;
    const int texture_base=target_set*3;
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    for(int i=0;i<plane_count_;++i) {
        const uint8_t* upload=frame_planes[i];
        if(frame_strides[i]!=row_bytes[i]) {
            copy_rows(planes_[i],upload,frame_strides[i],row_bytes[i],row_counts[i]);
            upload=planes_[i].data();
        }
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0+i));
        glBindTexture(GL_TEXTURE_2D,textures_[texture_base+i]);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,
                        i==0?width_:chroma_w,
                        i==0?height_:chroma_h,
                        formats[i],GL_UNSIGNED_BYTE,upload);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    if(glGetError()!=GL_NO_ERROR) return false;
    texture_set_=target_set;
    uploaded_generation_=generation;
    return true;
}

bool PS4PigletVideoRenderer::drawLatest(NVGcontext*,int,int,AVFrame* frame,int,uint64_t generation) {
    if(!ready_) return false;
    const bool new_frame=generation!=uploaded_generation_;
    if(!UploadFrame(frame,generation)) return false;
    if(!measurement_start_ms_) measurement_start_ms_=now_ms();
    if(new_frame) ++rendered_frames_;
    const uint64_t elapsed=now_ms()-measurement_start_ms_;
    if(elapsed>=1000) {
        stats_.rendered_fps=static_cast<float>(rendered_frames_*1000.0/elapsed);
        stats_.rendered_frames=rendered_frames_;
        stats_.total_render_time=elapsed;
        measurement_start_ms_=now_ms();rendered_frames_=0;
    }
    return true;
}

bool PS4PigletVideoRenderer::DrawVideo() {
    if(!ready_||!program_||width_<=0||height_<=0||uploaded_generation_==UINT64_MAX||texture_set_<0) return false;
    glViewport(0,0,kScreenWidth,kScreenHeight);
    glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f,0.f,0.f,1.f);glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    const int texture_base=texture_set_*3;
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures_[texture_base]);glUniform1i(sampler_y_,0);
    if(format_==AV_PIX_FMT_NV12) {
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,textures_[texture_base+1]);glUniform1i(sampler_uv_,1);
        glUniform1f(three_plane_uniform_,0.f);
    } else {
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,textures_[texture_base+1]);glUniform1i(sampler_u_,1);
        glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,textures_[texture_base+2]);glUniform1i(sampler_v_,2);
        glUniform1f(three_plane_uniform_,1.f);
    }
    const bool full=full_range_;
    const float yoff=full?0.f:16.f/255.f;
    const float yscale=full?1.f:255.f/219.f;
    const float rv=full?(bt709_?1.5748f:1.402f):(bt709_?1.7927f:1.596f);
    const float gu=full?(bt709_?-0.1873f:-0.344136f):(bt709_?-0.2132f:-0.3918f);
    const float gv=full?(bt709_?-0.4681f:-0.714136f):(bt709_?-0.5329f:-0.813f);
    const float bu=full?(bt709_?1.8556f:1.772f):(bt709_?2.1124f:2.017f);
    glUniform1f(offset_uniform_,yoff);glUniform1f(range_uniform_,yscale);
    glUniform3f(matrix_uniform_,rv,gu,bu);glUniform1f(green_v_uniform_,gv);
    static const float vertices[]={-1.f,-1.f,0.f,1.f,1.f,-1.f,1.f,1.f,-1.f,1.f,0.f,0.f,1.f,1.f,1.f,0.f};
    glVertexAttribPointer(position_attr_,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices);
    glVertexAttribPointer(uv_attr_,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices+2);
    glEnableVertexAttribArray(position_attr_);glEnableVertexAttribArray(uv_attr_);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    return glGetError()==GL_NO_ERROR;
}

bool PS4PigletVideoRenderer::DrawOverlay(SDL_Surface* overlay) {
    if(!ready_||!overlay||overlay->w!=kScreenWidth||overlay->h!=kScreenHeight||overlay->format->format!=SDL_PIXELFORMAT_RGBA32) return false;
    if(!overlay_initialized_) {
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,kScreenWidth,kScreenHeight,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        overlay_initialized_=true;
    }
    if(overlay->pitch==kScreenWidth*4) {
        glBindTexture(GL_TEXTURE_2D,overlay_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,kScreenWidth,kScreenHeight,GL_RGBA,GL_UNSIGNED_BYTE,overlay->pixels);
    } else {
        std::vector<unsigned char> tight(static_cast<size_t>(kScreenWidth)*kScreenHeight*4);
        const auto* pixels=static_cast<const unsigned char*>(overlay->pixels);
        for(int y=0;y<kScreenHeight;y++) std::memcpy(tight.data()+static_cast<size_t>(y)*kScreenWidth*4,pixels+static_cast<size_t>(y)*overlay->pitch,kScreenWidth*4);
        glBindTexture(GL_TEXTURE_2D,overlay_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,kScreenWidth,kScreenHeight,GL_RGBA,GL_UNSIGNED_BYTE,tight.data());
    }
    glUseProgram(0);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(overlay_program_);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,overlay_texture_);
    glUniform1i(overlay_sampler_,0);
    static const float vertices[]={-1.f,-1.f,0.f,1.f,1.f,-1.f,1.f,1.f,-1.f,1.f,0.f,0.f,1.f,1.f,1.f,0.f};
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices+2);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glDisable(GL_BLEND);
    return glGetError()==GL_NO_ERROR;
}

bool PS4PigletVideoRenderer::Present(SDL_Surface* overlay,bool show_video,bool overlay_changed) {
    if(!ready_||!overlay) return false;
    if(show_video && uploaded_generation_!=UINT64_MAX) {
        if(!DrawVideo()) { presentation_error_=glGetError();return false; }
    } else { glViewport(0,0,kScreenWidth,kScreenHeight);glClearColor(0.f,0.f,0.f,1.f);glClear(GL_COLOR_BUFFER_BIT); }
    if(overlay_changed || !overlay_initialized_) {
        if(!DrawOverlay(overlay)) { presentation_error_=glGetError();return false; }
    } else {
        glUseProgram(0);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(overlay_program_);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,overlay_texture_);
        glUniform1i(overlay_sampler_,0);
        static const float vertices[]={-1.f,-1.f,0.f,1.f,1.f,-1.f,1.f,1.f,-1.f,1.f,0.f,0.f,1.f,1.f,1.f,0.f};
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices+2);
        glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glDisable(GL_BLEND);
    }
    if(!eglSwapBuffers(g_display,g_surface)) { presentation_error_=eglGetError();return false; }
    const uint64_t swap_now=now_ms();
    if(g_successful_swaps==0) {
        g_first_swap_timestamp_ms=swap_now;
        char detail[64];
        snprintf(detail,sizeof(detail),"timestamp_ms=%llu",static_cast<unsigned long long>(swap_now));
        opennow::LogAppLifecycleEvent("PIGLET_FIRST_SWAP_OK",detail);
    }
    g_last_swap_timestamp_ms=swap_now;
    ++g_successful_swaps;
    if(!g_swap_measurement_start_ms) g_swap_measurement_start_ms=swap_now;
    ++g_swap_frames;
    const uint64_t swap_elapsed=swap_now-g_swap_measurement_start_ms;
    if(swap_elapsed>=1000) {
        g_swap_fps=static_cast<float>(g_swap_frames*1000.0/swap_elapsed);
        g_swap_frames=0;
        g_swap_measurement_start_ms=swap_now;
    }
    return true;
}

#endif
