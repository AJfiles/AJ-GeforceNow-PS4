#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <orbis/Pigletv2VSH.h>
#include <orbis/libkernel.h>

enum { kWidth = 1920, kHeight = 1080 };

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;

// =============================================================================================
// LOG A FICHERO — POR QUE ESTA SONDA ANTES NO DECIA NADA
// =============================================================================================
// La version anterior solo llamaba a `sceKernelDebugOutText`, que escribe en el canal de depuracion
// del kernel. **En una consola con GoldHEN ese canal no llega a ningun fichero que se pueda mirar**,
// asi que la sonda fallaba y no quedaba ni rastro.
//
// Y el diseno era: pantalla ROJA con el numero de etapa. El problema es que si el fallo ocurre ANTES
// de tener un contexto GLES, `fail_visible` NO puede pintar nada (necesita `g_display` y
// `eglMakeCurrent`) y la pantalla se queda en NEGRO, colgada, sin decir en que etapa murio.
//
// Por eso ahora se escribe SIEMPRE un fichero de texto, en `/data/` (que es escribible y sobrevive a
// la reinstalacion del PKG). Se abre y se cierra en cada linea a proposito: si el proceso muere en la
// llamada siguiente, **la ultima linea escrita es la que sobrevive**, que es justo el dato que hace
// falta. No se usa `fflush` diferido ni un buffer abierto, porque un cierre seco se lo llevaria.
#define PROBE_LOG "/data/aj_piglet_probe.log"

static void probe_log(const char* message) {
    // El canal del kernel se mantiene: no cuesta nada y en un emulador o con un TTY si se ve.
    sceKernelDebugOutText(0, message);
    sceKernelDebugOutText(0, "\n");

    FILE* f = fopen(PROBE_LOG, "a");
    if (!f) return;
    fprintf(f, "%s\n", message);
    fclose(f);   // cerrar siempre: garantiza que la linea esta en disco antes de seguir
}

static void debug_line(const char* message) {
    probe_log(message);
}

static int load_runtime_module(const char* name) {
    char path[256];
    const char* sandbox = sceKernelGetFsSandboxRandomWord();
    if (!sandbox || snprintf(path, sizeof(path), "/%s/common/lib/%s", sandbox, name) <= 0)
        return -1;
    int module_start_result = 0;
    return (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &module_start_result);
}

static void fail_visible(int stage) {
    char message[80];
    snprintf(message, sizeof(message), "AJ GLES PROBE FAILED AT STAGE %d", stage);
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

    // =========================================================================================
    // UNA LINEA DE LOG POR PASO
    // =========================================================================================
    // El valor de cada `return false` se imprime aqui porque el mensaje "FAILED AT STAGE 3" no dice
    // CUAL de los nueve pasos de esta funcion fallo. Con estos logs, la ultima linea del fichero
    // identifica el paso exacto. Cada linea se escribe y se cierra (ver `probe_log`), asi que la
    // ultima que aparezca es la del paso que mato el proceso.
    probe_log("STEP 1/9 scePigletSetConfigurationVSH");
    if (!scePigletSetConfigurationVSH(&piglet)) {
        probe_log("STEP 1/9 FAILED: scePigletSetConfigurationVSH devolvio 0");
        return false;
    }

    probe_log("STEP 2/9 eglGetDisplay(EGL_DEFAULT_DISPLAY)");
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        // ESTE es el fallo historico de nuestro cliente: EGL_NO_DISPLAY (0x3000). Se registra el
        // codigo de error de EGL, que es lo unico que permite distinguir las causas.
        char buf[128];
        snprintf(buf, sizeof(buf), "STEP 2/9 FAILED: eglGetDisplay=EGL_NO_DISPLAY eglGetError=0x%04X",
                 (unsigned)eglGetError());
        probe_log(buf);
        return false;
    }

    probe_log("STEP 3/9 eglInitialize");
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_display, &major, &minor)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "STEP 3/9 FAILED: eglInitialize eglGetError=0x%04X", (unsigned)eglGetError());
        probe_log(buf);
        return false;
    }
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "STEP 3/9 OK: EGL %d.%d", (int)major, (int)minor);
        probe_log(buf);
    }

    probe_log("STEP 4/9 eglBindAPI(EGL_OPENGL_ES_API)");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "STEP 4/9 FAILED: eglBindAPI eglGetError=0x%04X", (unsigned)eglGetError());
        probe_log(buf);
        return false;
    }

    const EGLint config_attributes[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLE_BUFFERS, 0,
        EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE
    };
    probe_log("STEP 5/9 eglChooseConfig");
    EGLConfig config = 0;
    EGLint config_count = 0;
    if (!eglChooseConfig(g_display, config_attributes, &config, 1, &config_count) || config_count < 1) {
        char buf[128];
        snprintf(buf, sizeof(buf), "STEP 5/9 FAILED: eglChooseConfig count=%d eglGetError=0x%04X",
                 (int)config_count, (unsigned)eglGetError());
        probe_log(buf);
        return false;
    }

    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const EGLint surface_attributes[] = { EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE };
    OrbisPglWindow window = { 0, kWidth, kHeight, 0 };
    probe_log("STEP 6/9 eglCreateWindowSurface");
    g_surface = eglCreateWindowSurface(g_display, config, &window, surface_attributes);
    if (g_surface == EGL_NO_SURFACE) {
        char buf[128];
        snprintf(buf, sizeof(buf), "STEP 6/9 FAILED: eglCreateWindowSurface eglGetError=0x%04X",
                 (unsigned)eglGetError());
        probe_log(buf);
        return false;
    }

    probe_log("STEP 7/9 eglCreateContext(ES2)");
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, context_attributes);
    if (g_context == EGL_NO_CONTEXT) {
        char buf[128];
        snprintf(buf, sizeof(buf), "STEP 7/9 FAILED: eglCreateContext eglGetError=0x%04X",
                 (unsigned)eglGetError());
        probe_log(buf);
        return false;
    }

    probe_log("STEP 8/9 eglMakeCurrent");
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "STEP 8/9 FAILED: eglMakeCurrent eglGetError=0x%04X",
                 (unsigned)eglGetError());
        probe_log(buf);
        return false;
    }

    // OJO: `eglSwapInterval(1)` es lo que hace nuestro cliente, y el codigo de DolphinPS4 advierte
    // que en PS4 un swap interval de 1 puede hacer que `eglSwapBuffers` se bloquee PARA SIEMPRE.
    // Se deja a 1 para que la sonda reproduzca exactamente las condiciones de nuestro cliente: si se
    // cuelga aqui, ese es precisamente el dato que buscamos.
    probe_log("STEP 9/9 eglSwapInterval(1) + glGetString");
    (void)eglSwapInterval(g_display, 1);
    if (glGetString(GL_VERSION) == NULL) {
        probe_log("STEP 9/9 FAILED: glGetString(GL_VERSION) es NULL");
        return false;
    }
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "STEP 9/9 OK: GL_VERSION=%s", (const char*)glGetString(GL_VERSION));
        probe_log(buf);
    }
    return true;
}

int main(void) {
    // Marca de arranque. Si el fichero existe pero no tiene esta linea, el proceso murio antes de
    // llegar aqui (carga del PKG o del runtime).
    probe_log("===== AJ GLES PROBE START (PAID 0x3100000000000002) =====");
    probe_log("AJ GLES PROBE: loading Piglet runtime");
    if (load_runtime_module("libScePigletv2VSH.sprx") < 0) fail_visible(1);
    probe_log("OK: libScePigletv2VSH.sprx cargado");
    if (load_runtime_module("libScePrecompiledShaders.sprx") < 0) fail_visible(2);
    probe_log("OK: libScePrecompiledShaders.sprx cargado");
    probe_log("AJ GLES PROBE: creating EGL ES2 context");
    if (!create_gles_context()) fail_visible(3);
    probe_log("AJ GLES PROBE: EGL/GLES READY; green stripe means active GPU presentation");

    uint32_t frame = 0;
    for (;;) {
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.025f, 0.045f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        const int x = (int)((frame * 18U) % (kWidth - 260));
        glScissor(x, kHeight - 38, 260, 18);
        glClearColor(0.40f, 1.0f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        if (!eglSwapBuffers(g_display, g_surface)) fail_visible(4);
        ++frame;
    }
}
