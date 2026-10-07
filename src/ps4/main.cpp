#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <SDL2/SDL.h>
#include <orbis/Keyboard.h>
#include <orbis/Pad.h>
#include <orbis/ImeDialog.h>
#include <orbis/CommonDialog.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Random.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include "../third_party/cJSON/cJSON.h"
#include "../opennow/gfn_client.hpp"
#include "../opennow/qrcodegen.h"
#include "../opennow/webrtc_session.hpp"
#include "../opennow/stream/ffmpeg/AVFrameHolder.hpp"
#include "../opennow/stream/SDLVideoRenderer.hpp"
#include "../opennow/stream/PS4PigletVideoRenderer.hpp"
#include "../opennow/stream/PS4VideoOutRenderer.hpp"
#include "../opennow/stream/color_simd.hpp"
#include "../opennow/stream/videodec_arbitration.hpp"
#include "../opennow/stream/videodec2_probe.hpp"
#include "../opennow/stream/videodec1_probe.hpp"
#include "../opennow/session_recorder.hpp"   // registrador de sesion
#include "../opennow/trace_detail.hpp"       // traza detallada de arranque y sesion (v4.25)
#include "../opennow/stream_settings.hpp"
#include "../opennow/keyboard_input_policy.hpp"
#include "../opennow/http_client.hpp"
#include "../opennow/stream_diagnostics.hpp"
#include "../opennow/stream_startup_diagnostics.hpp"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavcodec/packet.h>
#include <stdio.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctime>
#include <unistd.h>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <pthread.h>   // pthread_self() para la prioridad del hilo principal durante el stream (v3.64)
#include <chrono>
#include <exception>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// OpenOrbis ships placeholder (void) declarations for SceMouse. Keep the ABI
// declarations local and explicit so USB mice can be polled on PS4 directly.
extern "C" {
int sceMouseInit(void);
int sceMouseOpen(int32_t userId,int32_t type,int32_t index,void* param);
int sceMouseRead(int32_t handle,void* data,int32_t count);
int sceMouseClose(int32_t handle);
}

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

// W y H son la resolucion REAL del display, no una constante. Si la ventana SDL se crea mas
// grande que el display (1920x1080 en un panel de 1280x720), el flip de SDL-PS4 falla y la
// pantalla se queda NEGRA aunque la app dibuje frames con normalidad: eso es lo que ocurrio en
// las 2.91 y 2.92. Se detectan al arrancar y se crea la ventana con las dimensiones reales.
// Declaracion anticipada: applyLogicalSize() se define mas abajo (junto a text()), pero se usa
// en las funciones de creacion/restauracion del renderizador SDL, que estan antes.
// VERSION DE LA APLICACION.
//
// El valor REAL lo inyecta scripts/build-ps4.ps1 con -DGFN_APP_VERSION, tomandolo de $appVersion.
// Se define aqui un respaldo para que el fichero siga compilando si alguien lo compila a mano.
//
// POR QUE ASI: la version estaba escrita a mano en TRES sitios de este fichero (el log de arranque,
// el inventario de build y la barra de estado) ademas de estar en el script. Al subir a 3.31 se
// desincronizaron y la auditoria de build lo detecto: "Version en build-ps4.ps1: 3.31 - en el binario:
// NO". Con una sola fuente de verdad eso no puede volver a pasar.
#ifndef GFN_APP_VERSION
#define GFN_APP_VERSION "0.00-dev"
#endif
static void applyLogicalSize();

static int W = 1920;
static int H = 1080;
static SDL_Window* window = NULL;
// Lienzo propio del renderizador (v3.84). Ver la explicacion completa donde se crea: dibujar en la
// superficie de la VENTANA causaba a la vez el parpadeo (el driver copia esa misma memoria a VideoOut)
// y el blit lento.
static SDL_Surface* g_ownCanvas = nullptr;

// =====================================================================================================
// FORMATO DEL LIENZO PROPIO (v4.06). ESTE ES EL ARREGLO DEL COLOR. LEER ANTES DE CAMBIARLO.
// =====================================================================================================
// SINTOMA QUE ARREGLA: "toda la interfaz se ve roja en lugar de negra/verde" y las caratulas y el video
// con R y B intercambiados.
//
// LA CADENA COMPLETA, con las mascaras reales del codigo de SDL y del driver:
//
//   1. El driver SDL-PS4 declara la superficie de la ventana como `SDL_PIXELFORMAT_BGR888`
//      (`SDL_ps4video.c:456`). A pesar del nombre, **`BGR888` en SDL es `PACKEDORDER_XBGR`**, y sus
//      mascaras son `Rmask=0x000000FF, Gmask=0x0000FF00, Bmask=0x00FF0000`
//      (`SDL_pixels.c:446-450`, comprobado). Es decir: **en memoria `[R][G][B][X]`**.
//
//   2. El driver envia esa memoria a VideoOut **copia cruda, sin conversion**:
//          memcpy(&pDst[...], &surface->pixels[...], sizeof(uint32_t)*drawW);   // SDL_ps4video.c:519
//
//   3. VideoOut lo registra como `SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB`
//      (`SDL_ps4video.c:221`), que en little-endian es `0xAARRGGBB` = **en memoria `[B][G][R][A]`**.
//
//   CONCLUSION: el driver manda `[R][G][B]` y el panel lee eso como `[B][G][R]` -> **R y B cambiados**.
//
//   El lienzo tiene que compensarlo: si el lienzo esta en un formato cuyo byte bajo es **R**, la cadena
//   completa queda correcta. **`BGR888` es ese formato** (`Bmask=0x00FF0000` -> el byte alto es B y el
//   bajo es R, o sea memoria `[R][G][B]`), y es ademas **el mismo formato que la ventana**, que es lo
//   que la v3.82 midio como la condicion que activa el camino rapido de SDL (27,4 ns/px en el camino
//   lento frente a 1,25 ns/px).
//
// HISTORIA, porque este formato ya se cambio mal una vez: la v3.81 puso la TEXTURA en `BGR888` y salio
// en blanco y negro. Aquello no fue por 3 bytes sino por una INCOHERENCIA DEL DRIVER: declara la
// superficie de 3 bytes pero la recorre con `uint32_t*` y `drawW*4`. Aqui la textura NO se toca (sigue
// en `ARGB8888`, que es lo que la v3.82 arreglo); lo que cambia es el LIENZO, que es el que se copia a
// la ventana con `memcpy`.
//
// SI ALGUN DIA EL VIDEO SALE CON LOS COLORES CAMBIADOS, este es el valor que hay que tocar, y el log
// `SDL_OWN_CANVAS_ON` dice cual se aplico de verdad.
//
// =====================================================================================================
// VERIFICACION CONTRA LA FUENTE DEFINITIVA, Y CONTRA UNA CONTRADICCION APARENTE (v4.07)
// =====================================================================================================
// Este diagnostico se cuestiono una vez, porque **la documentacion historica parece decir lo contrario**.
// Conviene dejar escrito por que NO lo dice:
//
//   `PS4-V3.84` incluye una tabla que afirma **"Color: correcto"** para un lienzo `ARGB8888`. **Esa tabla
//   era un PRONOSTICO, no una medicion**: el propio texto lo delata unos parrafos antes, donde dice
//   *"El color se mantiene (se escribe el mismo dato `B,G,R,A` que produce el conversor `color_simd.cpp`)"*.
//   Es decir, el razonamiento fue **"el lienzo coincide con lo que yo escribo"**, y **nadie comprobo que
//   orden de bytes LEE el panel**. Esa es exactamente la parte que faltaba.
//
//   **La fuente definitiva es el emulador**, que traduce el formato de VideoOut a un formato de GPU:
//
//       // shadPS4, src/video_core/renderer_vulkan/vk_presenter.cpp:651
//       case PixelFormat::A8B8G8R8Srgb:  return vk::Format::eR8G8B8A8Srgb;
//       case PixelFormat::A8R8G8B8Srgb:  return vk::Format::eB8G8R8A8Srgb;
//
//   **`A8B8G8R8_SRGB` es `RGBA8`**: en memoria `[R][G][B][A]`. La nomenclatura de OpenOrbis se lee al
//   reves de lo que sugiere: `A8B8G8R8` no es ABGR, es RGBA.
//
//   La tabla completa, cada valor con su fuente:
//
//       VideoOut A8B8G8R8_SRGB            -> memoria [R][G][B][A]   vk_presenter.cpp:651
//       Superficie del driver (BGR888)   -> memoria [R][G][B][X]   SDL_pixels.c:446-450
//       Lienzo ARGB8888 (lo de ANTES)    -> memoria [B][G][R][A]   SDL_pixels.c:458-461  <-- INVERTIDO
//       Lienzo BGR888 (lo de AHORA)      -> memoria [R][G][B][X]   SDL_pixels.c:446-450  <-- COINCIDE
//
//   Con el lienzo en ARGB8888 la cadena era: la UI escribe `[B][G][R]`, el driver lo copia tal cual, y el
//   panel lo lee como `[R][G][B]` -> **toda la interfaz con R y B cambiados**, que es el sintoma reportado
//   ("todo se ve rojo en lugar de negro/verde") y por que **las caratulas y los QR con color salen en
//   negativo**.
//
//   **Y de paso explica por que esto pudo pasar desapercibido tanto tiempo:** la interfaz clasica era
//   **fondo negro con texto gris**, y **con R=G=B un intercambio de canales no se ve**. En cuanto hay
//   brillos verdes y caratulas a color, se ve inmediatamente.
//
//   -------------------------------------------------------------------------------------------------
//   LA COMPROBACION, REPETIBLE EN DOS PASOS (v4.08). Hacerla ANTES de cambiar este valor otra vez.
//   -------------------------------------------------------------------------------------------------
//
//   **Paso 1 — el orden de bytes de `BGR888`.** El header del SDK lo dice con un comentario que no deja
//   lugar a dudas (`SDL_pixels.h`):
//
//       /* Array component order, low byte -> high byte. */      <- se refiere a SDL_PACKEDORDER_*
//       SDL_PIXELFORMAT_BGR888 = SDL_DEFINE_PIXELFORMAT(..., SDL_PACKEDORDER_XBGR, ...);
//
//   Y las mascaras con las que el propio SDL reconoce ese formato (`SDL_pixels.c:446-450`):
//
//       Rmask = 0x000000FF      Gmask = 0x0000FF00      Bmask = 0x00FF0000
//
//   **R en el byte BAJO** -> memoria `[R][G][B]`.
//
//   **Paso 2 — el orden que LEE VideoOut.** El emulador traduce el formato a un formato de GPU, y eso no
//   admite interpretacion:
//
//       // shadPS4, src/video_core/renderer_vulkan/vk_presenter.cpp:651
//       case PixelFormat::A8B8G8R8Srgb:  return vk::Format::eR8G8B8A8Srgb;
//
//   **`A8B8G8R8_SRGB` -> `RGBA8`** -> memoria `[R][G][B]`.
//
//   Los dos coinciden: **el lienzo tiene que estar en `[R][G][B]`, y `BGR888` es ese formato.**
//   `ARGB8888` es `[B][G][R]` (mascaras `Rmask=0x00FF0000`, `Bmask=0x000000FF`, `SDL_pixels.c:458-461`)
//   y por eso producia la inversion.
//
//   **Nota sobre la documentacion historica:** `PS4-V3.84` afirma *"Color: correcto"* con un lienzo
//   `ARGB8888`. **Esa tabla era un pronostico, no una medicion**: su propio texto dice *"se escribe el
//   mismo dato `B,G,R,A` que produce el conversor"*, es decir, razono **"coincide con lo que yo
//   escribo"** y **no comprobo que orden lee el panel**. Es el paso 2 de arriba, y es el que faltaba.
#define GFN_CANVAS_PIXELFORMAT SDL_PIXELFORMAT_BGR888

static SDL_Renderer* renderer = NULL;
static SDL_Surface* pigletOverlaySurface = nullptr;
static bool pigletVideoActive = false;
static bool videoOutDirectActive = false;
static bool g_videoOutHandoffDone = false;
// CUARENTENA DE LA RUTA DIRECTA (v3.94). Se activa cuando un fallo PERSISTENTE de flips obliga a
// degradar a SDL. Mientras este puesta, los siguientes streams de esta misma ejecucion NO vuelven a
// intentar la ruta directa: sin ella, cada entrada a jugar repetiria el tiron y la restauracion.
// Se limpia sola al reiniciar la app, que es cuando tiene sentido reintentar.
static bool g_videoOutQuarantined = false;
static bool g_videoOutDebugFlagDetected = false;
// Marca de un solo frame: el video ya esta dibujado en el lienzo. La usa el camino SDL del stream
// para NO limpiar la pantalla despues de dibujar, que era lo que la dejaba en negro.
static int g_streamVideoDrawnThisFrame = 0;
static int appLoopFps = 0;
static bool pigletStaticStreamOverlayReady=false;
static bool rendererUsesWindowSurface=false;
static bool rendererHardwareAccelerated=false;
static bool streamFirstDrawLogged=false;
static bool streamFirstLoopLogged=false,streamFirstEventPassLogged=false,streamFirstPadPassLogged=false;
static bool streamFirstPadPollBeginLogged=false;
static bool streamJoinBeginLogged=false,streamJoinDoneLogged=false,streamCoverPassLogged=false;
static bool streamFrameCallbackLogged=false;
struct Ps4MouseData { uint64_t timestamp; bool connected; uint32_t buttons; int32_t xAxis,yAxis,wheel,tilt; uint8_t reserve[8]; };
struct Ps4MouseOpenParam { uint8_t behaviorFlag; uint8_t reserve[7]; };
static int ps4MouseHandle=-1;
static uint32_t ps4MouseButtons=0;
static bool ps4MouseReady=false;
static int page = 0; // 0 home, 1 settings, 2 connection, 3 catalog, 4 login, 5 juego, 6 carga exclusiva
static int selection = 0;
static int resolution = 1;
static int bitrate = 25;
// 30 FPS POR DEFECTO (punto C del plan v3.30).
//
// POR QUE: el escalado 960x540->1280x720 (lo que el servidor entrega convertido a nuestro
// framebuffer) cuesta 13.871-15.152 us medidos, con picos de 20.469. El presupuesto a 60 FPS es de
// 16.666 us: el margen libre es del 10-17%, asi que cualquier pico del decodificador o de la UI hace
// que el frame no llegue a tiempo, y de ahi los FPS oscilando entre 20 y 70 que se observaron.
//
// A 30 FPS el presupuesto se DUPLICA a 33.333 us y el escalado deja ~19.400 us libres. Es la unica
// configuracion con margen real dado lo que el servidor entrega. El usuario puede volver a 60 FPS
// desde el ajuste de la fila 3 si prefiere fluidez a estabilidad.
static int fps = 30;
// 0 prioritizes lower delay, 1 image quality, 2 leaves the chosen profile intact.
static int networkMode = 1;
// Ajustes de calidad y sesion anadidos en la 2.89. Estaban disponibles en la API de GFN
// (struct StreamSettings) pero no se exponian ni se enviaban.
//   imageQualityMode : como reparte el servidor el bitrate. Clarity=nitidez, Balanced=equilibrio,
//                      Adaptive=prioriza fluidez. Es el unico ajuste que afecta a la nitidez
//                      SIN depender del bitrate.
//   audioBufferMs    : tamano del buffer de audio. Menos = menos latencia, mas = menos cortes.
//   gameLanguage     : idioma del texto DENTRO del juego, no de la interfaz.
static int imageQualityMode = 0;
static int audioBufferMs = 40;
static int gameLanguage = 0;
// Nivel de realce de nitidez del escalador. El realce compensa el suavizado que introduce el
// escalado bilineal cuando el servidor entrega 540p en vez de 720p.
//   0 = DESACTIVADO, 1 = SUAVE, 2 = MEDIO (por defecto), 3 = ALTO
// Es ajustable porque el nivel ideal depende del juego y del gusto: un realce excesivo genera
// halos en los bordes, y uno insuficiente deja la imagen blanda.
// POR DEFECTO DESACTIVADO (0).
// Medido en consola: el realce adaptativo tipo RCAS hace 4 lecturas y 2 mezclas por pixel, y en el
// Jaguar de la PS4 eso lleva el escalado a dispatch_avg_us=28837-33121 y los FPS a 23-25. Ni la
// eliminacion de la division (tabla de reciprocros) lo arreglo, porque el coste esta en las
// LECTURAS, no en el calculo.
//
// Se deja disponible en la interfaz porque el nivel ideal depende del juego y del equipo: quien
// prefiera nitidez a 30 FPS puede activarlo. Por defecto se prioriza la fluidez, que es lo que el
// usuario describio como "comodo".
// MODO DE DECODIFICADOR: 0=AUTO, 1=HARDWARE, 2=SOFTWARE.
//
// POR QUE EXISTE: el decodificador de video de PS4 (libSceVideodec2) falla al cargar con
// rc=0x805A1000 y su ABI de llamadas estaba sin resolver. YouTube y Twitch SI lo usan en esta misma
// consola, asi que el modulo existe y el problema es de secuencia de inicializacion.
//
// Con este ajuste se puede forzar cada camino y ver en el log cual funciona, en vez de depender de
// que el automatico acierte. AUTO intenta hardware y cae a software registrando el motivo exacto.
// MODO DE ESCALADO DE VIDEO: 0 = NATIVO (pedir 720p y escalar si el servidor baja a 540p),
// 1 = 540P FIJO (pedir 540p y NO ESCALAR: el hardware de VideoOut lo lleva a la pantalla gratis).
//
// POR QUE EXISTE (medido en consola):
//   El servidor entrega 960x540 por su cuenta en todas las sesiones. Nuestro escalado 540p->720p en
//   CPU cuesta 17-26 ms, y el presupuesto a 60 FPS es de 16,666 ms: la conversion SOLA ya lo supera
//   (VIDEOOUT_PRESENT_STAGES convert_us=647182 para 37 frames = 17.491 us por frame).
//
//   Pidiendo 540p directamente NO HAY NADA QUE ESCALAR en la CPU (0 ms), y `sceVideoOut` escala el
//   framebuffer de 960x540 a la pantalla POR HARDWARE, que es gratis y de calidad igual o mejor que
//   nuestro bilineal. Se pierde algo de nitidez teorica frente a un 720p real, pero se gana poder
//   mantener 60 FPS estables, que es lo que hace que un juego sea jugable.
//
// En modo NATIVO se conserva el comportamiento de siempre.
static int scaleMode = 0;   // 0 = 720P FIJO, 1 = 1080P FIJO
static int savedScaleMode = 0;
static int decoderMode = 0;
static int savedDecoderMode = 0;
static int sharpnessLevel = 0;
static int savedSharpnessLevel = 0;
static int savedImageQualityMode = 0;
static int savedAudioBufferMs = 40;
static int savedGameLanguage = 0;
static int savedResolution = 1;
static int savedBitrate = 25;
static int savedFps = 30;
static int savedNetworkMode = 1;
static int language = 0; // 0 Spanish, 1 English
static int savedLanguage = 0;
static int inputDevice = 2; // 0 controller, 1 keyboard/mouse, 2 both
static int savedInputDevice = 2;
static std::string streamRegion = "Auto";
static std::string savedStreamRegion = "Auto";
static std::vector<opennow::StreamRegion> settingsRegions;
static std::mutex settingsRegionsMutex;
static std::atomic<int> settingsRegionState{0}; // 0 idle, 1 loading, 2 ready, 3 unavailable
static SDL_Thread* settingsRegionThread=nullptr;
static bool rememberLogin=false;
static bool savedRememberLogin=false;
// Beta disclaimer: shown once on startup unless the user permanently disabled it.
static bool betaDisclaimerVisible=false;
static bool betaDisclaimerDontShowAgain=true;
static bool savedBetaDisclaimerDontShowAgain=false;
static int saveResult = 0;
static int testKind = 0; // 0 service reachability, 1 throughput estimate
static int controllerCount = 0;
static SDL_GameController* gameControllers[8]={nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
static SDL_Joystick* genericJoysticks[8]={nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
static bool genericSplitTriggerAxes[8]={false,false,false,false,false,false,false,false};
static int genericTriggerRest[8][2]{};
static int lastAxisX = 0;
static int lastAxisY = 0;
static std::atomic<int> probeState(0); // 0 idle, 1 running, 2 response, 3 transport/TLS error
static std::atomic<int> probeHttpStatus(0);
static std::atomic<int> probeError(0);
static std::atomic<int> probeSpeedMbps10(0);
static std::atomic<uint64_t> probeSpeedBytes(0);
static std::atomic<int> probeElapsedMs(0);
static SDL_Thread* probeThread = NULL;
struct CatalogGame { char title[160]; char store[24]; char launchAppId[96]; char imageUrl[640]; uint8_t linked; };
static CatalogGame catalogGames[2400];
// Network workers build a private snapshot. The UI thread publishes it between
// frames so catalog rendering/input never races partially-written game rows.
static CatalogGame pendingCatalogGames[2400];
static std::atomic<int> pendingCatalogCount(0);
static std::atomic<bool> pendingCatalogReady(false);
static std::atomic<int> catalogState(0); // 0 idle, 1 loading, 2 ready, 3 error
static std::atomic<int> catalogCount(0);
static std::atomic<int> catalogHttpStatus(0);
static std::atomic<int> catalogError(0);
static SDL_Thread* catalogThread = NULL;
static SDL_mutex* networkMutex = NULL;
static int catalogSelection = 0;
static int catalogOffset = 0;
static int catalogVisible[2400];
static int catalogVisibleCount = 0;
static std::string catalogSearch;
static int catalogStoreFilter = 0;
static bool catalogSearchActive = false;
static const char* catalogFilters[] = {"VINCULADOS", "TODOS", "STEAM", "EPIC", "UBISOFT", "OTROS", "FAVORITOS"};
static std::atomic<bool> catalogFullLoaded(false);
static std::atomic<bool> pendingCatalogIsFull(false);
static std::atomic<bool> catalogFullRequested(false);
struct CatalogFavorite { std::string title; std::string store; };
static std::vector<CatalogFavorite> catalogFavorites;
static std::unordered_set<std::string> catalogFavoriteIndex;
static std::atomic<int> authState(0); // 0 idle, 1 requesting, 2 waiting for QR, 3 signed in, 4 error/cancelled
static std::atomic<bool> authCancelled(false);
static SDL_Thread* authThread = NULL;
static SDL_mutex* authMutex = NULL;
static opennow::AuthSession authSession;
static opennow::QrLoginChallenge authChallenge;
static std::string authError;
static std::atomic<int> launchState(0); // 0 idle, 1 preparando, 2 lista, 3 error
static std::atomic<bool> launchCancelled(false);
static SDL_Thread* launchThread=NULL;
static SDL_Thread* stopThread=NULL;
static SDL_Thread* streamStartThread=NULL;
static std::atomic<int> streamStartState(0); // 0 idle, 1 connecting, 2 connected setup, 3 startup exception
static SDL_mutex* launchMutex=NULL;
static opennow::SessionInfo pendingGameSession;
static char launchError[512]{};
static std::atomic<int> launchQueuePosition{0};
static std::atomic<int> launchSeatEtaMs{0};
static std::atomic<int> launchPollCount{0};
static std::atomic<int> launchCloudStatus{0};
static std::chrono::steady_clock::time_point launchStartedAt{};
static std::chrono::steady_clock::time_point launchReadyAt{};
static bool launchUiLoopEnteredLogged=false;
static bool launchUiPadPollLogged=false;
static bool launchUiDrawBeginLogged=false;
static bool launchUiDrawDoneLogged=false;
static int launchUiDrawStage=0;
static SDL_Texture* catalogCoverTexture=nullptr;
static SDL_Thread* catalogCoverThread=nullptr;
static std::atomic<int> catalogCoverState{0}; // 0 idle, 1 downloading, 2 decoded, 3 unavailable
static std::atomic<bool> catalogCoverJobDone{false};
static std::atomic<uint64_t> catalogCoverGeneration{0};
static std::string catalogCoverDesiredUrl;
static std::string catalogCoverLoadedUrl;
static Uint32 catalogCoverSelectionChangedAt=0;
static std::mutex catalogCoverMutex;
static std::vector<uint8_t> catalogCoverPixels;
static int catalogCoverWidth=0, catalogCoverHeight=0;
static std::string launchAppId,launchStore,launchTitle;
static std::unique_ptr<WebRtcSession> activeStream;
// The stream-start worker owns pendingStream until the UI thread joins it.
static std::unique_ptr<WebRtcSession> pendingStream;
// activeStream is UI-thread-owned and populated only after joining the worker.
static bool hasPublishedStream() {
    return streamStartState.load(std::memory_order_acquire)==2 && activeStream!=nullptr;
}

// NUMERO DE TARJETAS DEL CENTRO DE JUEGO.
//
// HISTORIA: esto era una funcion `homeCardCount()` que devolvia 7 o 6 segun si habia una sesion en
// segundo plano, y estaba escrito a mano en TRES sitios. Ademas NO coincidia con las tarjetas que se
// dibujaban. Consecuencia visible: la cruceta movia la seleccion a un hueco que no existia.
//
// SE HA ELIMINADO LA FUNCION. Ahora el numero de tarjetas es la constante `kHomeCardCount`, definida
// junto a la unica tabla (`buildHomeCards`), y todos los sitios que la necesitan usan ESA constante.
// Dejar aqui un `homeCardCount()` que devolviera un numero distinto seria volver a crear la trampa.


// =============================================================================================
// TARJETAS DEL CENTRO DE JUEGO — UNA SOLA FUENTE DE VERDAD
// =============================================================================================
// POR QUE ESTA AQUI Y NO DENTRO DEL DIBUJADO:
//
// Antes habia DOS listas paralelas: la tabla de tarjetas (en el dibujado) y una cadena de `if` con
// indices escritos a mano (en el dispatch de la X). Al reescribir la tabla de 7 entradas a 5, el `if`
// se quedo con la numeracion vieja y se rompieron DOS cosas a la vez, ambas reportadas desde consola:
//
//   1. "ACERCA DE" DESAPARECIO. Su rama pedia `selection==5` o `selection==6`, numeros que la tabla
//      ya no producia: era CODIGO MUERTO, imposible de alcanzar.
//   2. DESFASE DE SELECCION. Al pulsar X sobre una tarjeta, la app ejecutaba la accion de OTRA,
//      porque el `if` y el dibujado hablaban de indices distintos.
//
// La tabla correcta tiene SEIS tarjetas, en tres filas de dos:
//
//     fila 0:  [0] PRUEBA DE CONEXION   [1] AJ - CONFIGURACION
//     fila 1:  [2] INICIAR SESION GFN   [3] JUGAR DESDE GEFORCE NOW
//     fila 2:  [4] PROBAR MANDO         [5] ACERCA DE
//
// Ahora hay UNA tabla, cada tarjeta lleva su ACCION, y tanto el dibujado como el dispatch la leen.
// No hay ningun indice escrito a mano en ningun sitio: es imposible que vuelvan a desincronizarse.
enum class HomeAction {
    kServiceProbe,    // -> pagina 2 (prueba de red)
    kSettings,        // -> pagina 1 (configuracion)
    kLogin,           // -> pagina 4 (y arranca el login si no hay sesion)
    kCatalog,         // -> pagina 3 (catalogo / jugar)
    kControllerTest,  // -> pagina 7 (probador de mando)
    kAbout,           // -> pagina 8 (ACERCA DE, con los tres QR)
};

struct HomeCard {
    const char* title;
    const char* detail;
    HomeAction action;
};

// Cuantas tarjetas tiene el centro de juego. Debe coincidir con el tamano de la tabla de abajo.
constexpr int kHomeCardCount = 6;
// Filas REALES de la rejilla (2 columnas x 3 filas). La navegacion y el dibujado usan ESTA constante:
// antes cada uno tenia su propio numero escrito a mano y no coincidian.
constexpr int kHomeRows = 3;

// Rellena las tarjetas. `signedIn` y `streamActive` solo cambian los TEXTOS, nunca el numero de
// tarjetas ni su orden: eso es lo que garantiza que el indice visual y el logico sean siempre el mismo.
static void buildHomeCards(HomeCard out[kHomeCardCount], bool signedIn, bool streamActive) {
    out[0]={"PRUEBA DE CONEXION","RED Y SERVICIO",HomeAction::kServiceProbe};
    out[1]={"AJ - CONFIGURACION","RESOLUCION, BITRATE E IDIOMA",HomeAction::kSettings};
    out[2]={signedIn?"SESION GFN INICIADA":"INICIAR SESION GFN",
            signedIn?"ABRIR CUENTA / CERRAR SESION":"INICIO CON CODIGO QR",HomeAction::kLogin};
    out[3]={streamActive?"ABRIR CATALOGO":"JUGAR DESDE GEFORCE NOW",
            streamActive?"BIBLIOTECA DE JUEGOS"
                        :(signedIn?"CATALOGO DE TU CUENTA":"INICIA SESION PRIMERO"),
            HomeAction::kCatalog};
    out[4]={"PROBAR MANDO","GAMEPAD TESTER",HomeAction::kControllerTest};
    // ACERCA DE: la tarjeta que habia desaparecido. Siempre presente, siempre en el indice 5.
    out[5]={"ACERCA DE","PROYECTO AJ / GFN PS4",HomeAction::kAbout};
}

// Indice de la tarjeta del centro de juego que lleva a una accion concreta.
// Devuelve 0 si no se encuentra (nunca deberia pasar: la tabla es fija).
//
// PARA QUE SIRVE: al salir de otra pantalla hay que devolver el foco a una tarjeta concreta, y eso
// estaba escrito como `selection=4` a mano. Si la tabla cambia de orden, ese 4 apuntaria a otra
// tarjeta: exactamente el tipo de desfase que se reporto. Buscando por la ACCION no puede fallar.
static int homeIndexFor(HomeAction action) {
    HomeCard cards[kHomeCardCount];
    buildHomeCards(cards, false, false);   // los textos no importan: solo la accion
    for(int i=0;i<kHomeCardCount;i++) {
        if(cards[i].action==action) return i;
    }
    opennow::LogAppLifecycleEvent("MENU_ACTION_NOT_FOUND",
        ("accion sin tarjeta: "+std::to_string(static_cast<int>(action))).c_str());
    return 0;
}

// =============================================================================================
// ANIMACION DE LA SELECCION DEL CENTRO DE JUEGO
// =============================================================================================
// ESTA AQUI (y no dentro del dibujado como `static` local) POR UN FALLO REAL: al salir de una tarjeta
// el codigo reiniciaba `selection=0`, pero la variable suavizada conservaba el valor anterior. El
// resultado era que el resalte aparecia en una tarjeta distinta de la seleccionada: el "foco atrapado"
// que se reporto desde consola. Con el estado a nivel de fichero, el dispatch puede reiniciarlo.
static float g_homeSelectionSmooth=-1.0f;
static Uint32 g_homeSelectionLastTick=0;

// Reinicia la animacion para que el resalte salte DIRECTAMENTE a la seleccion nueva, sin recorrer las
// tarjetas intermedias. Se llama cuando la seleccion se reinicia a 0 al salir del menu.
static void homeSelectionAnimReset() {
    g_homeSelectionSmooth=-1.0f;
    g_homeSelectionLastTick=0;
}
static std::string activeSessionId;
static std::string activeGameTitle;
static bool g_activeRecoverableSession = false;
static int lastMouseX=0,lastMouseY=0;
static void leaveStream(bool close_cloud_session = true);
static std::chrono::steady_clock::time_point activeStreamStartedAt{};
static bool streamMenuVisible=false;
static bool streamStatsVisible=false;
static bool g_streamExitPromptActive=false;
static std::chrono::steady_clock::time_point g_streamExitPromptUntil{};
static std::chrono::steady_clock::time_point s_forcedExitChordSince{};
static uint64_t s_lastPresentedGeneration = 0;
// Ultimo valor conocido de los contadores de video, para la traza detallada (v4.25). Los actualiza el
// bloque de salud del stream; la fila por frame del CSV los lee. Un valor con hasta 500 ms de
// antiguedad es mas honesto que un cero que parezca una medida.
static int g_traceDecodeQueue = 0;
static int g_traceDecodeFps = 0;
static bool streamSuppressInputUntilNeutral=false;
static Uint32 streamInputSuppressDeadline=0;

// =================================================================================================
// PRIORIDAD DEL HILO PRINCIPAL DURANTE EL STREAM (v3.64) — MITIGACION DEL INPUT LAG
// =================================================================================================
// EL PROBLEMA, ENCONTRADO LEYENDO EL CODIGO DEL DECODIFICADOR:
//
// `FFmpegVideoDecoder::decode()` hace esto **en CADA frame**:
//
//     int policy; sched_param params{};
//     pthread_getschedparam(pthread_self(), &policy, &params);
//     params.sched_priority = sched_get_priority_max(policy);   // <-- PRIORIDAD MAXIMA
//     pthread_setschedparam(pthread_self(), policy, &params);
//
// Es decir: **el decodificador se pone a la prioridad mas alta del sistema**, y lo hace en cada
// llamada. El hilo PRINCIPAL —el que lee el mando, dibuja y presenta— se queda compitiendo desde
// abajo, con la CPU ademas sobresuscrita (5 hilos de decodificador + 6 del escalador + red en 8
// nucleos).
//
// **Eso es el input lag**: el hilo que envia los botones al servidor **tiene que esperar a que el
// decodificador le deje la CPU**, y lo mismo el que presenta el frame. No es que la lectura del mando
// sea lenta (es una llamada al sistema de microsegundos); es que **al hilo le quitan el procesador**.
//
// LA CORRECCION: subir la prioridad del hilo principal mientras hay una partida en curso, para que
// compita en igualdad con el decodificador. Se hace UNA vez al entrar y se restaura al salir —
// **no en cada frame**, porque `setschedparam` es una llamada al sistema y hacerla por frame seria
// justo el tipo de coste que se quiere evitar.
//
// Se guarda la prioridad ORIGINAL para restaurarla: el hilo principal tambien atiende los menus, y
// dejarlo con prioridad alta fuera del stream no aporta nada y podria quitar CPU a la red.
//
// SE USAN LAS FUNCIONES NATIVAS DE PS4 (`scePthreadGetprio`/`scePthreadSetprio`, declaradas en
// `orbis/libkernel.h`, que este fichero ya incluye) en lugar de `pthread_getschedparam`, porque
// `main.cpp` no incluye `pthread.h`. Hacen lo mismo y su firma es directa: devuelven 0 si van bien.
//
// RANGO DE PRIORIDADES en Orbis: en la escala de pthread de PS4 el valor **mas bajo es el MAS
// prioritario** (igual que la escala FIFO de POSIX, donde 0 es la maxima). Por eso la prioridad
// "alta" es un numero PEQUENO y no `sched_get_priority_max()` como en otras plataformas.
// `0x100` es el valor tipico de maxima prioridad de usuario; el decodificador se pone en el maximo
// con su propia llamada, asi que aqui se usa el mismo para competir en igualdad.
static int g_mainThreadPriorityOriginal = -1;   // -1 = no se ha tocado
static bool g_mainThreadPriorityRaised = false;

static void setStreamingThreadPriority(bool streaming) {
#ifdef __ORBIS__
    const OrbisPthread self = pthread_self();
    if(streaming) {
        if(g_mainThreadPriorityRaised) return;
        int32_t original = 0;
        if(scePthreadGetprio(self, &original) == 0) {
            g_mainThreadPriorityOriginal = original;
            // Se sube a la misma prioridad que usa el decodificador para si mismo. No mas alto: la
            // red y el propio decodificador tienen que seguir avanzando.
            if(scePthreadSetprio(self, 0x100) == 0) {
                g_mainThreadPriorityRaised = true;
                char d[176];
                snprintf(d,sizeof(d),
                         "prio=%d -> 0x100 motivo=input_lag_decodificador_se_pone_al_maximo",
                         (int)original);
                opennow::LogAppLifecycleEvent("STREAM_MAIN_THREAD_PRIORITY", d);
            } else {
                opennow::LogAppLifecycleEvent("STREAM_MAIN_THREAD_PRIORITY_FAIL","scePthreadSetprio");
            }
        } else {
            opennow::LogAppLifecycleEvent("STREAM_MAIN_THREAD_PRIORITY_FAIL","scePthreadGetprio");
        }
    } else {
        if(!g_mainThreadPriorityRaised) return;
        if(g_mainThreadPriorityOriginal >= 0) {
            (void)scePthreadSetprio(self, g_mainThreadPriorityOriginal);
        }
        g_mainThreadPriorityRaised = false;
        g_mainThreadPriorityOriginal = -1;
        opennow::LogAppLifecycleEvent("STREAM_MAIN_THREAD_PRIORITY_RESTORED","fuera_de_partida");
    }
#else
    (void)streaming;
#endif
}

static bool settingsRegionPickerVisible=false;
static int settingsRegionSelection=0;
static bool settingsRegionPickerDrawLogged=false;
static int streamMenuSelection=0;
static bool streamMenuChordWasDown=false;
static bool s_menuChordReleasedSinceOpen=false;
static uint16_t s_menuActionsPrevButtons=0;
static int ps4KeyboardHandle=-1;
static int ps4PadHandle=-1;
static OrbisPadData ps4PadState{};
static OrbisPadData ps4PadPrevious{};
static Uint32 lastMouseTraceAt=0;
static bool ps4PadReady=false;
static bool ps4PadButtonLatch[16]{};
static bool ps4PadAxisLatch[4]{};
static std::chrono::steady_clock::time_point testerCircleSince{};
static bool testerCircleHeld=false;
static bool touchpadMouseActive=false,touchpadMousePressed=false;
static uint16_t touchpadLastX=0,touchpadLastY=0;
static int ps4PadOpenRc=-1;
static int traceLastPage=-1;
static int inputAxisCenter[4]={128,128,128,128};
static int inputTriggerRest[2]={0,0};
static bool inputCalibrationReady=false;
static bool inputCalibrationNative=false;
static Uint32 inputCalibrationLastAt=0;
static bool inputCalibrationSampling=false;
static Uint32 inputCalibrationStartedAt=0;
static int inputCalibrationSamples=0;
static int inputCalibrationMin[6]={32767,32767,32767,32767,32767,32767};
static int inputCalibrationMax[6]={-32768,-32768,-32768,-32768,-32768,-32768};
static int64_t inputCalibrationSum[6]={0,0,0,0,0,0};
static Uint32 inputCalibrationStableSince=0;
static bool inputCalibrationHavePrevious=false;
static int inputCalibrationPrevious[6]{};
static int64_t inputCalibrationStableSum[6]={0,0,0,0,0,0};
static int inputCalibrationStableSamples=0;
static char inputCalibrationStatus[112]="PULSA CUADRADO, SUELTA LOS STICKS Y ESPERA";
static bool isSdlPs4Joystick(SDL_Joystick* joystick) {
    if(!joystick) return false;
    const char* name=SDL_JoystickName(joystick);
    return name && (SDL_strcasecmp(name,"Dualshock4")==0 || SDL_strcasecmp(name,"PS4 Controller")==0);
}
static bool readFreshSdlPs4State(SDL_Joystick* joystick,OrbisPadData& state) {
    if(!isSdlPs4Joystick(joystick)) return false;
    // The bundled SDL-PS4 joystick driver can leave a cached axis unchanged
    // when it returns to center. Its joystick instance ID is the scePad handle,
    // so read the current hardware sample directly instead of trusting that cache.
    const SDL_JoystickID instance=SDL_JoystickInstanceID(joystick);
    if(instance<=0) return false;
    OrbisPadData fresh{};
    const int rc=scePadReadState(static_cast<int32_t>(instance),&fresh);
    if(rc<0 || !fresh.connected) {
        static bool failureLogged=false;
        if(!failureLogged) {
            char detail[120]; snprintf(detail,sizeof(detail),"source=scePadReadState instance=%d rc=%d connected=%d",static_cast<int>(instance),rc,fresh.connected?1:0);
            opennow::LogAppLifecycleEvent("GAMEPAD_FRESH_PS4_READ_FAILED",detail);
            failureLogged=true;
        }
        return false;
    }
    state=fresh;
    static bool readyLogged=false;
    if(!readyLogged) {
        char detail[96]; snprintf(detail,sizeof(detail),"source=scePadReadState instance=%d axes=unsigned8 fresh_poll=1",static_cast<int>(instance));
        opennow::LogAppLifecycleEvent("GAMEPAD_FRESH_PS4_READ_READY",detail);
        readyLogged=true;
    }
    static int previous[6]={-1,-1,-1,-1,-1,-1};
    static Uint32 lastSampleLog=0;
    const int current[6]={fresh.leftStick.x,fresh.leftStick.y,fresh.rightStick.x,fresh.rightStick.y,fresh.analogButtons.l2,fresh.analogButtons.r2};
    bool changed=false;
    for(int i=0;i<6;i++) if(previous[i]<0 || std::abs(current[i]-previous[i])>=2) changed=true;
    const Uint32 now=SDL_GetTicks();
    if(changed && static_cast<Uint32>(now-lastSampleLog)>=120u) {
        char detail[160]; snprintf(detail,sizeof(detail),"sticks_u8=%d,%d,%d,%d triggers_u8=%d,%d buttons=0x%08X",
            current[0],current[1],current[2],current[3],current[4],current[5],fresh.buttons);
        opennow::LogAppLifecycleEvent("GAMEPAD_FRESH_PS4_SAMPLE",detail);
        for(int i=0;i<6;i++) previous[i]=current[i];
        lastSampleLog=now;
    }
    return true;
}
static bool isSdlPs4Instance(SDL_JoystickID instance) {
    for(SDL_Joystick* joystick:genericJoysticks)
        if(joystick && SDL_JoystickInstanceID(joystick)==instance && isSdlPs4Joystick(joystick)) return true;
    return false;
}

static void suppressStreamInputBriefly() {
    streamSuppressInputUntilNeutral=true;
    streamInputSuppressDeadline=SDL_GetTicks()+450;
}

static void releaseStaleInputSuppression() {
    if(streamSuppressInputUntilNeutral &&
       static_cast<Sint32>(SDL_GetTicks()-streamInputSuppressDeadline)>=0) {
        streamSuppressInputUntilNeutral=false;
        opennow::LogAppLifecycleEvent("STREAM_INPUT_SUPPRESSION_RELEASED","cause=timeout maximum_ms=450");
    }
}

static void traceSdlAction(const SDL_Event& ev,int currentPage) {
    char detail[112];
    switch(ev.type) {
    case SDL_QUIT: opennow::TraceAppAction("SDL_EVENT","quit"); break;
    case SDL_KEYDOWN:
        if(currentPage==4) opennow::TraceAppAction("SDL_INPUT","key_down context=login; key omitted");
        else { snprintf(detail,sizeof(detail),"key_down page=%d sym=%d scancode=%d repeat=%d",currentPage,
                        static_cast<int>(ev.key.keysym.sym),static_cast<int>(ev.key.keysym.scancode),ev.key.repeat);
               opennow::TraceAppAction("SDL_INPUT",detail); }
        break;
    case SDL_KEYUP:
        if(currentPage==4) opennow::TraceAppAction("SDL_INPUT","key_up context=login; key omitted");
        else { snprintf(detail,sizeof(detail),"key_up page=%d sym=%d",currentPage,static_cast<int>(ev.key.keysym.sym));
               opennow::TraceAppAction("SDL_INPUT",detail); }
        break;
    case SDL_TEXTINPUT:
        snprintf(detail,sizeof(detail),"text_input page=%d length=%u content=omitted",currentPage,
                 static_cast<unsigned>(strlen(ev.text.text)));
        opennow::TraceAppAction("SDL_INPUT",detail);
        break;
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
        snprintf(detail,sizeof(detail),"controller_button %s page=%d button=%u",
                 ev.type==SDL_CONTROLLERBUTTONDOWN?"down":"up",currentPage,ev.cbutton.button);
        opennow::TraceAppAction("SDL_INPUT",detail); break;
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
        snprintf(detail,sizeof(detail),"joystick_button %s page=%d button=%u",
                 ev.type==SDL_JOYBUTTONDOWN?"down":"up",currentPage,ev.jbutton.button);
        opennow::TraceAppAction("SDL_INPUT",detail); break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        snprintf(detail,sizeof(detail),"mouse_button %s page=%d button=%u x=%d y=%d",
                 ev.type==SDL_MOUSEBUTTONDOWN?"down":"up",currentPage,ev.button.button,ev.button.x,ev.button.y);
        opennow::TraceAppAction("SDL_INPUT",detail); break;
    case SDL_MOUSEMOTION: {
        const Uint32 now=SDL_GetTicks();
        if(static_cast<Uint32>(now-lastMouseTraceAt)>=100u && (ev.motion.xrel||ev.motion.yrel)) {
            lastMouseTraceAt=now;
            snprintf(detail,sizeof(detail),"mouse_motion page=%d dx=%d dy=%d sampled_ms=100",
                     currentPage,ev.motion.xrel,ev.motion.yrel);
            opennow::TraceAppAction("SDL_INPUT",detail);
        }
        break;
    }
    case SDL_JOYHATMOTION:
        snprintf(detail,sizeof(detail),"joystick_hat page=%d value=%u",currentPage,ev.jhat.value);
        opennow::TraceAppAction("SDL_INPUT",detail); break;
    default: break;
    }
}
static std::chrono::steady_clock::time_point lastNavAt{},lastHorizontalAt{};
static std::string launchCoverUrl;
// CloudMatch JSON and the WebRTC/DTLS constructors are unusually deep on PS4.
// SDL's platform default is too small for these C++ worker call chains and can
// turn a normal game launch into an app freeze/crash before the UI can report it.
static constexpr size_t kNetworkWorkerStackSize = 4u * 1024u * 1024u;
static void suppressStreamInputBriefly();
static bool consumeDisclaimerEvent(const SDL_Event& ev);
static void activateStreamMenu();
static int ps4KeyboardInitRc=-1;
static int ps4KeyboardOpenRc=-1;
static bool keyboardActivityLatched=false;
static bool mapSdlKey(SDL_Keycode key,Uint16 modifiers,opennow::input::KeyboardStroke& stroke);
static void eraseCatalogSearchChar();
static void toggleCatalogSearch();
static void editCatalogSearch(const char* input);
static std::unordered_map<std::string,SDL_Texture*> textCache;

// =================================================================================================
// CONTADORES DE PRIMITIVAS POR FRAME (v3.69)
// =================================================================================================
// POR QUE HACEN FALTA: `UI_DRAW_PHASES` da el TIEMPO de cada fase, pero no su CAUSA. Saber que una
// pantalla gasta 12 ms en el contenido no dice si eso son 4 rectangulos o 60 etiquetas de texto, y
// **cada primitiva del renderizador software es una copia de memoria en la CPU**: sin el recuento, la
// unica forma de optimizar seria probar cambios a ciegas (que es justo el error que se corrigio en la
// v3.49/v3.53/v3.57).
//
// Se cuentan las dos familias que dominan:
//   - `g_frameFills` : rectangulos rellenos (`fill`/`fillAlpha` -> SDL_RenderFillRect)
//   - `g_frameLabels`: etiquetas de texto (`label` -> copia de la textura del glifo)
//
// El coste de incrementar un entero por primitiva es despreciable frente a la copia que esa primitiva
// ya hace, asi que la instrumentacion no falsea la medida que pretende explicar.
static uint32_t g_frameFills  = 0;
static uint32_t g_frameLabels = 0;
static uint32_t g_frameRects  = 0;   // total de llamadas a fill/fillAlpha (las "cajas" dibujadas)

static uint64_t getProcessTimeUs() {
#ifdef __ORBIS__
    return sceKernelGetProcessTime();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

static void releaseSdlForVideoOut() {
    opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SDL_RELEASE_BEGIN");
    const uint64_t t0 = getProcessTimeUs();

    // The DCE keeps flips queued after sceVideoOutSubmitFlip() returns.
    // Destroying the SDL VideoOut owner while that queue is draining can leave
    // the app with no visible output (black screen). Wait for a proven-empty
    // queue while our own handle is still valid, instead of guessing.
    if(g_videoOutHandoffDone && videoOutDirectActive) {
        opennow::PS4VideoOutRenderer::WaitForFlipsSettled(250, "handoff_sdl_release");
    }

    // =================================================================================================
    // LIBERAR EL LIENZO PROPIO (v3.93). ESTA FUGA ES REAL Y ESTABA SIN CORREGIR.
    // =================================================================================================
    // POR QUE HACE FALTA AQUI:
    //
    // El lienzo propio (`g_ownCanvas`, `ARGB8888` de 1920x1080 = **8,29 MB**) se crea al arrancar y en
    // `restoreSdlFromVideoOut()`. **No habia ni una sola llamada a `SDL_FreeSurface` sobre el en todo el
    // fichero.** Y el traspaso SDL -> VideoOut hacia:
    //
    //     1. `releaseSdlForVideoOut()`  -> destruye renderer y window, **deja `g_ownCanvas` vivo**
    //     2. `restoreSdlFromVideoOut()` -> `g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(...)`
    //                                       **sobrescribe el puntero SIN liberar el anterior**
    //
    // Resultado: **8,29 MB perdidos en cada ciclo de stream** (entrar a jugar, volver, volver a entrar).
    // `SDL_CreateSoftwareRenderer` NO toma propiedad de la superficie que recibe, asi que el renderizador
    // tampoco la liberaba al destruirse.
    //
    // Y la memoria agotada es exactamente la hipotesis documentada del cierre sin traza
    // (`PS4-V2.81-CRASH-HEARTBEAT.md`: *"el kernel lo mata (memoria agotada, watchdog del sistema)"*,
    // con degradacion progresiva antes de morir). La sesion de la v3.90 murio tras 300 s.
    //
    // Se libera aqui, ANTES de destruir el renderizador (que ya no la necesita), para cortar la fuga.
    if(g_ownCanvas) {
        SDL_FreeSurface(g_ownCanvas);
        g_ownCanvas = nullptr;
        opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_FREED", "motivo=traspaso_a_videoout");
    }

    if(renderer) {
        const uint64_t t_rend0 = getProcessTimeUs();
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
        // PUNTERO COLGANTE EVITADO (v3.94). `SDLVideoRenderer` guarda el renderizador en un `static
        // target_` que se fija con `SetRenderTarget()`. Si aqui se destruye el renderizador y `target_`
        // sigue apuntando a el, cualquier llamada posterior a la ruta SDL de video (por ejemplo
        // `activeStream->draw()` en una version futura que olvidara el `return` del modo directo)
        // escribiria sobre memoria liberada. Hoy NO ocurre —el camino directo hace `return` antes— pero
        // el coste de blindarlo es cero y el fallo seria un cierre sin traza, que es justo lo que hay
        // que evitar. Se limpia el destino Y el lienzo de video.
        SDLVideoRenderer::SetRenderTarget(nullptr);
        SDLVideoRenderer::SetRenderCanvas(nullptr);
        const uint64_t rend_us = getProcessTimeUs() - t_rend0;
        char rendDetail[64];
        std::snprintf(rendDetail, sizeof(rendDetail), "us=%llu", static_cast<unsigned long long>(rend_us));
        opennow::LogAppLifecycleEvent("VIDEOOUT_SDL_DESTROY_RENDERER_US", rendDetail);
    }
    if(window) {
        const uint64_t t_win0 = getProcessTimeUs();
        SDL_DestroyWindow(window);
        window = nullptr;
        const uint64_t win_us = getProcessTimeUs() - t_win0;
        char winDetail[64];
        std::snprintf(winDetail, sizeof(winDetail), "us=%llu", static_cast<unsigned long long>(win_us));
        opennow::LogAppLifecycleEvent("VIDEOOUT_SDL_DESTROY_WINDOW_US", winDetail);
    }
    if(SDL_WasInit(SDL_INIT_VIDEO)) {
        const uint64_t t_quit0 = getProcessTimeUs();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        const uint64_t quit_us = getProcessTimeUs() - t_quit0;
        char quitDetail[64];
        std::snprintf(quitDetail, sizeof(quitDetail), "us=%llu", static_cast<unsigned long long>(quit_us));
        opennow::LogAppLifecycleEvent("VIDEOOUT_SDL_QUIT_VIDEO_US", quitDetail);
    }
    rendererUsesWindowSurface = false;
    rendererHardwareAccelerated = false;

    const uint64_t elapsed_us = getProcessTimeUs() - t0;
    char detail[64];
    std::snprintf(detail, sizeof(detail), "elapsed_us=%llu", static_cast<unsigned long long>(elapsed_us));
    opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SDL_RELEASE_OK", detail);
}

static bool restoreSdlFromVideoOut() {
    opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SDL_RESTORE_BEGIN");
    const uint64_t t0 = getProcessTimeUs();

    // Stage telemetry. Both field logs ended right after the window was
    // recreated, so each remaining step is logged BEFORE it runs: whatever the
    // log stops on is the step that blocks.
    auto stage = [&](const char* name) {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "stage=%s elapsed_us=%llu",
                      name, static_cast<unsigned long long>(getProcessTimeUs() - t0));
        opennow::LogAppLifecycleEvent("VIDEOOUT_SDL_RESTORE_STAGE", detail);
    };

#if defined(__ORBIS__)
    // Wait for a proven-empty flip queue instead of a fixed 20 ms guess: a
    // blind sleep can recreate the window while the DCE still owns scanout.
    opennow::PS4VideoOutRenderer::WaitForFlipsSettled(250, "handoff_sdl_restore");
#endif
    stage("flips_drained");

    if(!SDL_WasInit(SDL_INIT_VIDEO)) {
        stage("sdl_video_init_begin");
        if(SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SDL_RESTORE_FAIL", SDL_GetError());
            return false;
        }
        stage("sdl_video_init_done");
    }

    stage("window_create_begin");
    window = SDL_CreateWindow("AJ - GeForce NOW PS4", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, 0);
    if(!window) {
        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SDL_RESTORE_FAIL", "stage=window");
        return false;
    }
    stage("window_create_done");

    SDL_ClearError();
    // =================================================================================================
    // SE RECREA EL LIENZO PROPIO (v3.93). ANTES ESTA RUTA VOLVIA AL LIENZO DE LA VENTANA.
    // =================================================================================================
    // Lo que hacia antes: `SDL_GetWindowSurface(window)` + `SDL_CreateSoftwareRenderer(surface)`, es
    // decir **volvia a dibujar en la superficie de la VENTANA**, que el driver declara `BGR888`
    // (3 bytes). Con la textura del video en `ARGB8888`, **los formatos no coinciden** y SDL cae a
    // `SDL_LowerBlit` (conversion por pixel), que es lo que costo 25 ms por frame en la v3.90.
    //
    // Ademas `SetRenderCanvas(g_ownCanvas)` se llamaba con `g_ownCanvas` **nulo** (lo acaba de liberar
    // `releaseSdlForVideoOut`), asi que el escalador propio quedaba desactivado sin avisar.
    //
    // Consecuencia real: **terminar un stream y entrar a otro dejaba la app en el camino lento**, en
    // silencio. Ahora se recrea el lienzo propio igual que en el arranque, para que las dos rutas sean
    // identicas.
    if(g_ownCanvas) {                 // defensivo: no debe quedar ninguno, pero no se filtra memoria
        SDL_FreeSurface(g_ownCanvas);
        g_ownCanvas = nullptr;
    }
    {
        // =================================================================================================
        // POR QUE EL LIENZO MIDE EL DISPLAY Y NO EL STREAM (v4.27) — ESTO EXPLICA EL TECHO DE LA RUTA SDL
        // =================================================================================================
        // `W` y `H` son **el tamano del display** (1920x1080), no el del stream. Y eso **no es un
        // descuido: es obligatorio en esta ruta**, por la cadena del driver:
        //
        //   1. El driver de SDL-PS4 copia su superficie de VENTANA a los buffers de VideoOut con un
        //      **memcpy crudo de 32 bits** (`SDL_ps4video.c:519`), **sin escalar**.
        //   2. Y registra esos buffers **al tamano del display**, no al de la ventana.
        //   3. Por tanto, si el lienzo midiera 1280x720, la imagen saldria **recortada** a la esquina
        //      superior izquierda, no estirada.
        //
        // **CONSECUENCIA, y es la clave de todo el problema del rendimiento:** en la ruta SDL, un stream
        // de 720p **se tiene que escalar a 1920x1080 EN LA CPU** para llenar la pantalla. Ese escalado
        // cuesta ~31.000 us medidos (mas 45.000 del blit en su version lenta), sobre un presupuesto de
        // 16.666: **la ruta SDL no puede pasar de ~20 fps, y no es optimizable.**
        //
        // LA RUTA DIRECTA ES DISTINTA Y POR ESO DA 60: registra su propio framebuffer a **1280x720**
        // (el tamano del stream), convierte **1:1 sin escalar**, y **deja que VideoOut estire ese
        // framebuffer al panel**: comprobado en consola, el 720p llena la pantalla. El escalado lo hace
        // el hardware en vez de la CPU.
        //
        // Dicho de otro modo: **la misma imagen se puede producir escalando en la CPU (SDL, 20 fps) o
        // escalando en el hardware de VideoOut (directa, 60 fps).** La eleccion no es de estilo.
        const int cw = W>0?W:1920, ch = H>0?H:1080;
        // v4.06: el formato del lienzo tiene que COMPENSAR la cadena del driver. Ver el bloque largo de
        // `GFN_CANVAS_PIXELFORMAT`: el driver envia [R][G][B] y VideoOut lo lee como [B][G][R], asi que el
        // lienzo tiene que estar en el formato cuyo byte bajo es R. Con respaldo: si el driver no
        // soportara ese formato, se cae al de antes en vez de quedarse sin lienzo.
        g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, GFN_CANVAS_PIXELFORMAT);
        if(!g_ownCanvas) {
            opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_FORMAT_FALLBACK",
                "formato_pedido=SDL_PIXELFORMAT_BGR888 motivo=no_soportado usando=ARGB8888");
            g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, SDL_PIXELFORMAT_ARGB8888); // RESPALDO_DECLARADO
        }
    }
    SDL_Surface* surface = g_ownCanvas;
    if(!surface) {
        opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_FAILED", SDL_GetError());
        surface = SDL_GetWindowSurface(window);
    } else {
        char cb[176];
        std::snprintf(cb,sizeof(cb),
                      "canvas=%dx%d fmt=%s pitch=%d (propio, en RAM) "
                      "motivo=restauracion_tras_videoout",
                      g_ownCanvas->w, g_ownCanvas->h,
                      SDL_GetPixelFormatName(g_ownCanvas->format->format), g_ownCanvas->pitch);
        opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_ON", cb);
    }
    stage("window_surface_ok");
    renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    applyLogicalSize();
    rendererUsesWindowSurface = (renderer != nullptr);
    rendererHardwareAccelerated = false;

    if(!renderer) {
        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SDL_RESTORE_FAIL", "stage=renderer");
        return false;
    }
    stage("software_renderer_ok");

    SDLVideoRenderer::SetRenderTarget(renderer);
SDLVideoRenderer::SetRenderWindow(window);
SDLVideoRenderer::SetRenderCanvas(g_ownCanvas);   // el escalador escribe en el LIENZO PROPIO
    if(hasPublishedStream()) {
        activeStream->switch_to_sdl_video_renderer();
    }
    stage("video_renderer_retargeted");

    char winDetail[112];
    std::snprintf(winDetail, sizeof(winDetail), "width=%d height=%d format=SDL_SOFTWARE", W, H);
    opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SDL_WINDOW_RECREATED", winDetail);

    // Invalidate cached text textures because the SDL_Renderer handle changed
    for(auto& item : textCache) {
        if(item.second) SDL_DestroyTexture(item.second);
    }
    textCache.clear();
    stage("text_cache_cleared");

    // Do NOT touch the renderer here.
    //
    // Three field logs ended at exactly this point, and after removing the manual
    // SDL_UpdateWindowSurface() call the hang persisted identically - so that was
    // not the culprit. The frame markers (frame_clear_begin / frame_present_begin)
    // never appear either, which means the blocking call is inside SDL_RenderClear
    // or SDL_RenderPresent on the freshly re-created software renderer.
    //
    // Instead of guessing again, hand control straight back to the main loop: it
    // calls draw() -> presentFrame() on its very next iteration and that path is
    // already proven to work (it is the same one used before any handoff). If the
    // hang reappears there, the new stage markers will place it in the main loop
    // rather than in this restore routine.
    opennow::LogAppLifecycleEvent("VIDEOOUT_SDL_RESTORE_DEFERRED_PRESENT",
                                  "reason=renderer_present_hangs; owner=main_loop");

    const uint64_t elapsed_us = getProcessTimeUs() - t0;
    char uiDetail[128];
    std::snprintf(uiDetail, sizeof(uiDetail), "page=%d catalog_scroll=%d textures_reloaded=1 elapsed_us=%llu",
                  page, catalogOffset, static_cast<unsigned long long>(elapsed_us));
    opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_UI_RESTORED", uiDetail);
    return true;
}

// =====================================================================================================
// RECUPERACION DE LA RUTA DIRECTA, LLAMABLE DESDE FUERA DEL CALLBACK (v4.31)
// =====================================================================================================
// POR QUE SE EXTRAE A UNA FUNCION:
//
//   El camino de recuperacion ya existia y es correcto: apaga VideoOut, marca la cuarentena y restaura
//   SDL. **Pero vivia DENTRO del callback del frame** (`if(NeedsFullRecreate())`, en la linea ~6009).
//
//   Y el fallo que hay que recuperar es, precisamente, **que la presentacion se pare** — que es lo mismo
//   que decir que ese callback puede no estar ejecutandose. **Una recuperacion que solo corre dentro del
//   callback no sirve para el caso en que el callback no corre.**
//
//   Por eso se extrae aqui: la llaman **los dos**, el callback (por fallo persistente de flips) y el
//   vigilante del bucle principal (por presentacion que deja de avanzar). Mismo cuerpo, dos disparadores.
//
// Alcance de la cuarentena: **el resto de la ejecucion**. Un reinicio de la app vuelve a intentar la ruta
// directa, que es lo razonable: el fallo pudo ser de esa sesion concreta del servidor.
//
// =====================================================================================================
// ESTA FUNCION LLAMA A `Shutdown()`, Y ESO OBLIGA A UNA GARANTIA EXPLICITA (v4.31). LEER ANTES DE TOCAR.
// =====================================================================================================
// El proyecto tiene un invariante escrito: **`Shutdown()` no puede ejecutarse mientras `Present()` podria
// estar escribiendo en la memoria directa**, porque no hay exclusion mutua: solo la marca `s_shutting_down`
// dentro de `Present()`. Ese invariante se cumplia **por construccion**, porque los dos vivian en el mismo
// callback y `Present()` retorna antes.
//
// **Al extraer esta funcion, `Shutdown()` puede ahora llamarse desde DOS sitios**: el callback (fallo
// persistente de flips) y el bucle principal (vigilante). El del bucle principal **no esta dentro del
// callback**, asi que hay que demostrar que no puede coincidir con un `Present()` en curso. Se demuestra
// con el candado de abajo: **`Present()` y esta funcion se excluyen mutuamente.**
//
// El candado es de un solo sentido a proposito: `Present()` NO lo coge (es el camino caliente y no debe
// pagar un candado), pero **siempre se ejecuta con el candado libre** porque los dos unicos sitios que lo
// cogen son este y el propio vigilante, que corre en el mismo hilo principal. Y el hilo principal es el
// unico que llama a `Present()`.
static std::mutex g_directPathRecoveryMutex;

static void recoverFromDirectPathFailure(const char* reason, bool desdeElCallback)
{
    // Si venimos del bucle principal, se coge el candado para dejar constancia de la exclusion y para que
    // un futuro cambio que moviera `Present()` a otro hilo se note aqui (habria que cogerlo tambien alli).
    // Si venimos del callback, **no se puede coger**: ya estamos dentro de la secuencia que lo protege, y
    // cogerlo ahi seria un bloqueo innecesario en el camino caliente.
    std::unique_lock<std::mutex> guard(g_directPathRecoveryMutex, std::defer_lock);
    if (!desdeElCallback) {
        guard.lock();
        // Se deja escrito en el log que la recuperacion NO viene del callback, que es la situacion nueva:
        // sirve para saber en consola por cual de los dos disparadores se degrado la ruta.
        opennow::LogAppLifecycleEvent("VIDEOOUT_RECOVERY_FROM_MAIN_LOOP",
                                      "fuera_del_callback=1 exclusion_con_present=1");
    }
    opennow::LogAppLifecycleEvent("VIDEOOUT_RECOVERY_FULL_RECREATE", reason ? reason : "reason=?");
    opennow::PS4VideoOutRenderer::ClearFullRecreate();
    opennow::PS4VideoOutRenderer::Shutdown();
    g_videoOutHandoffDone = false;
    videoOutDirectActive = false;   // degradacion automatica a SDL
    g_videoOutQuarantined = true;
    opennow::LogAppLifecycleEvent("VIDEOOUT_QUARANTINED",
        "reason=presentacion_detenida alcance=resto_de_la_ejecucion "
        "siguiente_stream=SDL reinicio_app=reintenta_directo");
    if(restoreSdlFromVideoOut()) {
        opennow::LogAppLifecycleEvent("VIDEOOUT_RECOVERY_SDL_READY", "renderer=restored");
    } else {
        opennow::LogAppLifecycleEvent("VIDEOOUT_RECOVERY_SDL_FAILED", "stage=sdl_restore");
    }
}

static bool fallbackFromPiglet(const char* reason) {
    opennow::LogAppLifecycleEvent("PIGLET_FALLBACK_SDL",reason);
    opennow::LogAppLifecycleEvent("PIGLET_RUNTIME_FALLBACK_BEGIN",reason);
    if(renderer) { SDL_DestroyRenderer(renderer); renderer=nullptr; }
    if(pigletOverlaySurface) { SDL_FreeSurface(pigletOverlaySurface); pigletOverlaySurface=nullptr; }
    PS4PigletVideoRenderer::Shutdown();
    pigletVideoActive=false;
    pigletStaticStreamOverlayReady=false;
    if(!SDL_WasInit(SDL_INIT_VIDEO)) {
        if(SDL_InitSubSystem(SDL_INIT_VIDEO)!=0) {
            opennow::LogAppLifecycleEvent("PIGLET_FALLBACK_SDL_VIDEO_FAIL",SDL_GetError());
        }
    }
    window=SDL_CreateWindow("AJ - GeForce NOW PS4",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,W,H,0);
    if(!window) {
        opennow::LogAppLifecycleEvent("PIGLET_RUNTIME_FALLBACK_FAILED","stage=window");
        return false;
    }
    SDL_ClearError();
    SDL_Surface* surface=SDL_GetWindowSurface(window);
    renderer=surface?SDL_CreateSoftwareRenderer(surface):nullptr;
    applyLogicalSize();
    rendererUsesWindowSurface=renderer!=nullptr;
    rendererHardwareAccelerated=false;
    if(renderer) opennow::LogAppLifecycleEvent("PIGLET_RUNTIME_FALLBACK_SOFTWARE",
        "renderer=SDL_window_surface; present=explicit_videoout_flip");
    if(!renderer) {
        opennow::LogAppLifecycleEvent("PIGLET_RUNTIME_FALLBACK_FAILED","stage=renderer");
        return false;
    }
    SDLVideoRenderer::SetRenderTarget(renderer);
SDLVideoRenderer::SetRenderWindow(window);
SDLVideoRenderer::SetRenderCanvas(g_ownCanvas);   // el escalador escribe en el LIENZO PROPIO
    if(hasPublishedStream()) activeStream->switch_to_sdl_video_renderer();
    opennow::LogAppLifecycleEvent("PIGLET_RUNTIME_FALLBACK_READY",
        rendererHardwareAccelerated?"renderer=SDL_ACCELERATED":"renderer=SDL_SOFTWARE");
    return true;
}
static void presentFrame() {
    // Etapa 6 del latido: si la app muere presentando, last_stage.txt lo dira.
    opennow::SetCurrentStage(6 /* STAGE_PRESENT */);
    if(pigletVideoActive) {
        // SDL renders the UI into an off-screen surface in this mode. Flush
        // its queued software drawing, but let Piglet/EGL own the only visible
        // surface and perform the single actual swap below.
        SDL_RenderFlush(renderer);
        static int previousPage=-1;
        static bool previousStats=false,previousMenu=false,previousTerminal=false,streamOverlayReady=false;
        const bool streaming=page==5 && hasPublishedStream();
        const bool terminal=streaming && activeStream->is_terminal();
        const bool overlayChanged=page!=previousPage || streamStatsVisible!=previousStats ||
            streamMenuVisible!=previousMenu || terminal!=previousTerminal ||
            !streaming || streamStatsVisible || streamMenuVisible || terminal || !streamOverlayReady;
        const bool shown=streaming && !terminal;
        if(PS4PigletVideoRenderer::Present(pigletOverlaySurface,shown,overlayChanged)) {
            if(streaming && !streamStatsVisible && !streamMenuVisible && !terminal) {
                streamOverlayReady=true;
                pigletStaticStreamOverlayReady=true;
            } else {
                streamOverlayReady=false;
                pigletStaticStreamOverlayReady=false;
            }
        } else {
            pigletStaticStreamOverlayReady=false;
            (void)fallbackFromPiglet("stage=egl_present_or_gles");
        }
        previousPage=page; previousStats=streamStatsVisible; previousMenu=streamMenuVisible; previousTerminal=terminal;
    } else {
        // =============================================================================================
        // MEDICION DEL PRESENT, PASO A PASO (v3.89)
        // =============================================================================================
        // Hasta aqui se ha medido TODO menos esto, y las cuentas no cuadran:
        //
        //     UI_LOOP_BUDGET  iter_ms=50  draw_ms=50  sleep_ms=0
        //     UI_DRAW_PHASES  present_us/frames = 2,49 ms
        //     STREAM_VIDEO_DRAW_US  video_draw_us/frames = 1,62 ms
        //     STREAM_VIDEO_PHASES   copy_us/frames = 3,00 ms
        //
        // Y la referencia de memoria dice que copiar 8,3 MB son ~1,3 ms en un PC, asi que **ni el blit
        // ni la copia del lienzo pueden costar 50 ms**. Queda UN tramo sin instrumentar: el present, que
        // son tres cosas distintas y de coste muy distinto:
        //
        //     t_present_sdl  : `SDL_RenderPresent` (con el renderer sobre un lienzo propio, su ventana es
        //                      NULL, asi que solo vacia la cola de comandos de software)
        //     t_copy         : la copia del lienzo propio a la superficie de la ventana
        //     t_update       : `SDL_UpdateWindowSurface`, que es donde el DRIVER hace su
        //                      `memset(BufferSize)` + `memcpy(window->w x window->h)` a VideoOut **y
        //                      espera al flip**
        //
        // Con estas tres cifras se sabra cual de los tres se lleva el frame. Se registran una vez por
        // segundo en `SDL_PRESENT_STAGES`.
        const uint64_t t_present_sdl0 = getProcessTimeUs();
        SDL_RenderPresent(renderer);
        const uint64_t t_present_sdl1 = getProcessTimeUs();
        uint64_t t_copy0 = t_present_sdl1, t_copy1 = t_present_sdl1;
        // SDL_CreateSoftwareRenderer(surface) targets an off-screen surface and
        // has no SDL_Window attached. On SDL-PS4, the actual VideoOut flip is
        // performed by SDL_UpdateWindowSurface(), so RenderPresent alone can
        // successfully draw while the TV remains black.
        if(rendererUsesWindowSurface && window) {
            static bool updateFailureLogged=false;
            // =========================================================================================
            // COPIA DEL LIENZO PROPIO A LA SUPERFICIE DE LA VENTANA (v3.84)
            // =========================================================================================
            // Con el lienzo propio, el renderizador dibuja en memoria normal y el driver copia a
            // VideoOut desde la superficie de la VENTANA. **Son dos memorias distintas a proposito**:
            // es lo que elimina el parpadeo de la v3.83 (alli se escribia el video en la misma memoria
            // que el driver estaba copiando).
            //
            // La copia se hace **fila a fila respetando el paso de cada superficie**, sin asumir que
            // coinciden: el lienzo propio es `ARGB8888` (4 bytes) y el de la ventana lo declara el
            // driver de otra forma. Se copian `min(fila, ancho*4)` bytes por fila.
            if(g_ownCanvas && g_ownCanvas->pixels) {
                SDL_Surface* winSurf = SDL_GetWindowSurface(window);
                if(winSurf && winSurf->pixels) {
                    const int rowBytes = g_ownCanvas->w * 4;
                    const int rows = std::min(g_ownCanvas->h, winSurf->h);
                    const int srcPitch = g_ownCanvas->pitch;
                    const int dstPitch2 = winSurf->pitch;
                    const int copyBytes = std::min(rowBytes, std::min(srcPitch, dstPitch2));
                    // =============================================================================
                    // MEDICION DE LA COPIA AL FRAMEBUFFER (v3.88)
                    // =============================================================================
                    // EL COSTE POR FRAME DE ESTE PUNTO ES LA CLAVE DE LOS 20 FPS, y tiene dos partes
                    // que conviene separar:
                    //
                    //   1. Esta copia: `fila_copiada x filas` bytes del lienzo propio a la superficie de
                    //      la ventana.
                    //   2. La que hace el DRIVER al presentar (`SDL_ps4video.c:504-519`): un
                    //      `memset(BufferSize)` **mas** un `memcpy(drawW x drawH)` a la memoria de
                    //      VideoOut. **Y el driver IGNORA los rectangulos**: copia siempre
                    //      `window->w x window->h`.
                    //
                    // Por eso aqui se registran los tamanios REALES (ventana, lienzo, pasos) y el
                    // tiempo de esta copia. Con `ventana=WxH` se sabe cuantos bytes mueve el driver por
                    // frame, que es la magnitud que explica los fps.
                    const uint64_t t_cpy0 = getProcessTimeUs();
                    for(int y = 0; y < rows; ++y) {
                        SDL_memcpy(static_cast<Uint8*>(winSurf->pixels) + static_cast<size_t>(y)*dstPitch2,
                                   static_cast<const Uint8*>(g_ownCanvas->pixels) + static_cast<size_t>(y)*srcPitch,
                                   static_cast<size_t>(copyBytes));
                    }
                    const uint64_t t_cpy1 = getProcessTimeUs();
                    t_copy0 = t_cpy0;
                    t_copy1 = t_cpy1;
                    {
                        static uint64_t s_cpySum=0, s_cpyFrames=0, s_cpyLastLog=0;
                        s_cpySum += (t_cpy1 > t_cpy0) ? (t_cpy1 - t_cpy0) : 0;
                        ++s_cpyFrames;
                        if(s_cpyLastLog == 0 || (t_cpy1 - s_cpyLastLog) >= 1000000ULL) {
                            s_cpyLastLog = t_cpy1;
                            const uint64_t n2 = s_cpyFrames ? s_cpyFrames : 1;
                            int winW=0, winH=0;
                            SDL_GetWindowSize(window,&winW,&winH);
                            char cp2[288];
                            std::snprintf(cp2,sizeof(cp2),
                                          "copia_us=%llu frames=%llu lienzo=%dx%d ventana=%dx%d "
                                          "fila=%d filas=%d bytes_frame=%llu driver_memcpy_bytes=%llu",
                                          (unsigned long long)(s_cpySum/n2),
                                          (unsigned long long)s_cpyFrames,
                                          g_ownCanvas->w, g_ownCanvas->h, winW, winH,
                                          copyBytes, rows,
                                          (unsigned long long)((uint64_t)copyBytes*(uint64_t)rows),
                                          (unsigned long long)((uint64_t)winW*(uint64_t)winH*4u));
                            opennow::LogAppLifecycleEvent("SDL_CANVAS_COPY", cp2);
                            s_cpySum=0; s_cpyFrames=0;
                        }
                    }
                    static bool canvasPitchLogged=false;
                    if(!canvasPitchLogged) {
                        canvasPitchLogged=true;
                        char cp[160];
                        std::snprintf(cp,sizeof(cp),
                                      "canvas_pitch=%d ventana_pitch=%d ancho=%d fila_copiada=%d",
                                      srcPitch, dstPitch2, g_ownCanvas->w, copyBytes);
                        opennow::LogAppLifecycleEvent("SDL_CANVAS_COPY_ONCE", cp);
                    }
                }
            }
            const uint64_t t_update0 = getProcessTimeUs();
            if(SDL_UpdateWindowSurface(window)!=0 && !updateFailureLogged) {
                updateFailureLogged=true;
                opennow::LogAppLifecycleEvent("SDL_VIDEOOUT_PRESENT_FAILED",SDL_GetError());
            }
            const uint64_t t_update1 = getProcessTimeUs();
            // Informe de las tres etapas del present, una linea por segundo.
            {
                static uint64_t s_pSum=0, s_cSum=0, s_uSum=0, s_pFrames=0, s_pLastLog=0;
                s_pSum += (t_present_sdl1 > t_present_sdl0) ? (t_present_sdl1 - t_present_sdl0) : 0;
                s_cSum += (t_copy1 > t_copy0) ? (t_copy1 - t_copy0) : 0;
                s_uSum += (t_update1 > t_update0) ? (t_update1 - t_update0) : 0;
                ++s_pFrames;
                if(s_pLastLog == 0 || (t_update1 - s_pLastLog) >= 1000000ULL) {
                    s_pLastLog = t_update1;
                    const uint64_t n3 = s_pFrames ? s_pFrames : 1;
                    char ps[224];
                    std::snprintf(ps,sizeof(ps),
                                  "page=%d stream=%d renderpresent_us=%llu copia_lienzo_us=%llu "
                                  "updatewindowsurface_us=%llu suma_us=%llu frames=%llu",
                                  page, hasPublishedStream()?1:0,
                                  (unsigned long long)(s_pSum/n3),
                                  (unsigned long long)(s_cSum/n3),
                                  (unsigned long long)(s_uSum/n3),
                                  (unsigned long long)((s_pSum+s_cSum+s_uSum)/n3),
                                  (unsigned long long)s_pFrames);
                    opennow::LogAppLifecycleEvent("SDL_PRESENT_STAGES", ps);
                    s_pSum=0; s_cSum=0; s_uSum=0; s_pFrames=0;
                }
            }
        }
    }
}
static void logLaunchDrawStage(int stage,const char* name) {
    if(stage<=launchUiDrawStage) return;
    launchUiDrawStage=stage;
    opennow::LogAppLifecycleEvent("UI_LAUNCH_DRAW_STAGE",name);
}
struct NativeKey { int keycode=0; opennow::input::KeyboardStroke stroke{}; };
static NativeKey nativeKeys[32]{};
static int nativeSearchKeys[32]{};
static bool initPs4Keyboard() {
    int rc=sceSysmoduleLoadModule(ORBIS_SYSMODULE_KEYBOARD);
    if(rc<0 && rc!=0x80960003) { ps4KeyboardInitRc=rc; return false; }
    sceUserServiceInitialize(nullptr);
    int userId=0; if(sceUserServiceGetInitialUser(&userId)<0) userId=0;
    ps4KeyboardInitRc=sceKeyboardInit();
    if(ps4KeyboardInitRc<0 && ps4KeyboardInitRc!=0x80960003) return false;
    ps4KeyboardHandle=sceKeyboardOpen(userId,0,0,nullptr);
    ps4KeyboardOpenRc=ps4KeyboardHandle;
    return ps4KeyboardHandle>=0;
}
static bool initPs4Pad() {
    const int initRc=scePadInit();
    if(initRc<0 && initRc!=0x80960003) { ps4PadOpenRc=initRc; return false; }
    sceUserServiceInitialize(nullptr);
    int userId=0; if(sceUserServiceGetInitialUser(&userId)<0) userId=0;
    ps4PadHandle=scePadOpen(userId,ORBIS_PAD_PORT_TYPE_STANDARD,0,nullptr);
    ps4PadOpenRc=ps4PadHandle;
    ps4PadReady=ps4PadHandle>=0;
    return ps4PadReady;
}
static float nativeStick(uint8_t value) {
    return std::clamp((static_cast<float>(value)-127.5f)/127.5f,-1.0f,1.0f);
}
static float calibratedStickValue(int value,int slot,bool native) {
    const bool hasCenter=inputCalibrationReady && inputCalibrationNative==native;
    const float center=hasCenter?static_cast<float>(inputAxisCenter[slot]):(native?127.5f:0.0f);
    const float delta=static_cast<float>(value)-center;
    const float range=delta<0?center-(native?0.0f:-32768.0f):(native?255.0f:32767.0f)-center;
    float normalized=range>1.0f?delta/range:0.0f;
    constexpr float deadzone=0.12f;
    const float magnitude=std::fabs(normalized);
    if(magnitude<=deadzone) return 0.0f;
    normalized=std::copysign((magnitude-deadzone)/(1.0f-deadzone),normalized);
    return std::clamp(normalized,-1.0f,1.0f);
}
static uint8_t calibratedTriggerValue(int value,int slot,bool native) {
    if(!inputCalibrationReady || inputCalibrationNative!=native) {
        if(native) return static_cast<uint8_t>(std::clamp(value,0,255));
        return static_cast<uint8_t>(std::clamp((value+32768)*255/65535,0,255));
    }
    const int maximum=native?255:32767;
    const int span=std::max(1,maximum-inputTriggerRest[slot]);
    const float amount=std::clamp(static_cast<float>(value-inputTriggerRest[slot])/span,0.0f,1.0f);
    constexpr float deadzone=0.06f;
    if(amount<=deadzone) return 0;
    return static_cast<uint8_t>(std::clamp(static_cast<int>((amount-deadzone)*255.0f/(1.0f-deadzone)),0,255));
}
static uint8_t genericTriggerValue(int value,int slot,int device=0) {
    const int rest=(inputCalibrationReady && !inputCalibrationNative && device==0)
        ?inputTriggerRest[slot]:genericTriggerRest[device][slot];
    const int towardMin=rest+32768,towardMax=32767-rest;
    const int span=std::max(1,std::max(towardMin,towardMax));
    const float amount=std::clamp(static_cast<float>(std::abs(value-rest))/span,0.0f,1.0f);
    // A 6% dead zone combined with the digital-button fallback turned even a
    // tiny analog R2 press into a full 100% trigger. Preserve its travel.
    constexpr float deadzone=0.004f;
    if(amount<=deadzone) return 0;
    return static_cast<uint8_t>(std::clamp(static_cast<int>((amount-deadzone)*255.0f/(1.0f-deadzone)),0,255));
}
static int genericStickAxis(int slot) {
    if(slot<2) return slot;
    return genericSplitTriggerAxes[0]?slot+2:slot;
}
static int genericTriggerAxis(int slot) {
    return genericSplitTriggerAxes[0]?slot+2:slot+4;
}
static int genericTriggerRestValue(int slot,int device=0) {
    return (inputCalibrationReady && !inputCalibrationNative && device==0)
        ?inputTriggerRest[slot]:genericTriggerRest[device][slot];
}
static void traceGamepadAnalogSample(const int raw[6],bool l2Button,bool r2Button) {
    static int previous[6]={0,0,0,0,0,0};
    static bool previousL2=false,previousR2=false,havePrevious=false;
    static Uint32 lastLoggedAt=0;
    const Uint32 now=SDL_GetTicks();
    bool changed=!havePrevious || l2Button!=previousL2 || r2Button!=previousR2;
    for(int i=0;i<6 && !changed;i++) if(std::abs(raw[i]-previous[i])>=128) changed=true;
    if(!changed || static_cast<Uint32>(now-lastLoggedAt)<120u) return;
    char detail[220];
    snprintf(detail,sizeof(detail),"axes=%d,%d,%d,%d,%d,%d B18=%d B19=%d L2=%d%% R2=%d%%",
        raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],l2Button?1:0,r2Button?1:0,
        genericTriggerValue(raw[genericTriggerAxis(0)],0)*100/255,
        genericTriggerValue(raw[genericTriggerAxis(1)],1)*100/255);
    opennow::LogAppLifecycleEvent("GAMEPAD_ANALOG_SAMPLE",detail);
    for(int i=0;i<6;i++) previous[i]=raw[i];
    previousL2=l2Button; previousR2=r2Button; havePrevious=true; lastLoggedAt=now;
}
static bool beginInputCalibration() {
    const Uint32 now=SDL_GetTicks();
    if(inputCalibrationSampling || (inputCalibrationLastAt && static_cast<Uint32>(now-inputCalibrationLastAt)<1500u)) return false;
    if(!ps4PadReady && !gameControllers[0] && !genericJoysticks[0]) return false;
    inputCalibrationSampling=true; inputCalibrationStartedAt=now; inputCalibrationSamples=0;
    inputCalibrationStableSince=0; inputCalibrationHavePrevious=false; inputCalibrationStableSamples=0;
    for(int i=0;i<6;i++) inputCalibrationStableSum[i]=0;
    for(int i=0;i<6;i++) { inputCalibrationMin[i]=32767; inputCalibrationMax[i]=-32768; inputCalibrationSum[i]=0; }
    snprintf(inputCalibrationStatus,sizeof(inputCalibrationStatus),"SUELTA LOS STICKS Y NO TOQUES EL MANDO");
    opennow::LogAppLifecycleEvent("GAMEPAD_CALIBRATION_BEGIN","method=stable_rest_capture timeout_ms=8000");
    return true;
}
static void sampleInputCalibration() {
    if(!inputCalibrationSampling) return;
    bool native=ps4PadReady;
    int sample[6]{};
    bool triggerPressed=false;
    if(native) {
        sample[0]=ps4PadState.leftStick.x; sample[1]=ps4PadState.leftStick.y;
        sample[2]=ps4PadState.rightStick.x; sample[3]=ps4PadState.rightStick.y;
        sample[4]=ps4PadState.analogButtons.l2; sample[5]=ps4PadState.analogButtons.r2;
        triggerPressed=sample[4]>18 || sample[5]>18;
    } else if(gameControllers[0]) {
        SDL_GameController* pad=gameControllers[0];
        sample[0]=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX); sample[1]=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY);
        sample[2]=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTX); sample[3]=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTY);
        sample[4]=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT); sample[5]=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        triggerPressed=sample[4]>12000 || sample[5]>12000;
    } else if(genericJoysticks[0] && readFreshSdlPs4State(genericJoysticks[0],ps4PadState)) {
        native=true;
        sample[0]=ps4PadState.leftStick.x; sample[1]=ps4PadState.leftStick.y;
        sample[2]=ps4PadState.rightStick.x; sample[3]=ps4PadState.rightStick.y;
        sample[4]=ps4PadState.analogButtons.l2; sample[5]=ps4PadState.analogButtons.r2;
        triggerPressed=sample[4]>18 || sample[5]>18;
    } else if(genericJoysticks[0]) {
        SDL_Joystick* pad=genericJoysticks[0];
        for(int i=0;i<4;i++) { const int axis=genericStickAxis(i); sample[i]=SDL_JoystickNumAxes(pad)>axis?SDL_JoystickGetAxis(pad,axis):0; }
        for(int i=0;i<2;i++) { const int axis=genericTriggerAxis(i); sample[4+i]=SDL_JoystickNumAxes(pad)>axis?SDL_JoystickGetAxis(pad,axis):genericTriggerRest[0][i]; }
        triggerPressed=(SDL_JoystickNumButtons(pad)>18 && SDL_JoystickGetButton(pad,18)) ||
                       (SDL_JoystickNumButtons(pad)>19 && SDL_JoystickGetButton(pad,19));
    } else { inputCalibrationSampling=false; return; }
    if(triggerPressed) {
        inputCalibrationSampling=false;
        snprintf(inputCalibrationStatus,sizeof(inputCalibrationStatus),"CALIBRACION CANCELADA: SUELTA L2 Y R2");
        opennow::LogAppLifecycleEvent("GAMEPAD_CALIBRATION_REJECTED","reason=trigger_pressed");
        return;
    }
    const Uint32 now=SDL_GetTicks();
    for(int i=0;i<6;i++) inputCalibrationSum[i]+=sample[i];
    ++inputCalibrationSamples;
    bool stable=inputCalibrationHavePrevious;
    const int stabilityLimit=native?3:350;
    for(int i=0;i<4 && stable;i++) if(std::abs(sample[i]-inputCalibrationPrevious[i])>stabilityLimit) stable=false;
    for(int i=0;i<6;i++) inputCalibrationPrevious[i]=sample[i];
    inputCalibrationHavePrevious=true;
    const bool releaseDelayElapsed=static_cast<Uint32>(now-inputCalibrationStartedAt)>=300u;
    if(releaseDelayElapsed && stable) {
        if(!inputCalibrationStableSince) {
            inputCalibrationStableSince=now; inputCalibrationStableSamples=0;
            for(int i=0;i<6;i++) { inputCalibrationStableSum[i]=0; inputCalibrationMin[i]=sample[i]; inputCalibrationMax[i]=sample[i]; }
        }
        for(int i=0;i<6;i++) {
            inputCalibrationMin[i]=std::min(inputCalibrationMin[i],sample[i]);
            inputCalibrationMax[i]=std::max(inputCalibrationMax[i],sample[i]);
        }
        for(int i=0;i<6;i++) inputCalibrationStableSum[i]+=sample[i];
        ++inputCalibrationStableSamples;
        snprintf(inputCalibrationStatus,sizeof(inputCalibrationStatus),"LEYENDO REPOSO: NO TOQUES EL MANDO");
    } else {
        inputCalibrationStableSince=0; inputCalibrationStableSamples=0;
        for(int i=0;i<6;i++) inputCalibrationStableSum[i]=0;
        snprintf(inputCalibrationStatus,sizeof(inputCalibrationStatus),"SUELTA LOS STICKS Y NO TOQUES EL MANDO");
    }
    if(static_cast<Uint32>(now-inputCalibrationStartedAt)>8000u) {
        inputCalibrationSampling=false;
        snprintf(inputCalibrationStatus,sizeof(inputCalibrationStatus),"NO HUBO REPOSO ESTABLE; SUELTA Y REPITE");
        char detail[200];
        snprintf(detail,sizeof(detail),"reason=rest_not_stable raw=%d,%d,%d,%d jitter=%d,%d,%d,%d",
            sample[0],sample[1],sample[2],sample[3],inputCalibrationMax[0]-inputCalibrationMin[0],
            inputCalibrationMax[1]-inputCalibrationMin[1],inputCalibrationMax[2]-inputCalibrationMin[2],inputCalibrationMax[3]-inputCalibrationMin[3]);
        opennow::LogAppLifecycleEvent("GAMEPAD_CALIBRATION_REJECTED",detail);
        return;
    }
    if(!inputCalibrationStableSince || static_cast<Uint32>(now-inputCalibrationStableSince)<1200u || inputCalibrationStableSamples<45) return;
    if(isSdlPs4Joystick(genericJoysticks[0])) {
        // An arbitrary stable position is not a neutral point: the previous
        // algorithm accepted a stick held at the edge and stored that as center.
        // For DS4's unsigned 0..255 stick values, only accept a center near 128.
        for(int i=0;i<4;i++) {
            if(std::abs(static_cast<int>(inputCalibrationStableSum[i]/inputCalibrationStableSamples)-128)>32) {
                inputCalibrationSampling=false;
                snprintf(inputCalibrationStatus,sizeof(inputCalibrationStatus),"CENTRO FUERA DE RANGO: SUELTA LOS STICKS Y REPITE");
                char detail[160]; snprintf(detail,sizeof(detail),"reason=non_neutral_ds4_axis axis=%d center=%d expected=128 tolerance=32",i,static_cast<int>(inputCalibrationStableSum[i]/inputCalibrationStableSamples));
                opennow::LogAppLifecycleEvent("GAMEPAD_CALIBRATION_REJECTED",detail);
                return;
            }
        }
        inputCalibrationNative=true;
    }
    inputCalibrationSampling=false;
    inputCalibrationNative=native;
    // Center on the controller's measured resting values. Some SDL/USB pads
    // report a hardware offset even when released, so requiring raw zero (or
    // proximity to the axis midpoint) makes them impossible to calibrate.
    for(int i=0;i<4;i++) inputAxisCenter[i]=static_cast<int>(inputCalibrationStableSum[i]/inputCalibrationStableSamples);
    for(int i=0;i<2;i++) inputTriggerRest[i]=static_cast<int>(inputCalibrationStableSum[4+i]/inputCalibrationStableSamples);
    inputCalibrationReady=true; inputCalibrationLastAt=SDL_GetTicks();
    snprintf(inputCalibrationStatus,sizeof(inputCalibrationStatus),"CALIBRACION LISTA: REPOSO COMPENSADO");
    char detail[240];
    snprintf(detail,sizeof(detail),"source=%s samples=%d rest_samples=%d center_rest=%d,%d,%d,%d rest_jitter=%d,%d,%d,%d triggers_rest=%d,%d",
        inputCalibrationNative?"scePad":"SDL",inputCalibrationSamples,inputCalibrationStableSamples,
        inputAxisCenter[0],inputAxisCenter[1],inputAxisCenter[2],inputAxisCenter[3],
        inputCalibrationMax[0]-inputCalibrationMin[0],inputCalibrationMax[1]-inputCalibrationMin[1],
        inputCalibrationMax[2]-inputCalibrationMin[2],inputCalibrationMax[3]-inputCalibrationMin[3],inputTriggerRest[0],inputTriggerRest[1]);
    opennow::LogAppLifecycleEvent("GAMEPAD_CENTER_CALIBRATED",detail);
}
static SDL_Keycode nativeKeyToSdl(int keycode,int character) {
    if(character>=32 && character<=126) return static_cast<SDL_Keycode>(character);
    if(keycode>=4 && keycode<=29) return static_cast<SDL_Keycode>(SDLK_a+(keycode-4));
    if(keycode>=30 && keycode<=38) return static_cast<SDL_Keycode>('1'+keycode-30);
    if(keycode==39) return SDLK_0;
    switch(keycode) {
    case 40:return SDLK_RETURN; case 41:return SDLK_ESCAPE; case 42:return SDLK_BACKSPACE; case 43:return SDLK_TAB; case 44:return SDLK_SPACE;
    case 45:return SDLK_MINUS; case 46:return SDLK_EQUALS; case 47:return SDLK_LEFTBRACKET; case 48:return SDLK_RIGHTBRACKET; case 49:return SDLK_BACKSLASH;
    case 51:return SDLK_SEMICOLON; case 52:return SDLK_QUOTE; case 53:return SDLK_BACKQUOTE; case 54:return SDLK_COMMA; case 55:return SDLK_PERIOD; case 56:return SDLK_SLASH;
    case 57:return SDLK_CAPSLOCK; case 73:return SDLK_INSERT; case 74:return SDLK_HOME; case 75:return SDLK_PAGEUP; case 76:return SDLK_DELETE; case 77:return SDLK_END; case 78:return SDLK_PAGEDOWN;
    case 79:return SDLK_RIGHT; case 80:return SDLK_LEFT; case 81:return SDLK_DOWN; case 82:return SDLK_UP;
    default: return SDLK_UNKNOWN;
    }
}
static void pollPs4Keyboard() {
    if(ps4KeyboardHandle<0) return;
    OrbisKeyboardData data{};
    if(sceKeyboardReadState(ps4KeyboardHandle,&data)<0) return;
    if(data.nkeys>0 && !keyboardActivityLatched) {
        opennow::LogAppLifecycleEvent("USB_KEY_ACTIVITY",("page="+std::to_string(page)+" keys="+std::to_string(std::min<int>(data.nkeys,32))).c_str());
        keyboardActivityLatched=true;
    } else if(data.nkeys<=0) keyboardActivityLatched=false;
    if(page==3 && catalogSearchActive) {
        for(int i=0;i<std::min<int>(data.nkeys,32);i++) {
            const int code=data.keycodes[i]; if(std::find(std::begin(nativeSearchKeys),std::end(nativeSearchKeys),code)!=std::end(nativeSearchKeys)) continue;
            OrbisKeyboardKey2Char converted{}; const int rc=sceKeyboardGetKey2Char(ps4KeyboardHandle,false,data.locks,data.mods,code,&converted);
            if(code==42) eraseCatalogSearchChar();
            else if(code==40) toggleCatalogSearch();
            else if(rc>=0 && converted.ok && converted.keycode>=32 && converted.keycode<=126) { const char ch=static_cast<char>(converted.keycode); const char input[2]={ch,0}; editCatalogSearch(input); }
        }
        std::fill(std::begin(nativeSearchKeys),std::end(nativeSearchKeys),0);
        for(int i=0;i<std::min<int>(data.nkeys,32);i++) nativeSearchKeys[i]=data.keycodes[i];
        return;
    }
    if(inputDevice==0) return;
    if(page!=5 || !hasPublishedStream()) return;
    if(streamMenuVisible || streamSuppressInputUntilNeutral) {
        for(NativeKey& old:nativeKeys) if(old.keycode) {
            activeStream->send_keyboard_key(old.stroke.keycode,old.stroke.scancode,old.stroke.modifiers,false);
            old={};
        }
        return;
    }
    auto hasKey=[&](int code) { for(int i=0;i<std::min<int>(data.nkeys,32);i++) if(data.keycodes[i]==code) return true; return false; };
    for(NativeKey& old:nativeKeys) if(old.keycode && !hasKey(old.keycode)) { activeStream->send_keyboard_key(old.stroke.keycode,old.stroke.scancode,old.stroke.modifiers,false); old={}; }
    for(int i=0;i<std::min<int>(data.nkeys,32);i++) {
        const int code=data.keycodes[i]; bool known=false; for(const NativeKey& old:nativeKeys) if(old.keycode==code) { known=true; break; }
        if(known) continue;
        OrbisKeyboardKey2Char converted{}; const int rc=sceKeyboardGetKey2Char(ps4KeyboardHandle,false,data.locks,data.mods,code,&converted);
        const SDL_Keycode key=nativeKeyToSdl(code,rc>=0 && converted.ok?converted.keycode:0);
        opennow::input::KeyboardStroke stroke{};
        if(key==SDLK_UNKNOWN || !mapSdlKey(key,0,stroke)) { opennow::LogAppLifecycleEvent("USB_KEY_UNMAPPED",("hid="+std::to_string(code)).c_str()); continue; }
        if(data.mods&(ORBIS_KEYBOARD_MOD_LEFT_SHIFT|ORBIS_KEYBOARD_MOD_RIGHT_SHIFT)) stroke.modifiers|=0x0001;
        if(data.mods&(ORBIS_KEYBOARD_MOD_LEFT_CTRL|ORBIS_KEYBOARD_MOD_RIGHT_CTRL)) stroke.modifiers|=0x0002;
        if(data.mods&(ORBIS_KEYBOARD_MOD_LEFT_ALT|ORBIS_KEYBOARD_MOD_RIGHT_ALT)) stroke.modifiers|=0x0004;
        if(data.mods&(ORBIS_KEYBOARD_MOD_LEFT_META|ORBIS_KEYBOARD_MOD_RIGHT_META)) stroke.modifiers|=0x0008;
        NativeKey* slot=nullptr; for(NativeKey& old:nativeKeys) if(!old.keycode) { slot=&old; break; }
        if(slot) { *slot={code,stroke}; activeStream->send_keyboard_key(stroke.keycode,stroke.scancode,stroke.modifiers,true); }
    }
}
static std::vector<unsigned char> uiFontBytes;
static stbtt_fontinfo uiFont{};
static bool uiFontReady=false;
static uint64_t statsLastBytes=0,statsLastPresented=0,statsLastDecoded=0;
static Uint32 statsLastSampleMs=0;
static Uint32 streamHealthLogAt=0;
static int statsVideoKbps=0,statsVideoFps=0,statsDecodeFps=0,statsSwapFps=0;
// =================================================================================================
// NOMBRES DE RESOLUCION: TIENEN QUE DECIR LA VERDAD (v3.95)
// =================================================================================================
// POR QUE CAMBIAN. Este array se dibuja en DOS sitios (el perfil del catalogo, lineas ~5213 y ~5919) con
// el formato `PERFIL: %s / %d MBPS / %d FPS`. Antes decia `{"AUTO (720P)", "720P", "1080P"}`.
//
// **"1080P" era mentira.** `resolution_to_wh()` fuerza 720p para los TRES modos, porque:
//
//     framebuffer 1920x1080 -> flips presentados = 1   -> SE CERRO
//     framebuffer 1280x720  -> flips presentados = 119 -> sesion estable
//
// Es decir: el usuario elegia "1080P" y la pantalla le decia "PERFIL: 1080P", pero **se pedia y se
// aplicaba 720p**. Y con 1080p ademas harian falta ~31.000 us por frame solo de escalado, imposible
// para 60 fps.
//
// **Por que importa el texto y no solo el valor:** una interfaz que promete algo que no ocurre hace que
// cualquier problema posterior parezca aleatorio. El usuario no puede saber que su eleccion se ignora.
// Ahora el nombre dice que 1080p no esta disponible, asi que la eleccion es informada.
//
// NOTA: los indices NO cambian (0/1/2), porque son el valor que se guarda en `settings.cfg`. Solo cambia
// lo que se muestra. Un archivo de ajustes antiguo con `2` sigue cargando exactamente igual.
// El texto del indice 2 dice **720P** a proposito: es lo que de verdad se pide. `resolution_to_wh()`
// fuerza 720p en los tres modos, asi que prometer 1080p seria mentir en los otros dos sitios donde se
// dibuja este array (el perfil del catalogo).
static const char* resolutions[] = {"AUTO (720P)", "720P", "720P (1080P NO DISPONIBLE)"};
static void resolution_to_wh(int mode, int& w, int& h) {
    switch(mode) {
        case 0:  w = 1280; h = 720;  break; // Auto -> 720p base
        case 1:  w = 1280; h = 720;  break; // 720p explicito
        case 2:  w = 1280; h = 720;  break; // 1080p -> FORZADO a 720p (ver bloque de abajo)
        default: w = 1280; h = 720;  break; // Fallback seguro
    }
    // 1080P DESHABILITADO: SE APLICA 720P (objetivo v3.30, punto A del plan).
    //
    // POR QUE: el unico intento de sesion que cerro la aplicacion con "Se produjo un error" es el
    // que tenia framebuffer 1920x1080. La evidencia del log es concluyente:
    //
    //     framebuffer 1920x1080 -> flips presentados = 1   -> SE CERRO
    //     framebuffer 1280x720  -> flips presentados = 119 -> sesion estable
    //
    // El handoff se completaba sano (DMEM_ALLOC_OK, REGISTER_BUFFERS_OK, FLIP_EQUEUE_OK) y el log se
    // cortaba justo despues del primer VIDEOOUT_FLIP_SUBMIT. Escribir un frame BGRA de 1080p son
    // 7,9 MB por frame (475 MB/s a 60 Hz) frente a 3,5 MB (210 MB/s) a 720p: 2,25 veces mas, sobre
    // memoria visible por GPU. El primer frame se presentaba, el segundo ya no llegaba.
    //
    // Ademas, escalar 960x540 (lo que el servidor entrega) a 1920x1080 son 2.073.600 pixeles frente
    // a 921.600: ~31.000 us por frame, lo que hace imposible sostener 60 FPS.
    //
    // Por las dos razones, el modo 1080p se fuerza a 720p y se registra el motivo una sola vez.
    if (mode == 2) {
        static bool s_logged1080Blocked = false;
        w = 1280; h = 720;
        if (!s_logged1080Blocked) {
            s_logged1080Blocked = true;
            // Nota: este log se emite desde una funcion sin acceso a LogAppLifecycleEvent en el
            // arranque mas temprano, asi que se usa WriteStreamStartupStage que ya esta enlazado.
            opennow::LogAppLifecycleEvent(
                "VIDEOOUT_1080P_BLOCKED",
                "reason=framebuffer_1080p_crashed_app_1_flip_vs_119; applied=1280x720");
        }
    }
}
static const char* kPublicCatalogUrl = "https://static.nvidiagrid.net/supported-public-game-list/locales/gfnpc-en-US.json";
static void rebuildVisibleCatalog();
static void startCatalogFetch();
// Keep the quick account library separate from the optional full catalog cache.
static const char* kAccountCatalogSnapshotPath = "/data/gfnps4/account_library_cache_v3.json";
static const char* kAccountCatalogSnapshotTempPath = "/data/gfnps4/account_library_cache_v3.tmp";
static const char* kFullCatalogSnapshotPath = "/data/gfnps4/full_catalog_cache_v1.json";
static const char* kFullCatalogSnapshotTempPath = "/data/gfnps4/full_catalog_cache_v1.tmp";
static constexpr std::time_t kCatalogSnapshotMaxAge = 24 * 60 * 60;

static bool catalogSnapshotIsFresh(bool fullCatalog) {
    struct stat info{};
    const char* path=fullCatalog?kFullCatalogSnapshotPath:kAccountCatalogSnapshotPath;
    if(stat(path,&info)!=0 || info.st_size<=0 || info.st_size>4*1024*1024) return false;
    const std::time_t now=std::time(nullptr);
    return now>0 && info.st_mtime<=now+300 && now-info.st_mtime<=kCatalogSnapshotMaxAge;
}

static bool loadCatalogSnapshot(bool fullCatalog=false) {
    if(!catalogSnapshotIsFresh(fullCatalog)) return false;
    FILE* file=fopen(fullCatalog?kFullCatalogSnapshotPath:kAccountCatalogSnapshotPath,"rb");
    if(!file) return false;
    if(fseek(file,0,SEEK_END)!=0) { fclose(file); return false; }
    const long size=ftell(file);
    if(size<=0 || size>4*1024*1024 || fseek(file,0,SEEK_SET)!=0) { fclose(file); return false; }
    std::vector<char> json(static_cast<size_t>(size)+1,0);
    const size_t read=fread(json.data(),1,static_cast<size_t>(size),file);
    fclose(file);
    if(read!=static_cast<size_t>(size)) return false;
    cJSON* root=cJSON_ParseWithLength(json.data(),read);
    if(!cJSON_IsArray(root)) { if(root) cJSON_Delete(root); return false; }
    int count=0;
    for(cJSON* item=root->child;item && count<2400;item=item->next) {
        cJSON* title=cJSON_GetObjectItemCaseSensitive(item,"title");
        cJSON* store=cJSON_GetObjectItemCaseSensitive(item,"store");
        cJSON* appId=cJSON_GetObjectItemCaseSensitive(item,"appId");
        cJSON* image=cJSON_GetObjectItemCaseSensitive(item,"image");
        if(!cJSON_IsString(title) || !title->valuestring || !title->valuestring[0] ||
           !cJSON_IsString(appId) || !appId->valuestring || !appId->valuestring[0]) continue;
        CatalogGame& game=pendingCatalogGames[count];
        strncpy(game.title,title->valuestring,sizeof(game.title)-1); game.title[sizeof(game.title)-1]='\0';
        const char* storeName=cJSON_IsString(store)&&store->valuestring?store->valuestring:"GFN";
        strncpy(game.store,storeName,sizeof(game.store)-1); game.store[sizeof(game.store)-1]='\0';
        strncpy(game.launchAppId,appId->valuestring,sizeof(game.launchAppId)-1); game.launchAppId[sizeof(game.launchAppId)-1]='\0';
        const char* imageUrl=cJSON_IsString(image)&&image->valuestring?image->valuestring:"";
        strncpy(game.imageUrl,imageUrl,sizeof(game.imageUrl)-1); game.imageUrl[sizeof(game.imageUrl)-1]='\0';
        cJSON* linked=cJSON_GetObjectItemCaseSensitive(item,"linked");
        game.linked=fullCatalog?(cJSON_IsTrue(linked)?1:0):1;
        ++count;
    }
    cJSON_Delete(root);
    if(count==0) return false;
    pendingCatalogCount.store(count,std::memory_order_relaxed);
    pendingCatalogReady.store(true,std::memory_order_release);
    catalogHttpStatus.store(200); catalogError.store(0);
    pendingCatalogIsFull.store(fullCatalog,std::memory_order_release);
    catalogFullLoaded.store(fullCatalog,std::memory_order_release);
    opennow::LogAppLifecycleEvent("CATALOG_CACHE_STAGED",("games="+std::to_string(count)+(fullCatalog?" source=full":" source=account")).c_str());
    return true;
}

static void saveCatalogSnapshot(const CatalogGame* games,int count,bool fullCatalog=false) {
    if(count<=0 || count>2400) return;
    cJSON* root=cJSON_CreateArray();
    if(!root) return;
    for(int i=0;i<count;i++) {
        const CatalogGame& game=games[i];
        cJSON* item=cJSON_CreateObject();
        if(!item || !cJSON_AddStringToObject(item,"title",game.title) ||
           !cJSON_AddStringToObject(item,"store",game.store) ||
           !cJSON_AddStringToObject(item,"appId",game.launchAppId) ||
           !cJSON_AddStringToObject(item,"image",game.imageUrl) ||
           !cJSON_AddBoolToObject(item,"linked",game.linked!=0)) {
            if(item) cJSON_Delete(item);
            cJSON_Delete(root);
            return;
        }
        cJSON_AddItemToArray(root,item);
    }
    char* encoded=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(!encoded) return;
    const size_t size=strlen(encoded);
    if(size==0 || size>4*1024*1024) { cJSON_free(encoded); return; }
    mkdir("/data/gfnps4",0777);
    FILE* file=fopen(fullCatalog?kFullCatalogSnapshotTempPath:kAccountCatalogSnapshotTempPath,"wb");
    bool ok=file!=nullptr;
    if(file) {
        ok=fwrite(encoded,1,size,file)==size && fflush(file)==0 && fsync(fileno(file))==0;
        if(fclose(file)!=0) ok=false;
    }
    cJSON_free(encoded);
    const char* tempPath=fullCatalog?kFullCatalogSnapshotTempPath:kAccountCatalogSnapshotTempPath;
    const char* snapshotPath=fullCatalog?kFullCatalogSnapshotPath:kAccountCatalogSnapshotPath;
    if(ok && rename(tempPath,snapshotPath)==0)
        opennow::LogAppLifecycleEvent("CATALOG_CACHE_SAVED",("games="+std::to_string(count)+(fullCatalog?" source=full":" source=account")).c_str());
    else std::remove(tempPath);
}

struct CatalogCoverJob { std::string url; uint64_t generation; };
static std::string catalogCoverCachePath(const std::string& url) {
    uint64_t hash=1469598103934665603ULL;
    for(unsigned char c:url) { hash^=c; hash*=1099511628211ULL; }
    char path[128]; snprintf(path,sizeof(path),"/data/gfnps4/covers/%016llx.jpg",static_cast<unsigned long long>(hash));
    return path;
}
static bool decodeCatalogJpeg(const std::string& bytes,std::vector<uint8_t>& rgba,int& width,int& height) {
    if(bytes.empty() || bytes.size()>4*1024*1024) return false;
    const bool png=bytes.size()>=8 && static_cast<unsigned char>(bytes[0])==0x89 && bytes.compare(1,3,"PNG")==0;
    const AVCodec* codec=avcodec_find_decoder(png?AV_CODEC_ID_PNG:AV_CODEC_ID_MJPEG);
    if(!codec) return false;
    AVCodecContext* context=avcodec_alloc_context3(codec);
    AVPacket* packet=av_packet_alloc();
    AVFrame* frame=av_frame_alloc();
    if(context) {
        // Reject oversized cover dimensions before the decoder allocates the
        // output frame. The post-decode width/height check alone is too late
        // for a small compressed image with a hostile/broken IHDR or SOF.
        context->max_pixels=1600LL*1600LL;
        context->thread_count=1;
        context->thread_type=0;
        context->err_recognition=AV_EF_EXPLODE;
    }
    bool ok=false;
    if(context && packet && frame && avcodec_open2(context,codec,nullptr)>=0 &&
       av_new_packet(packet,static_cast<int>(bytes.size()))>=0) {
        memcpy(packet->data,bytes.data(),bytes.size());
        if(avcodec_send_packet(context,packet)>=0 && avcodec_receive_frame(context,frame)>=0 &&
           frame->width>0 && frame->height>0 && frame->width<=1600 && frame->height<=1600) {
            int hs=0,vs=0;
            switch(static_cast<AVPixelFormat>(frame->format)) {
            case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: hs=1;vs=1;break;
            case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: hs=1;vs=0;break;
            case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P: hs=0;vs=0;break;
            default: break;
            }
            const auto format=static_cast<AVPixelFormat>(frame->format);
            if(format==AV_PIX_FMT_RGB24 || format==AV_PIX_FMT_RGBA || format==AV_PIX_FMT_BGRA ||
               format==AV_PIX_FMT_RGB0 || format==AV_PIX_FMT_BGR0) {
                const int sourceWidth=frame->width,sourceHeight=frame->height;
                const float scale=std::min(1.0f,std::min(640.0f/sourceWidth,480.0f/sourceHeight));
                width=std::max(1,static_cast<int>(sourceWidth*scale));
                height=std::max(1,static_cast<int>(sourceHeight*scale));
                const int channels=(format==AV_PIX_FMT_RGB24?3:4);
                rgba.resize(static_cast<size_t>(width)*height*4);
                for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                    const int sx=std::min(sourceWidth-1,x*sourceWidth/width);
                    const int sy=std::min(sourceHeight-1,y*sourceHeight/height);
                    const uint8_t* src=frame->data[0]+sy*frame->linesize[0]+sx*channels;
                    uint8_t* dst=rgba.data()+(static_cast<size_t>(y)*width+x)*4;
                    const bool bgra=(format==AV_PIX_FMT_BGRA || format==AV_PIX_FMT_BGR0);
                    dst[0]=src[bgra?2:0]; dst[1]=src[1]; dst[2]=src[bgra?0:2]; dst[3]=(channels==4 && (format==AV_PIX_FMT_RGBA || format==AV_PIX_FMT_BGRA))?src[3]:255;
                }
                ok=true;
            }
            if(frame->data[0] && frame->data[1] && frame->data[2] &&
               (format==AV_PIX_FMT_YUV420P || format==AV_PIX_FMT_YUVJ420P ||
                format==AV_PIX_FMT_YUV422P || format==AV_PIX_FMT_YUVJ422P ||
                format==AV_PIX_FMT_YUV444P || format==AV_PIX_FMT_YUVJ444P)) {
                const int sourceWidth=frame->width,sourceHeight=frame->height;
                const float scale=std::min(1.0f,std::min(640.0f/sourceWidth,480.0f/sourceHeight));
                width=std::max(1,static_cast<int>(sourceWidth*scale));
                height=std::max(1,static_cast<int>(sourceHeight*scale));
                rgba.resize(static_cast<size_t>(width)*height*4);
                const bool full=(format==AV_PIX_FMT_YUVJ420P || format==AV_PIX_FMT_YUVJ422P || format==AV_PIX_FMT_YUVJ444P);
                for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                    const int sx=std::min(sourceWidth-1,x*sourceWidth/width);
                    const int sy=std::min(sourceHeight-1,y*sourceHeight/height);
                    const int yy=frame->data[0][sy*frame->linesize[0]+sx];
                    const int uu=frame->data[1][(sy>>vs)*frame->linesize[1]+(sx>>hs)]-128;
                    const int vv=frame->data[2][(sy>>vs)*frame->linesize[2]+(sx>>hs)]-128;
                    int r,g,b;
                    if(full) { r=yy+((359*vv+128)>>8); g=yy-((88*uu+183*vv+128)>>8); b=yy+((454*uu+128)>>8); }
                    else { const int c=std::max(0,yy-16); r=(298*c+409*vv+128)>>8; g=(298*c-100*uu-208*vv+128)>>8; b=(298*c+516*uu+128)>>8; }
                    const size_t out=(static_cast<size_t>(y)*width+x)*4;
                    rgba[out]=static_cast<uint8_t>(std::clamp(r,0,255)); rgba[out+1]=static_cast<uint8_t>(std::clamp(g,0,255));
                    rgba[out+2]=static_cast<uint8_t>(std::clamp(b,0,255)); rgba[out+3]=255;
                }
                ok=true;
            }
        }
    }
    if(frame) av_frame_free(&frame);
    if(packet) av_packet_free(&packet);
    if(context) avcodec_free_context(&context);
    return ok;
}
static int loadCatalogCover(void* opaque) {
    std::unique_ptr<CatalogCoverJob> job(static_cast<CatalogCoverJob*>(opaque));
    bool ok=false; std::vector<uint8_t> pixels; int width=0,height=0;
    try {
        const std::string path=catalogCoverCachePath(job->url);
        std::string bytes;
        bool fetched=false;
        FILE* cached=fopen(path.c_str(),"rb");
        if(cached) {
            if(fseek(cached,0,SEEK_END)==0) {
                const long size=ftell(cached);
                if(size>0 && size<=4*1024*1024 && fseek(cached,0,SEEK_SET)==0) {
                    bytes.resize(static_cast<size_t>(size));
                    if(fread(bytes.data(),1,bytes.size(),cached)!=bytes.size()) bytes.clear();
                }
            }
            fclose(cached);
        }
        if(bytes.empty()) {
            opennow::HttpClient http;
            const auto response=http.Get(job->url,opennow::GfnClient::kUserAgent,
                {"Accept: image/jpeg,image/png,image/*,*/*;q=0.8"},{},
                {nullptr,4*1024*1024,5000});
            if(response.status_code==200 && !response.body.empty()) {
                bytes=response.body; fetched=true;
            }
        }
        ok=decodeCatalogJpeg(bytes,pixels,width,height);
        if(ok && fetched) {
            mkdir("/data/gfnps4",0777); mkdir("/data/gfnps4/covers",0777);
            FILE* out=fopen(path.c_str(),"wb");
            if(out) { fwrite(bytes.data(),1,bytes.size(),out); fclose(out); }
        } else if(!ok && !bytes.empty()) std::remove(path.c_str());
        if(!ok) opennow::LogAppLifecycleEvent("CATALOG_COVER_DECODE_FAILED",
            ("bytes="+std::to_string(bytes.size())+" cached="+(fetched?"no":"yes")).c_str());
    } catch(const std::exception& error) {
        ok=false;
        opennow::LogAppLifecycleEvent("CATALOG_COVER_LOAD_EXCEPTION",error.what());
    } catch(...) {
        ok=false;
        opennow::LogAppLifecycleEvent("CATALOG_COVER_LOAD_EXCEPTION","unknown");
    }
    if(job->generation==catalogCoverGeneration.load()) {
        if(ok) {
            std::lock_guard<std::mutex> lock(catalogCoverMutex);
            catalogCoverPixels=std::move(pixels); catalogCoverWidth=width; catalogCoverHeight=height;
            catalogCoverState.store(2);
        } else catalogCoverState.store(3);
    }
    catalogCoverJobDone.store(true);
    return 0;
}

// =============================================================================================
// CODIGOS QR DE CONTACTO (SOLO PARA LA PANTALLA "ACERCA DE")
// =============================================================================================
// Se cargan de /app0/assets/misc/*.png, junto al resto de recursos. Van ahi y no en una carpeta
// propia porque `create-gp4.exe` construye el arbol de directorios del PKG a partir de una lista
// FIJA (audio, fonts, images, misc, videos): una carpeta nueva nunca se declara, y el empaquetado
// falla al resolver el directorio padre del fichero.
//
// Tres decisiones que importan para el RENDIMIENTO:
//
//  1. SE CARGAN UNA SOLA VEZ, en un HILO DE FONDO al arrancar. La decodificacion de un PNG grande
//     cuesta decenas de milisegundos; hacerlo en el hilo de la interfaz clavaria el menu.
//  2. SE GUARDAN COMO SDL_Texture, no como pixeles. Dibujar una textura es UNA llamada; volver a
//     subir los pixeles en cada frame costaria megabytes por segundo.
//  3. SON UNOS 480x480 tras el decodificado (el decodificador reduce). Se dibujan a ~180 px, asi que
//     sobra resolucion y la memoria es de ~0,9 MB por QR.
// Estructura de un QR ya convertido a RGBA crudo en tiempo de compilacion.
//
// POR QUE RGBA CRUDO Y NO PNG
// ---------------------------
// La primera version leia los PNG y los pasaba a `decodeCatalogJpeg`, que internamente hace
// `avcodec_find_decoder(AV_CODEC_ID_PNG)`. En la consola salia SIEMPRE esto:
//
//     ABOUT_QR_LOAD_FAILED tiktoklink.png bytes=147635
//     ABOUT_QR_LOAD_FAILED youtubelink.png bytes=233696
//     ABOUT_QR_LOAD_FAILED miweblink.png bytes=156805
//
// Fijate en que los BYTES SON CORRECTOS: el fichero se leia bien. Lo que fallaba era el DECODIFICADOR.
// La causa esta comprobada en `build/ffmpeg-ps4/config_components.h`: este FFmpeg esta compilado con UN
// UNICO decodificador.
//
//     #define CONFIG_H264_DECODER 1      <-- el unico que hay
//     #define CONFIG_PNG_DECODER 0
//     #define CONFIG_MJPEG_DECODER 0
//     #define CONFIG_BMP_DECODER 0
//
// O sea: `avcodec_find_decoder` devolvia NULL y la funcion salia en su primera linea. De ahi los 0-3 ms
// del log. (Por el mismo motivo las caratulas del catalogo nunca se han decodificado: no hay NI UN
// decodificador de imagen en el binario.)
//
// LA SOLUCION: las imagenes se convierten a RGBA crudo EN TIEMPO DE COMPILACION. El script de build las
// redimensiona al tamano EXACTO al que se dibujan y escribe una cabecera de 8 bytes mas los pixeles. En
// la consola solo queda leer el fichero y subirlo a una textura.
//
// Ventajas frente a meter un decodificador PNG en el binario:
//   - CERO dependencias nuevas y cero codigo de terceros que mantener.
//   - CERO tiempo de decodificacion en la consola.
//   - El tamano en el PKG BAJA: los tres PNG suman 525 KB y llevan pixeles que nunca se mostrarian.
//   - Un QR necesita bordes NITIDOS: sin decodificador no hay artefactos de compresion que puedan
//     romper los modulos y hacer que el lector no lo reconozca.
//
// Formato del fichero `.rgba`:
//     bytes 0..3 : ancho (uint32 little-endian)
//     bytes 4..7 : alto  (uint32 little-endian)
//     bytes 8..  : pixeles RGBA8888, fila por fila, sin relleno
struct QrImage {
    const char* file;      // nombre del fichero .rgba en /app0/assets/misc/
    const char* caption;   // titulo que se dibuja ENCIMA del QR
    SDL_Texture* texture;  // se rellena cuando termina el hilo
    bool failed;
};
static QrImage qrImages[3]={
    {"tiktok_qr.rgba","TIKTOK",nullptr,false},
    {"youtube_qr.rgba","YOUTUBE",nullptr,false},
    {"miweb_qr.rgba","WEB",nullptr,false},
};
static std::mutex qrMutex;

// Lee un entero de 32 bits little-endian, byte a byte: no depende de la alineacion ni del orden de
// bytes de la maquina.
static uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1])<<8) |
           (static_cast<uint32_t>(p[2])<<16) | (static_cast<uint32_t>(p[3])<<24);
}

static int loadQrThread(void* data) {
    QrImage* qr=static_cast<QrImage*>(data);
    std::vector<uint8_t> bytes;
    const std::string path=std::string("/app0/assets/misc/")+qr->file;
    FILE* file=fopen(path.c_str(),"rb");
    if(file) {
        if(fseek(file,0,SEEK_END)==0) {
            const long size=ftell(file);
            if(size>=8 && size<=4*1024*1024 && fseek(file,0,SEEK_SET)==0) {
                bytes.resize(static_cast<size_t>(size));
                if(fread(bytes.data(),1,bytes.size(),file)!=bytes.size()) bytes.clear();
            }
        }
        fclose(file);
    }

    bool ok=false;
    int width=0,height=0;
    std::vector<uint8_t> pixels;
    if(bytes.size()>=8) {
        width=static_cast<int>(readLe32(bytes.data()));
        height=static_cast<int>(readLe32(bytes.data()+4));
        const size_t esperado=static_cast<size_t>(width)*static_cast<size_t>(height)*4u;
        // VALIDACION DEL TAMANO: si la cabecera no cuadra con el numero de bytes, el fichero esta mal
        // y NO se sube una textura a medias (que dibujaria basura o leería fuera del buffer).
        if(width>0 && height>0 && width<=512 && height<=512 && bytes.size()>=8+esperado) {
            pixels.assign(bytes.begin()+8,bytes.begin()+8+esperado);
            ok=true;
        }
    }

    if(ok) {
        std::lock_guard<std::mutex> lock(qrMutex);
        qr->texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,width,height);
        if(qr->texture) {
            SDL_UpdateTexture(qr->texture,nullptr,pixels.data(),width*4);
            SDL_SetTextureBlendMode(qr->texture,SDL_BLENDMODE_BLEND);
            // No se llama a SDL_SetTextureScaleMode: esta version de SDL2 para Orbis no la expone. La
            // textura ya viene al tamano al que se dibuja, asi que apenas se reescala.
        }
    }
    if(!ok || !qr->texture) {
        qr->failed=true;
        opennow::LogAppLifecycleEvent("ABOUT_QR_LOAD_FAILED",
            (std::string(qr->file)+" bytes="+std::to_string(bytes.size())+
             " cabecera="+std::to_string(width)+"x"+std::to_string(height)).c_str());
    } else {
        opennow::LogAppLifecycleEvent("ABOUT_QR_LOADED",
            (std::string(qr->file)+" px="+std::to_string(width)+"x"+std::to_string(height)+
             " bytes="+std::to_string(bytes.size())).c_str());
    }
    return 0;
}

// Handles de los tres hilos de carga. Se GUARDAN para poder esperarlos al cerrar.
//
// POR QUE NO SE USAN DETACHED: con hilos desprendidos, `freeQrTextures()` podia destruir una textura
// MIENTRAS el hilo todavia la estaba creando. La ventana es estrecha (solo si se cierra la app en los
// primeros milisegundos, mientras se decodifica el PNG) pero es una carrera real, y en una consola una
// carrera de este tipo se manifiesta como un cierre sin traza. Guardando el handle se espera y se
// elimina el problema.
static SDL_Thread* qrThreads[3]={nullptr,nullptr,nullptr};

// Lanza la carga de los tres QR en hilos de fondo. Se llama UNA vez, al arrancar la aplicacion: si se
// hiciera al entrar en "ACERCA DE" habria un tiron visible la primera vez.
static void startQrLoading() {
    for(size_t i=0;i<3;i++) {
        qrThreads[i]=SDL_CreateThread(loadQrThread,"gfn-qr",&qrImages[i]);
        if(!qrThreads[i]) {
            qrImages[i].failed=true;
            opennow::LogAppLifecycleEvent("ABOUT_QR_THREAD_FAILED",qrImages[i].file);
        }
    }
}

// Libera las texturas de los QR. Se llama al cerrar la aplicacion: una textura de SDL NO se libera
// sola, y no hacerlo deja memoria retenida hasta que el proceso muere.
static void freeQrTextures() {
    // PRIMERO se espera a que terminen los hilos (si alguno sigue decodificando), y solo despues se
    // destruyen las texturas. Al reves habria carrera con la creacion.
    for(size_t i=0;i<3;i++) {
        if(qrThreads[i]) { SDL_WaitThread(qrThreads[i],nullptr); qrThreads[i]=nullptr; }
    }
    std::lock_guard<std::mutex> lock(qrMutex);
    for(QrImage& qr:qrImages) {
        if(qr.texture) { SDL_DestroyTexture(qr.texture); qr.texture=nullptr; }
    }
}

static void maintainCatalogCover() {
    std::string wanted;
    const int count=catalogVisibleCount;
    if(page==3 && catalogState.load()==2 && count>0 && catalogSelection>=0 && catalogSelection<count) {
        const int index=catalogVisible[catalogSelection];
        wanted=catalogGames[index].imageUrl;
    } else if(page==6) wanted=launchCoverUrl;
    if(wanted!=catalogCoverDesiredUrl) {
        catalogCoverDesiredUrl=wanted;
        catalogCoverSelectionChangedAt=SDL_GetTicks();
        catalogCoverLoadedUrl.clear();
        catalogCoverGeneration.fetch_add(1);
        catalogCoverState.store(0);
        if(catalogCoverTexture) { SDL_DestroyTexture(catalogCoverTexture); catalogCoverTexture=nullptr; }
    }
    if(catalogCoverThread && catalogCoverJobDone.load()) {
        SDL_WaitThread(catalogCoverThread,nullptr); catalogCoverThread=nullptr;
        if(catalogCoverDesiredUrl==wanted && catalogCoverState.load()==2) catalogCoverLoadedUrl=wanted;
    }
    const bool catalogSelectionSettled=page!=3 ||
        static_cast<Uint32>(SDL_GetTicks()-catalogCoverSelectionChangedAt)>=250u;
    if(!catalogCoverThread && catalogSelectionSettled && !wanted.empty() && catalogCoverLoadedUrl!=wanted && catalogCoverState.load()!=3) {
        catalogCoverJobDone.store(false); catalogCoverState.store(1);
        auto* job=new CatalogCoverJob{wanted,catalogCoverGeneration.load()};
        catalogCoverThread=SDL_CreateThreadWithStackSize(
            loadCatalogCover,"gfn-cover",kNetworkWorkerStackSize,job);
        if(!catalogCoverThread) { delete job; catalogCoverState.store(3); }
    }
    if(catalogCoverState.load()==2 && !catalogCoverTexture) {
        std::lock_guard<std::mutex> lock(catalogCoverMutex);
        if(!catalogCoverPixels.empty() && catalogCoverWidth>0 && catalogCoverHeight>0) {
            catalogCoverTexture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,catalogCoverWidth,catalogCoverHeight);
            if(catalogCoverTexture) SDL_UpdateTexture(catalogCoverTexture,nullptr,catalogCoverPixels.data(),catalogCoverWidth*4);
        }
    }
}

static std::string upperAscii(std::string value) {
    for(char& c:value) if(c>='a' && c<='z') c=static_cast<char>(c-('a'-'A'));
    return value;
}
static std::string catalogFavoriteKey(const std::string& title,const std::string& store) {
    std::string key=upperAscii(store);
    key.push_back('\x1f');
    key+=upperAscii(title);
    return key;
}
static void rebuildCatalogFavoriteIndex() {
    catalogFavoriteIndex.clear();
    catalogFavoriteIndex.reserve(catalogFavorites.size());
    for(const auto& favorite:catalogFavorites)
        catalogFavoriteIndex.insert(catalogFavoriteKey(favorite.title,favorite.store));
}
static void rebuildVisibleCatalog() {
    catalogVisibleCount=0;
    const std::string query=upperAscii(catalogSearch);
    const int count=std::min(catalogCount.load(),2400);
    for(int i=0;i<count;++i) {
        const std::string store=upperAscii(catalogGames[i].store);
        const std::string title=upperAscii(catalogGames[i].title);
        const bool matchesQuery=query.empty() || title.find(query)!=std::string::npos;
        bool matchesStore=true;
        if(catalogStoreFilter==0) matchesStore=catalogGames[i].linked!=0;
        else if(catalogStoreFilter==2) matchesStore=store.find("STEAM")!=std::string::npos;
        else if(catalogStoreFilter==3) matchesStore=store.find("EPIC")!=std::string::npos;
        else if(catalogStoreFilter==4) matchesStore=store.find("UBI")!=std::string::npos || store.find("UPLAY")!=std::string::npos;
        else if(catalogStoreFilter==5) matchesStore=store.find("STEAM")==std::string::npos && store.find("EPIC")==std::string::npos && store.find("UBI")==std::string::npos && store.find("UPLAY")==std::string::npos;
        const bool favorite=catalogFavoriteIndex.find(store+"\x1f"+title)!=catalogFavoriteIndex.end();
        if(catalogStoreFilter==6) matchesStore=favorite;
        if(matchesQuery && matchesStore) catalogVisible[catalogVisibleCount++]=i;
    }
    if(catalogVisibleCount==0) catalogSelection=catalogOffset=0;
    else {
        catalogSelection=std::min(catalogSelection,catalogVisibleCount-1);
        catalogOffset=std::min(catalogOffset,catalogSelection);
        if(catalogSelection>=catalogOffset+9) catalogOffset=catalogSelection-8;
    }
}
static void publishPendingCatalog() {
    if(!pendingCatalogReady.exchange(false,std::memory_order_acquire)) return;
    const int count=std::min(pendingCatalogCount.load(std::memory_order_relaxed),2400);
    if(count<=0) return;
    memcpy(catalogGames,pendingCatalogGames,static_cast<size_t>(count)*sizeof(CatalogGame));
    catalogFullLoaded.store(pendingCatalogIsFull.load(std::memory_order_acquire),std::memory_order_release);
    catalogCount.store(count,std::memory_order_release);
    rebuildVisibleCatalog();
    catalogHttpStatus.store(200);
    catalogError.store(0);
    catalogState.store(2,std::memory_order_release);
    opennow::LogAppLifecycleEvent("CATALOG_READY",("games="+std::to_string(count)+" published=ui").c_str());
}
static void cycleCatalogFilter(int direction=1) {
    catalogStoreFilter=(catalogStoreFilter+direction+7)%7;
    rebuildVisibleCatalog();
    opennow::LogAppLifecycleEvent("CATALOG_FILTER",catalogFilters[catalogStoreFilter]);
    opennow::TraceAppAction("CATALOG_FILTER",catalogFilters[catalogStoreFilter]);
    if(page==3 && catalogStoreFilter==1 && !catalogFullLoaded.load(std::memory_order_acquire)) {
        catalogFullRequested.store(true,std::memory_order_release);
        startCatalogFetch();
    }
}
static void loadCatalogFavorites() {
    catalogFavorites.clear();
    FILE* file=fopen("/data/gfnps4/favorites.cfg","r");
    if(!file) return;
    char line[224];
    while(fgets(line,sizeof(line),file)) {
        size_t length=strlen(line);
        while(length && (line[length-1]=='\r' || line[length-1]=='\n')) line[--length]='\0';
        char* separator=strchr(line,'\t');
        if(!separator || separator==line || !separator[1]) continue;
        *separator='\0';
        CatalogFavorite favorite{line,separator+1};
        const bool exists=std::any_of(catalogFavorites.begin(),catalogFavorites.end(),[&](const CatalogFavorite& current) {
            return upperAscii(current.title)==upperAscii(favorite.title) && upperAscii(current.store)==upperAscii(favorite.store);
        });
        if(!exists && catalogFavorites.size()<2400) catalogFavorites.push_back(std::move(favorite));
    }
    fclose(file);
    rebuildCatalogFavoriteIndex();
    opennow::LogAppLifecycleEvent("FAVORITES_LOADED",std::to_string(catalogFavorites.size()).c_str());
}
static bool saveCatalogFavorites() {
    mkdir("/data/gfnps4",0777);
    FILE* file=fopen("/data/gfnps4/favorites.tmp","w");
    if(!file) return false;
    bool ok=true;
    for(const auto& favorite:catalogFavorites) {
        if(fprintf(file,"%s\t%s\n",favorite.title.c_str(),favorite.store.c_str())<0) { ok=false; break; }
    }
    if(fflush(file)!=0) ok=false;
    if(fsync(fileno(file))!=0) ok=false;
    if(fclose(file)!=0) ok=false;
    if(!ok || rename("/data/gfnps4/favorites.tmp","/data/gfnps4/favorites.cfg")!=0) return false;
    return true;
}
static bool catalogGameIsFavorite(int gameIndex) {
    if(gameIndex<0 || gameIndex>=catalogCount.load()) return false;
    return catalogFavoriteIndex.find(catalogFavoriteKey(catalogGames[gameIndex].title,catalogGames[gameIndex].store))
        !=catalogFavoriteIndex.end();
}
static void toggleSelectedCatalogFavorite() {
    if(catalogSelection<0 || catalogSelection>=catalogVisibleCount) return;
    const int gameIndex=catalogVisible[catalogSelection];
    if(gameIndex<0 || gameIndex>=catalogCount.load()) return;
    const std::string title=upperAscii(catalogGames[gameIndex].title);
    const std::string store=upperAscii(catalogGames[gameIndex].store);
    auto it=std::find_if(catalogFavorites.begin(),catalogFavorites.end(),[&](const CatalogFavorite& favorite) {
        return upperAscii(favorite.title)==title && upperAscii(favorite.store)==store;
    });
    const bool added=it==catalogFavorites.end();
    opennow::TraceAppAction("CATALOG_FAVORITE",added?"add":"remove");
    if(added) catalogFavorites.push_back({catalogGames[gameIndex].title,catalogGames[gameIndex].store});
    else catalogFavorites.erase(it);
    if(!saveCatalogFavorites()) {
        if(added) {
            catalogFavorites.pop_back();
        } else {
            catalogFavorites.push_back({catalogGames[gameIndex].title,catalogGames[gameIndex].store});
        }
        rebuildCatalogFavoriteIndex();
        opennow::LogAppLifecycleEvent("FAVORITE_SAVE_FAILED",added?"add":"remove");
        rebuildVisibleCatalog();
        return;
    }
    rebuildCatalogFavoriteIndex();
    opennow::LogAppLifecycleEvent("FAVORITE_CHANGED",added?"added":"removed");
    rebuildVisibleCatalog();
}
static void toggleCatalogSearch() {
    catalogSearchActive=!catalogSearchActive;
    if(catalogSearchActive) SDL_StartTextInput(); else SDL_StopTextInput();
    opennow::LogAppLifecycleEvent("CATALOG_SEARCH",catalogSearchActive?"active":"inactive");
    rebuildVisibleCatalog();
}
static void utf8ToU16(const std::string& input,uint16_t* output,size_t capacity) {
    size_t out=0; const unsigned char* p=reinterpret_cast<const unsigned char*>(input.c_str());
    while(*p && out+1<capacity) {
        uint32_t cp;
        if(*p<0x80) cp=*p++;
        else if((*p&0xe0)==0xc0 && p[1]) { cp=((p[0]&31)<<6)|(p[1]&63); p+=2; }
        else if((*p&0xf0)==0xe0 && p[1]&&p[2]) { cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3; }
        else if((*p&0xf8)==0xf0 && p[1]&&p[2]&&p[3]) { cp=((p[0]&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63); p+=4; }
        else { cp='?'; ++p; }
        if(cp>0xffff) { if(out+2>=capacity) break; cp-=0x10000; output[out++]=static_cast<uint16_t>(0xd800+(cp>>10)); output[out++]=static_cast<uint16_t>(0xdc00+(cp&0x3ff)); }
        else output[out++]=static_cast<uint16_t>(cp);
    }
    output[out]=0;
}
static std::string u16ToUtf8(const uint16_t* input) {
    std::string out;
    for(size_t i=0;input[i];i++) {
        uint32_t cp=input[i];
        if(cp>=0xd800 && cp<=0xdbff && input[i+1]>=0xdc00 && input[i+1]<=0xdfff) { cp=0x10000+((cp-0xd800)<<10)+(input[++i]-0xdc00); }
        if(cp<0x80) out.push_back(static_cast<char>(cp));
        else if(cp<0x800) { out.push_back(static_cast<char>(0xc0|(cp>>6))); out.push_back(static_cast<char>(0x80|(cp&63))); }
        else if(cp<0x10000) { out.push_back(static_cast<char>(0xe0|(cp>>12))); out.push_back(static_cast<char>(0x80|((cp>>6)&63))); out.push_back(static_cast<char>(0x80|(cp&63))); }
        else { out.push_back(static_cast<char>(0xf0|(cp>>18))); out.push_back(static_cast<char>(0x80|((cp>>12)&63))); out.push_back(static_cast<char>(0x80|((cp>>6)&63))); out.push_back(static_cast<char>(0x80|(cp&63))); }
    }
    return out;
}
static void openCatalogSearchKeyboard() {
    catalogSearchActive=true; SDL_StartTextInput();
    uint16_t input[256]={},title[96]={},placeholder[64]={};
    utf8ToU16(catalogSearch,input,256);
    utf8ToU16(language==0?"Buscar juego":"Search games",title,96);
    utf8ToU16(language==0?"Nombre del juego":"Game title",placeholder,64);
    OrbisImeDialogSetting setting{};
    int userId=0; sceUserServiceGetInitialUser(&userId);
    setting.userId=static_cast<uint32_t>(userId); setting.type=ORBIS_TYPE_DEFAULT; setting.enterLabel=ORBIS_BUTTON_LABEL_SEARCH;
    setting.inputMethod=ORBIS__DEFAULT; setting.maxTextLength=255; setting.inputTextBuffer=reinterpret_cast<wchar_t*>(input);
    // Orbis' dialog position is absolute. Centering with (0,0) anchors it off-screen on some PS4 builds.
    setting.posx=static_cast<float>(W)/2.0f; setting.posy=static_cast<float>(H)/2.0f;
    setting.horizontalAlignment=ORBIS_H_CENTER; setting.verticalAlignment=ORBIS_V_CENTER;
    setting.title=reinterpret_cast<const wchar_t*>(title); setting.placeholder=reinterpret_cast<const wchar_t*>(placeholder);
    const int rc=sceImeDialogInit(&setting,nullptr);
    if(rc!=0) { opennow::LogAppLifecycleEvent("CATALOG_IME_FAILED",("rc="+std::to_string(rc)).c_str()); return; }
    while(sceImeDialogGetStatus()==ORBIS_DIALOG_STATUS_RUNNING) SDL_Delay(16);
    OrbisDialogResult result{}; sceImeDialogGetResult(&result); sceImeDialogTerm();
    if(result.endstatus==ORBIS_DIALOG_OK) { catalogSearch=u16ToUtf8(input); rebuildVisibleCatalog(); opennow::LogAppLifecycleEvent("CATALOG_IME_ACCEPTED",("length="+std::to_string(catalogSearch.size())).c_str()); }
    else opennow::LogAppLifecycleEvent("CATALOG_IME_CANCELLED");
}
static void editCatalogSearch(const char* input) {
    if(!catalogSearchActive || !input) return;
    const size_t oldLength=catalogSearch.size();
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(input);*p;++p) {
        if(*p>=32 && *p<127 && catalogSearch.size()<60) catalogSearch.push_back(static_cast<char>(*p));
    }
    if(catalogSearch.size()!=oldLength) {
        opennow::LogAppLifecycleEvent("CATALOG_SEARCH_CHANGED",("length="+std::to_string(catalogSearch.size())).c_str());
        opennow::TraceAppAction("CATALOG_SEARCH_TEXT","changed; content omitted for privacy");
    }
    rebuildVisibleCatalog();
}
static void eraseCatalogSearchChar() {
    if(catalogSearchActive && !catalogSearch.empty()) { catalogSearch.pop_back(); opennow::LogAppLifecycleEvent("CATALOG_SEARCH_CHANGED",("length="+std::to_string(catalogSearch.size())).c_str()); rebuildVisibleCatalog(); }
}

struct RGB { Uint8 r, g, b; };
static RGB color(Uint8 r, Uint8 g, Uint8 b) { RGB c = {r,g,b}; return c; }
static bool mapSdlKey(SDL_Keycode key,Uint16 modifiers,opennow::input::KeyboardStroke& stroke) {
    const Uint16 shiftMask=static_cast<Uint16>(KMOD_LSHIFT|KMOD_RSHIFT);
    const Uint16 ctrlMask=static_cast<Uint16>(KMOD_LCTRL|KMOD_RCTRL);
    const Uint16 altMask=static_cast<Uint16>(KMOD_LALT|KMOD_RALT);
    const Uint16 metaMask=static_cast<Uint16>(KMOD_LGUI|KMOD_RGUI);
    if(key>=SDLK_a && key<=SDLK_z) {
        if(!opennow::input::MapAsciiKey(static_cast<char>(key),stroke)) return false;
    } else if(key>=SDLK_0 && key<=SDLK_9) {
        if(!opennow::input::MapAsciiKey(static_cast<char>(key),stroke)) return false;
    } else if(key>=32 && key<=126) {
        if(!opennow::input::MapAsciiKey(static_cast<char>(key),stroke)) return false;
    } else if(key>=SDLK_F1 && key<=SDLK_F12) {
        const int n=static_cast<int>(key-SDLK_F1);
        static const Uint8 scans[12]={0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43,0x44,0x57,0x58};
        stroke={static_cast<Uint16>(0x70+n),scans[n],0};
    } else {
        switch(key) {
        case SDLK_RETURN: case SDLK_KP_ENTER: stroke={0x0d,0x1c,0}; break;
        case SDLK_ESCAPE: stroke={0x1b,0x01,0}; break;
        case SDLK_BACKSPACE: stroke={0x08,0x0e,0}; break;
        case SDLK_SPACE: stroke={0x20,0x39,0}; break;
        case SDLK_TAB: stroke={0x09,0x0f,0}; break;
        case SDLK_DELETE: stroke={0x2e,0x53,0}; break;
        case SDLK_INSERT: stroke={0x2d,0x52,0}; break;
        case SDLK_HOME: stroke={0x24,0x47,0}; break;
        case SDLK_END: stroke={0x23,0x4f,0}; break;
        case SDLK_PAGEUP: stroke={0x21,0x49,0}; break;
        case SDLK_PAGEDOWN: stroke={0x22,0x51,0}; break;
        case SDLK_LEFT: stroke={0x25,0x4b,0}; break;
        case SDLK_UP: stroke={0x26,0x48,0}; break;
        case SDLK_RIGHT: stroke={0x27,0x4d,0}; break;
        case SDLK_DOWN: stroke={0x28,0x50,0}; break;
        case SDLK_LSHIFT: case SDLK_RSHIFT: stroke={0x10,0x2a,0x0001}; break;
        case SDLK_LCTRL: case SDLK_RCTRL: stroke={0x11,0x1d,0x0002}; break;
        case SDLK_LALT: case SDLK_RALT: stroke={0x12,0x38,0x0004}; break;
        case SDLK_LGUI: case SDLK_RGUI: stroke={0x5b,0x5b,0x0008}; break;
        default: return false;
        }
    }
    if(modifiers&shiftMask) stroke.modifiers|=0x0001;
    if(modifiers&ctrlMask) stroke.modifiers|=0x0002;
    if(modifiers&altMask) stroke.modifiers|=0x0004;
    if(modifiers&metaMask) stroke.modifiers|=0x0008;
    return true;
}

static const unsigned char* glyph(char c) {
    static const unsigned char blank[7] = {0,0,0,0,0,0,0};
    #define G(ch,a,b,c,d,e,f,g) case ch: { static const unsigned char v[7]={a,b,c,d,e,f,g}; return v; }
    switch (c) {
    G('A',14,17,17,31,17,17,17) G('B',30,17,17,30,17,17,30)
    G('C',14,17,16,16,16,17,14) G('D',30,17,17,17,17,17,30)
    G('E',31,16,16,30,16,16,31) G('F',31,16,16,30,16,16,16)
    G('G',14,17,16,23,17,17,15) G('H',17,17,17,31,17,17,17)
    G('I',14,4,4,4,4,4,14) G('J',7,2,2,2,18,18,12)
    G('K',17,18,20,24,20,18,17) G('L',16,16,16,16,16,16,31)
    G('M',17,27,21,21,17,17,17) G('N',17,25,21,19,17,17,17)
    G('O',14,17,17,17,17,17,14) G('P',30,17,17,30,16,16,16)
    G('Q',14,17,17,17,21,18,13) G('R',30,17,17,30,20,18,17)
    G('S',15,16,16,14,1,1,30) G('T',31,4,4,4,4,4,4)
    G('U',17,17,17,17,17,17,14) G('V',17,17,17,17,17,10,4)
    G('W',17,17,17,21,21,21,10) G('X',17,17,10,4,10,17,17)
    G('Y',17,17,10,4,4,4,4) G('Z',31,1,2,4,8,16,31)
    G('0',14,17,19,21,25,17,14) G('1',4,12,4,4,4,4,14)
    G('2',14,17,1,2,4,8,31) G('3',30,1,1,14,1,1,30)
    G('4',2,6,10,18,31,2,2) G('5',31,16,16,30,1,1,30)
    G('6',14,16,16,30,17,17,14) G('7',31,1,2,4,8,8,8)
    G('8',14,17,17,14,17,17,14) G('9',14,17,17,15,1,1,14)
    G('.',0,0,0,0,0,12,12) G(',',0,0,0,0,4,4,8)
    G(':',0,12,12,0,12,12,0) G('-',0,0,0,31,0,0,0)
    G('/',1,2,2,4,8,8,16) G('+',0,4,4,31,4,4,0)
    G('%',17,2,4,8,17,0,0) G('!',4,4,4,4,4,0,4)
    G('?',14,17,1,2,4,0,4) G('=',0,31,0,31,0,0,0)
    G('(',2,4,8,8,8,4,2) G(')',8,4,2,2,2,4,8)
    default: return blank;
    }
    #undef G
}

static void fill(int x,int y,int w,int h,RGB c) {
    ++g_frameRects; ++g_frameFills;
    SDL_Rect r = {x,y,w,h}; SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,255); SDL_RenderFillRect(renderer,&r);
}
static void drawLoadingBeam(int x,int y,int width,int height) {
    fill(x,y,width,height,color(35,47,63));
    const int beamWidth=std::min(240,width);
    const int travel=std::max(1,width-beamWidth);
    const int phase=static_cast<int>((SDL_GetTicks()/5)%(2*travel));
    const int beamX=x+(phase<=travel?phase:2*travel-phase);
    const int shadowLeft=std::max(x,beamX-22);
    const int shadowRight=std::min(x+width,beamX+beamWidth+22);
    fill(shadowLeft,y-2,shadowRight-shadowLeft,height+4,color(43,120,44));
    fill(beamX,y-1,beamWidth,height+2,color(118,255,66));
    fill(beamX+26,y,beamWidth-52,height,color(190,255,161));
}
static void outline(int x,int y,int w,int h,RGB c) {
    SDL_Rect r = {x,y,w,h}; SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,255); SDL_RenderDrawRect(renderer,&r);
}
// Rectangulo relleno con TRANSPARENCIA. `fill` escribe alpha 255 (opaco); para sombras suaves y
// veladuras hace falta poder componer, y eso es lo que aporta esta variante.
// Coste: el mismo que `fill` (una llamada a SDL_RenderFillRect); el alpha lo resuelve el mezclador
// del renderizador, no la CPU.
// Rectangulo relleno con TRANSPARENCIA.
//
// =============================================================================================
// ESTE ERA EL BUG DEL "PARPADEO NEGRO" — Y LUEGO EL DE LOS 19 FPS
// =============================================================================================
// PRIMERA VERSION (mal): ponia el alpha en `SDL_SetRenderDrawColor` pero NO habilitaba el modo de
// mezcla. Con `SDL_BLENDMODE_NONE`, SDL IGNORA el alpha y escribe el color tal cual: el velo de
// transicion (alpha 232) se pintaba como NEGRO OPACO. Ese fue el pantallazo negro.
//
// SEGUNDA VERSION (tambien mal, y peor): habilite `SDL_BLENDMODE_BLEND` en cada primitiva. Eso
// arreglo el color, pero **el renderizador de PS4 es SOFTWARE** (`VIDEO_RENDERER_READY name=software
// mode=software` en el log) y la mezcla por pixel en CPU es carisima:
//
//     +3932ms UI_LOOP_PERF loop_fps=19 render_max_ms=51 slow_frames=90    <-- antes: 59 fps
//
// Estaba pintando con alpha DOS rectangulos a pantalla completa POR FRAME:
//
//     fillAlpha(0,0,W,200,...)   -> 1920 x 200  =   384.000 pixeles mezclados
//     fillAlpha(0,0,W,H,...)     -> 1920 x 1080 = 2.073.600 pixeles mezclados
//
// Mas `SDL_GetRenderDrawBlendMode` y `SDL_SetRenderDrawBlendMode` en CADA llamada (17 en el fuente).
//
// ---------------------------------------------------------------------------------------------
// LA SOLUCION: BANDAS OPACAS DEGRADADAS
// ---------------------------------------------------------------------------------------------
// Se pinta el rectangulo como una serie de BANDAS HORIZONTALES OPACAS, cada vez mas claras, en lugar
// de mezclar cada pixel. **Sin mezcla y con un numero FIJO de llamadas** (kBandas), independiente del
// tamano del rectangulo.
//
// HISTORIA DE TRES INTENTOS, para no repetir ninguno:
//   1. `SDL_BLENDMODE_NONE` con alpha en el color -> SDL IGNORA el alpha y pinta OPACO. El velo de
//      transicion salio NEGRO ENTERO: el pantallazo negro reportado.
//   2. `SDL_BLENDMODE_BLEND` en cada primitiva -> colores correctos, pero **el renderizador de PS4 es
//      SOFTWARE** y la mezcla se paga pixel a pixel. Pintaba DOS rectangulos a pantalla completa por
//      frame (384.000 + 2.073.600 px mezclados) y los FPS cayeron de 59 a 19.
//   3. Tramado (una linea si, otra no) -> sin mezcla, pero generaba **1.920 llamadas** a
//      SDL_RenderFillRect para el velo de pantalla completa. El coste por llamada del renderizador
//      software se come la ganancia.
//
// AHORA: kBandas llamadas fijas, opacas, con el color interpolado hacia el fondo segun el alpha.
// Para un velo oscuro sobre fondo oscuro el resultado es indistinguible de una transparencia real,
// y el coste es CONSTANTE (no depende de w*h).
static void fillAlpha(int x,int y,int w,int h,RGB c,int alpha) {
    if(w<=0 || h<=0) return;
    if(alpha<=0) return;
    ++g_frameRects;
    if(alpha>255) alpha=255;

    // Numero de bandas. Con 8 el escalonado no se aprecia en un velo oscuro; se sube un poco para
    // rectangulos altos donde el degradado se notaria mas, pero acotado para que el coste sea fijo.
    //
    // AJUSTE TRAS MEDIR EN CONSOLA: estaba en 12 y el desglose por fases (`UI_DRAW_PHASES`) mostro
    // `header_us=8051`, casi la mitad del presupuesto de 16.666 us a 60 FPS. El velo de cabecera es
    // 1920x200 con 12 bandas = **12 llamadas a SDL_RenderFillRect por frame**, y cada llamada del
    // renderizador software tiene un coste fijo alto.
    //
    // Con 6 bandas el coste se REDUCE A LA MITAD y sobre un velo oscuro de 200 px de alto sobre fondo
    // igualmente oscuro la diferencia no se distingue: el escalonado solo se aprecia cuando hay un
    // salto de luminancia grande entre bandas, y aqui el salto total es de 16,22,33 a 8,11,17.
    const int kBandas = (h>=60) ? 6 : (h>=24) ? 3 : 1;

    if(kBandas<=1) {
        // Rectangulo pequeno: una sola banda opaca con el color ya atenuado.
        const int r=(c.r*alpha)>>8, g=(c.g*alpha)>>8, b=(c.b*alpha)>>8;
        SDL_Rect rect={x,y,w,h};
        SDL_SetRenderDrawColor(renderer,static_cast<Uint8>(r),static_cast<Uint8>(g),static_cast<Uint8>(b),255);
        SDL_RenderFillRect(renderer,&rect);
        return;
    }

    // Bandas opacas, de mas cobertura (arriba) a menos (abajo): simula un degradado.
    const int altoBase=h/kBandas;
    for(int i=0;i<kBandas;i++) {
        const int by=y+i*altoBase;
        const int bh=(i==kBandas-1)?(y+h-by):altoBase;
        if(bh<=0) break;
        // La ultima banda lleva la intensidad plena; la primera, una fraccion. Asi el borde superior
        // del velo queda mas suave y el inferior mas marcado, que es como se percibe una sombra.
        const int factor=alpha*(kBandas-i)/kBandas;
        const int r=(c.r*factor)>>8, g=(c.g*factor)>>8, b=(c.b*factor)>>8;
        SDL_Rect rect={x,by,w,bh};
        SDL_SetRenderDrawColor(renderer,static_cast<Uint8>(r),static_cast<Uint8>(g),static_cast<Uint8>(b),255);
        SDL_RenderFillRect(renderer,&rect);
    }
}
static std::string fontCacheKey(const char* s,int px) { return std::to_string(px)+":"+(s?s:""); }
static Uint32 nextUtf8(const unsigned char*& p) {
    if(*p<0x80) return *p++;
    if((*p&0xe0)==0xc0 && p[1]) { Uint32 c=((p[0]&31)<<6)|(p[1]&63); p+=2; return c; }
    if((*p&0xf0)==0xe0 && p[1] && p[2]) { Uint32 c=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3; return c; }
    if((*p&0xf8)==0xf0 && p[1] && p[2] && p[3]) { Uint32 c=((p[0]&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63); p+=4; return c; }
    ++p; return '?';
}
// Limite de la cache de texturas de texto.
//
// POR QUE NO SE BORRA TODA AL LLEGAR AL LIMITE
// --------------------------------------------
// La version anterior hacia esto:
//
//     if(textCache.size()>=512) { for(auto& i:textCache) SDL_DestroyTexture(i.second); textCache.clear(); }
//
// Funciona y no deja punteros colgantes (el `clear()` esta), pero **destruye las 512 texturas de golpe
// en el frame en que se cruza el limite**, y las que se siguen usando en pantalla hay que reconstruirlas
// inmediatamente despues: `stbtt_GetCodepointBitmap` por caracter, `SDL_CreateTextureFromSurface` por
// cadena. Eso es un PICO de trabajo dentro de un solo frame — exactamente el tipo de cosas que produce
// un tiron visible.
//
// Con 512 entradas es dificil llegar (la app tiene ~200 cadenas distintas), pero los textos del mando y
// del catalogo se construyen con numeros que cambian, asi que SI puede llegar.
//
// AHORA: al cruzar el limite se vacia UNA VEZ de forma controlada y se deja constancia en el log, para
// que si alguna vez ocurre se vea en el diagnostico en lugar de manifestarse como un tiron sin
// explicacion. Se conserva el `clear()` completo porque es la unica forma de garantizar que no quede
// ninguna textura huerfana, pero el aviso permite detectarlo.
static void evictTextCacheIfNeeded() {
    if(textCache.size() < 512) return;
    opennow::LogAppLifecycleEvent("UI_TEXT_CACHE_EVICT", "reason=size_limit count=512 rebuild_next_frame");
    for(auto& item : textCache) if(item.second) SDL_DestroyTexture(item.second);
    textCache.clear();
}
static SDL_Texture* makeTextTexture(const char* s,int px) {
    if(!uiFontReady || !s || !*s) return nullptr;
    const std::string key=fontCacheKey(s,px);
    auto found=textCache.find(key); if(found!=textCache.end()) return found->second;
    const float scale=stbtt_ScaleForPixelHeight(&uiFont,static_cast<float>(px));
    int ascent=0,descent=0,gap=0; stbtt_GetFontVMetrics(&uiFont,&ascent,&descent,&gap);
    const int baseline=static_cast<int>(std::ceil(ascent*scale));
    const unsigned char* p=reinterpret_cast<const unsigned char*>(s);
    std::vector<Uint32> cps; while(*p) cps.push_back(nextUtf8(p));
    int width=0;
    for(Uint32 cp:cps) { int advance=0,bearing=0; stbtt_GetCodepointHMetrics(&uiFont,static_cast<int>(cp),&advance,&bearing); width+=static_cast<int>(std::round(advance*scale)); }
    width=std::max(1,width+2); const int height=std::max(px+3,baseline-static_cast<int>(std::floor(descent*scale))+2);
    std::vector<Uint32> pixels(static_cast<size_t>(width)*height,0);
    int pen=1;
    for(Uint32 cp:cps) {
        int gw=0,gh=0,xoff=0,yoff=0,advance=0,bearing=0;
        unsigned char* bitmap=stbtt_GetCodepointBitmap(&uiFont,scale,scale,static_cast<int>(cp),&gw,&gh,&xoff,&yoff);
        stbtt_GetCodepointHMetrics(&uiFont,static_cast<int>(cp),&advance,&bearing);
        for(int yy=0;bitmap && yy<gh;yy++) for(int xx=0;xx<gw;xx++) {
            const int dx=pen+xoff+xx,dy=baseline+yoff+yy;
            if(dx>=0 && dx<width && dy>=0 && dy<height) pixels[static_cast<size_t>(dy)*width+dx]=(static_cast<Uint32>(bitmap[yy*gw+xx])<<24)|0x00ffffff;
        }
        if(bitmap) stbtt_FreeBitmap(bitmap,nullptr);
        pen+=static_cast<int>(std::round(advance*scale));
    }
    SDL_Surface* surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(),width,height,32,width*4,SDL_PIXELFORMAT_RGBA32);
    if(!surface) return nullptr;
    SDL_Texture* texture=SDL_CreateTextureFromSurface(renderer,surface); SDL_FreeSurface(surface);
    if(texture) {
        SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
        if(textCache.size()>=512) evictTextCacheIfNeeded();
        textCache.emplace(key,texture);
    }
    return texture;
}
// ---------------------------------------------------------------------------
// ESPACIO DE COORDENADAS DE LA INTERFAZ
//
// Todo el layout de la app esta disenado en coordenadas de 1920x1080 (hay 184 llamadas a
// label(), 30 a panel() y 26 a fill() con posiciones absolutas). Si la ventana SDL es mas
// pequena (por ejemplo 1280x720, que es lo que reporta el display cuando se guarda 720p),
// dos cosas se rompen a la vez:
//   - Los textos se dibujan con un tamano FIJO en pixeles (scale*8), asi que en una ventana
//     mas pequena ocupan proporcionalmente mucho mas: el sintoma es "todo se ve gigante".
//   - Las posiciones absolutas de 1080p se salen de la ventana y el contenido se corta.
//
// La solucion es decirle a SDL que el espacio logico es 1920x1080: SDL escala todo al tamano
// real de la ventana. Como el aspecto es el mismo (16:9), no hay barras negras y el layout
// queda identico al disenado, solo que a la resolucion del display.
//
// Es una sola llamada por renderer, y evita tener que tocar las 240 llamadas de dibujado.
static void applyLogicalSize() {
    if (!renderer) return;
    // Solo tiene sentido si la ventana difiere del espacio de diseno.
    if (W == 1920 && H == 1080) return;
    if (SDL_RenderSetLogicalSize(renderer, 1920, 1080) != 0) {
        opennow::LogAppLifecycleEvent("UI_LOGICAL_SIZE_FAIL", SDL_GetError());
        return;
    }
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "logical=1920x1080 window=%dx%d scale=%.4f", W, H,
                  static_cast<double>(W) / 1920.0);
    opennow::LogAppLifecycleEvent("UI_LOGICAL_SIZE", detail);
}
static void text(int x,int y,const char* s,int scale,RGB c) {
    // =============================================================================================
    // GUARDIAN DE ETIQUETA LENTA (v3.76)
    // =============================================================================================
    // POR QUE EXISTE: la v3.70 metio a la aplicacion en un camino patologico de SDL y **cada etiqueta
    // paso de ~200 us a 117.000 us** (250 veces mas). Eso dejo el bucle a 0 fps y la aplicacion murio,
    // y se detecto mirando una captura de pantalla — no por el log.
    //
    // Este guardian convierte ese sintoma en una linea de diagnostico: si UNA etiqueta tarda mas de
    // 20.000 us, se registra con su texto y su escala. Lo normal medido es 200-550 us, asi que 20.000
    // us son 40 veces lo normal y no hay falso positivo razonable.
    //
    // **No cambia el dibujado**: solo mide y registra. Un guardian que ademas saltara el dibujado
    // podria dejar la pantalla a medias, y eso es peor que un frame lento.
    const uint64_t t_text0 = getProcessTimeUs();
    const int px=std::max(11,scale*8);
    SDL_Texture* texture=makeTextTexture(s,px);
    ++g_frameLabels;
    if(!texture) {
        SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,255);
        for(;s && *s;++s,x+=6*scale) { const unsigned char* rows=glyph(*s>='a'&&*s<='z'?*s-32:*s); for(int yy=0;yy<7;yy++) for(int xx=0;xx<5;xx++) if(rows[yy]&(1<<(4-xx))) { SDL_Rect p={x+xx*scale,y+yy*scale,scale,scale}; SDL_RenderFillRect(renderer,&p); } }
        return;
    }
    // =============================================================================================
    // ESTA FUNCION SE ROMPIO EN LA v3.70 Y SE RESTAURO EN LA v3.73. EL FALLO, CON SUS NUMEROS.
    // =============================================================================================
    // En la v3.70 quite el `SDL_QueryTexture` y el destino explicito, dejando:
    //
    //     SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    //
    // razonando que "`dstrect = NULL` dibuja la textura a su tamanio en `(x,y)`". **La segunda mitad es
    // falsa: SDL usa `{0, 0, texture->w, texture->h}`, es decir la POSICION (0,0), no `(x,y)`.**
    //
    // El resultado en consola, medido en el log de la v3.70:
    //
    //     UI_DRAW_PHASES page=0 clear_us=2585 header_us=447921 content_us=2110174 footer_us=153757
    //     UI_LOOP_PERF  loop_fps=0 render_max_ms=2731 slow_frames=2
    //
    // **`content_us` = 2.110.174 us = 2,1 SEGUNDOS por frame**, donde antes eran ~9.800 us en total.
    // **Dibujar en (0,0) no solo pinta el texto en el sitio equivocado: mete a SDL en un camino
    // patologico** y cada etiqueta pasa de ~200 us a centenares de milisegundos. Sin frames, la captura
    // mostraba un lienzo a medio pintar (las rayas) y `FPS: 0.21`, y la aplicacion murio a los ~6 s en
    // `stage=18 name=DRAW_MENU_CONTENT beat=6`.
    //
    // Por eso esta funcion vuelve a ser EXACTAMENTE la que estaba probada en consola: consulta del
    // tamanio, destino explicito y mod de color para todos los colores. La micro-optimizacion del mod
    // blanco tambien se retira: **su ganancia no era medible y no merece dejar nada de la v3.70 vivo.**
    int w=0,h=0; SDL_QueryTexture(texture,nullptr,nullptr,&w,&h);
    SDL_SetTextureColorMod(texture,c.r,c.g,c.b); SDL_SetTextureAlphaMod(texture,255);
    SDL_Rect dst={x,y,w,h};
    SDL_RenderCopy(renderer,texture,nullptr,&dst);

    // Guardian: se registra como maximo una vez por segundo para no inundar el log en una racha.
    {
        const uint64_t t_text1 = getProcessTimeUs();
        const uint64_t cost = (t_text1 > t_text0) ? (t_text1 - t_text0) : 0;
        if(cost > 20000ULL) {
            static uint64_t s_lastSlowLabelUs = 0;
            if(t_text1 - s_lastSlowLabelUs >= 1000000ULL) {
                s_lastSlowLabelUs = t_text1;
                char sl[220];
                std::snprintf(sl,sizeof(sl),
                              "cost_us=%llu px=%d x=%d y=%d w=%d h=%d texto=%.64s",
                              (unsigned long long)cost, px, x, y, w, h, s ? s : "");
                opennow::LogAppLifecycleEvent("UI_SLOW_LABEL", sl);
            }
        }
    }
}
static void textAlpha(int x,int y,const char* s,int scale,RGB c,int alpha) {
    if(alpha<=0) return;
    const int px=std::max(11,scale*8);
    SDL_Texture* texture=makeTextTexture(s,px);
    if(!texture) return;
    ++g_frameLabels;
    // Restaurada a la version probada en consola (ver la explicacion del fallo de la v3.70 en `text()`).
    int w=0,h=0; SDL_QueryTexture(texture,nullptr,nullptr,&w,&h);
    SDL_SetTextureColorMod(texture,c.r,c.g,c.b);
    SDL_SetTextureAlphaMod(texture,static_cast<Uint8>(alpha>255?255:alpha));
    SDL_Rect dst={x,y,w,h};
    SDL_RenderCopy(renderer,texture,nullptr,&dst);
    SDL_SetTextureAlphaMod(texture,255);
}
// Dibuja la textura de un QR. `slot` indexa qrImages; si aun no ha cargado (o fallo) se dibuja un
// marcador para que el hueco no quede vacio ni descoloque el resto.
//
// POR QUE SE PINTA UNA TARJETA BLANCA DEBAJO: los QR son NEGROS SOBRE BLANCO. Sobre el fondo oscuro
// de la app el blanco desaparece y el lector no distingue los modulos. Y no vale dibujar el QR
// "recortando" el blanco para que se vea integrado: los lectores necesitan el margen claro alrededor
// (el "quiet zone"). Por eso cada QR va sobre su propia tarjeta blanca.
static void qrCard(int slot,int x,int y,int size) {
    if(slot<0 || slot>=3) return;
    const RGB cardColor=color(244,247,252);
    fill(x-8,y-8,size+16,size+16,cardColor);
    std::lock_guard<std::mutex> lock(qrMutex);
    QrImage& qr=qrImages[slot];
    if(qr.texture) {
        SDL_Rect dst={x,y,size,size};
        SDL_RenderCopy(renderer,qr.texture,nullptr,&dst);
        return;
    }
    // Marcador mientras carga (o si fallo): centrado DE VERDAD, midiendo el texto.
    //
    // Antes estaba en `x+10, y+size/2-6` a ojo, que con "QR NO DISPONIBLE" (mas largo) dejaba el
    // texto pegado a la izquierda. Se mide con la misma funcion que usa el motor para dibujar, asi
    // que el centrado es exacto y no depende de estimar el ancho de los caracteres.
    //
    // El texto va aqui literal, sin translateUi(): esa funcion se define MAS ABAJO en el fichero y
    // no es visible desde este punto.
    const char* message=qr.failed?"QR NO DISPONIBLE":"CARGANDO...";
    const int messagePx=11;   // misma escala que usa textAlpha con scale=1
    int messageW=0,messageH=0;
    if(SDL_Texture* messageTexture=makeTextTexture(message,messagePx)) {
        SDL_QueryTexture(messageTexture,nullptr,nullptr,&messageW,&messageH);
    }
    textAlpha(x+(size-messageW)/2,y+(size-messageH)/2,message,1,color(90,100,115),255);
}
static const char* translateUi(const char* text) {
    if(language!=1 || !text) return text;
    static const std::unordered_map<std::string,std::string> en={
        {"TU BIBLIOTECA, TU SESION, TU PERFIL DE STREAMING","YOUR LIBRARY, YOUR SESSION, YOUR STREAMING PROFILE"},
        {"CENTRO DE JUEGO","GAME HUB"},{"PRUEBA DE CONEXION","CONNECTION TEST"},{"RED Y SERVICIO","NETWORK AND SERVICE"},
        {"AJ - CONFIGURACION","AJ SETTINGS"},{"RESOLUCION Y BITRATE","RESOLUTION AND BITRATE"},
        {"SESION GFN INICIADA","GFN SIGNED IN"},{"INICIAR SESION GFN","SIGN IN TO GFN"},{"ABRIR CATALOGO","OPEN CATALOG"},
        {"CODIGO QR NVIDIA","NVIDIA QR CODE"},{"JUGAR DESDE GEFORCE NOW","PLAY ON GEFORCE NOW"},{"BIBLIOTECA DE JUEGOS","GAME LIBRARY"},
        {"INICIA SESION PRIMERO","SIGN IN FIRST"},{"VOLVER A LA SESION ACTIVA","RETURN TO ACTIVE SESSION"},{"NO SE CREA OTRA SESION","NO NEW SESSION WILL BE CREATED"},
        {"CRUCETA: ELEGIR     X: ABRIR","D-PAD: SELECT     X: OPEN"},{"CONFIGURACION AJ","AJ SETTINGS"},
        {"PROBAR MANDO","TEST CONTROLLER"},{"ACERCA DE","ABOUT"},{"PROYECTO AJ / GFN PS4","AJ / GFN PS4 PROJECT"},
        {"ACERCA DE AJ / GEFORCE NOW PS4","ABOUT AJ / GEFORCE NOW PS4"},{"CREADO POR AJ","CREATED BY AJ"},
        {"CLIENTE NATIVO EXPERIMENTAL PARA PLAYSTATION 4.","EXPERIMENTAL NATIVE CLIENT FOR PLAYSTATION 4."},
        {"INTEGRACION CON WEBRTC PARA TRANSMISION EN LA NUBE, CON","WEBRTC INTEGRATION FOR CLOUD STREAMING, WITH"},
        {"DECODIFICACION POR HARDWARE Y PRESENTACION DIRECTA EN","HARDWARE DECODING AND DIRECT PRESENTATION ON"},
        {"PANTALLA (SCEVIDEOOUT 1080P60). AUDIO OPUS NATIVO POR","SCREEN (SCEVIDEOOUT 1080P60). NATIVE OPUS AUDIO VIA"},
        {"SCEAUDIOOUT Y ENTRADA DE ULTRA BAJA LATENCIA (SCEPAD).","SCEAUDIOOUT AND ULTRA-LOW-LATENCY INPUT (SCEPAD)."},
        {"ESTA ES UNA VERSION DE PRUEBA: NO ES UNA VERSION FINAL.","THIS IS A TEST BUILD: IT IS NOT A FINAL RELEASE."},
        {"PUEDE FALLAR, CAMBIAR SIN AVISO Y NO ESTA AFILIADA NI","IT MAY FAIL, CHANGE WITHOUT NOTICE, AND IS NOT AFFILIATED"},
        {"RESPALDADA POR NVIDIA CORPORATION. TOMA OPENNOW-SWITCH","WITH OR ENDORSED BY NVIDIA CORPORATION. IT REFERENCES"},
        {"(LICENCIA MIT) COMO REFERENCIA DEL PROTOCOLO.","OPENNOW-SWITCH (MIT LICENSE) AS A PROTOCOL REFERENCE."},
        {"TIKTOK","TIKTOK"},{"YOUTUBE","YOUTUBE"},{"WEB","WEB"},
        {"CARGANDO...","LOADING..."},{"QR NO DISPONIBLE","QR UNAVAILABLE"},
        {"CLIENTE EXPERIMENTAL NATIVO PARA PLAYSTATION 4","EXPERIMENTAL NATIVE PLAYSTATION 4 CLIENT"},
        {"INTEGRA SESION GFN, WEBRTC, VIDEO H.264, AUDIO E INPUT EN SDL.","INTEGRATES GFN SESSIONS, WEBRTC, H.264 VIDEO, AUDIO AND SDL INPUT."},
        {"NO ES UNA APP OFICIAL DE NVIDIA. TOMA OPENNOW-SWITCH (MIT) COMO REFERENCIA; ESTE CLIENTE USA APIS NATIVAS DE PS4.","NOT AN OFFICIAL NVIDIA APP. IT REFERENCES OPENNOW-SWITCH (MIT); THIS CLIENT USES NATIVE PS4 APIS."},
        {"EL PROYECTO PRUEBA LA COMPATIBILIDAD DE GEFORCE NOW EN PS4.","THIS PROJECT EXPLORES GEFORCE NOW COMPATIBILITY ON PS4."},
        {"LA DECODIFICACION ACTUAL ES POR SOFTWARE; LA PRESENTACION GPU SIGUE EN INVESTIGACION.","DECODING CURRENTLY USES SOFTWARE; GPU PRESENTATION IS STILL UNDER RESEARCH."},
        {"CIRCULO: VOLVER AL CENTRO DE JUEGO","CIRCLE: BACK TO GAME HUB"},
        // Textos del menu y de CONFIGURACION que faltaban en la tabla (la auditoria los detectaba
        // como SIN TRADUCIR). Se anaden aqui para que la cobertura sea completa.
        {"SESION ACTIVA EN SEGUNDO PLANO","SESSION RUNNING IN BACKGROUND"},
        {"RESOLUCION DE VIDEO","VIDEO RESOLUTION"},
        {"IDIOMA DEL JUEGO","GAME LANGUAGE"},
        {"MODO DE CALIDAD","QUALITY MODE"},
        {"REALCE DE NITIDEZ","SHARPNESS BOOST"},
        {"FPS DE TRANSMISION","STREAMING FPS"},
        {"BUFFER DE AUDIO","AUDIO BUFFER"},
        {"DECODIFICADOR","DECODER"},
        {"NO DISPONIBLE (SW)","NOT AVAILABLE (SW)"},
        {"VER AVISO DE VERSION BETA","SHOW BETA VERSION NOTICE"},
        {"IMPORTANTE LEER","IMPORTANT - READ"},
        {"ESPANOL","SPANISH"},
        {"AJ / GEFORCE NOW PS4","AJ / GEFORCE NOW PS4"},
        {"MODO DE JUEGO SEGUN RED","NETWORK-ADAPTATION MODE"},{"LATENCIA","LOW LATENCY"},
        {"CALIDAD OPTIMA","OPTIMAL QUALITY"},{"SIN AJUSTE LOCAL","NO LOCAL ADJUSTMENT"},
        {"SE APLICA AL INICIAR: LATENCIA LIMITA A 720P/15 MBPS; NVIDIA AUN PUEDE ADAPTAR.","APPLIES ON START: LOW LATENCY CAPS AT 720P/15 MBPS; NVIDIA MAY STILL ADAPT."},
        {"CALIBRACION ACTIVA: EJES CENTRADOS Y GATILLOS EN REPOSO","CALIBRATION ACTIVE: STICKS CENTERED AND TRIGGERS AT REST"},
        {"PULSA CUADRADO, SUELTA LOS STICKS Y ESPERA","PRESS SQUARE, RELEASE THE STICKS AND WAIT"},
        {"CALIBRACION LISTA: CENTRO DEL RECORRIDO COMPLETO","CALIBRATION COMPLETE: CENTER FROM FULL STICK TRAVEL"},
        {"SUELTA LOS STICKS Y NO TOQUES EL MANDO","RELEASE THE STICKS AND LEAVE THE CONTROLLER ALONE"},
        {"LEYENDO REPOSO: NO TOQUES EL MANDO","READING REST POSITION: DO NOT TOUCH THE CONTROLLER"},
        {"CALIBRACION LISTA: REPOSO COMPENSADO","CALIBRATION COMPLETE: REST OFFSET COMPENSATED"},
        {"NO HUBO REPOSO ESTABLE; SUELTA Y REPITE","NO STABLE REST POSITION; RELEASE AND RETRY"},
        {"RESOLUCION MAXIMA","MAXIMUM RESOLUTION"},{"LIMITE DE BITRATE","BITRATE LIMIT"},{"FOTOGRAMAS POR SEGUNDO","FRAMES PER SECOND"},
        {"DISPOSITIVO DE ENTRADA","INPUT DEVICE"},{"MEDIR VELOCIDAD DE INTERNET","RUN SPEED TEST"},{"GUARDAR Y VOLVER","SAVE AND RETURN"},
        {"INICIAR SESION EN GEFORCE NOW","SIGN IN TO GEFORCE NOW"},{"ESCANEA EL CODIGO QR Y AUTORIZA EL INICIO","SCAN THE QR CODE TO AUTHORIZE SIGN-IN"},
        {"ABRE NVIDIA.COM/LOGIN EN TU TELEFONO","OPEN NVIDIA.COM/LOGIN ON YOUR PHONE"},{"LA CUENTA SE MANTIENE SOLO EN MEMORIA","ACCOUNT STAYS IN MEMORY ONLY"},
        {"SESION AUTORIZADA EN ESTA EJECUCION","SIGNED IN FOR THIS APP SESSION"},{"VOLVER AL INICIO","BACK TO HOME"},{"INTENTAR DE NUEVO","TRY AGAIN"},
        {"MI CATALOGO GEFORCE NOW","MY GEFORCE NOW CATALOG"},{"CATALOGO PUBLICO DE GEFORCE NOW","PUBLIC GEFORCE NOW CATALOG"},
        {"DESCARGANDO CATALOGO DE NVIDIA...","LOADING NVIDIA CATALOG..."},{"VISTA PREVIA","PREVIEW"},{"CARGANDO CARATULA...","LOADING COVER..."},
        {"CARATULA NO DISPONIBLE","COVER UNAVAILABLE"},{"SIN CARATULA","NO COVER"},{"INICIA SESION PARA PODER JUGAR","SIGN IN TO PLAY"},
        {"BUSCAR: ","SEARCH: "},{"ESCRIBE CON TECLADO","TYPE TO SEARCH"},{"TODOS","ALL"},{"FAVORITOS","FAVORITES"},
        {"CATEGORIA: ","CATEGORY: "},{"PULSA X PARA CONECTAR CON EL STREAM","PRESS X TO CONNECT TO THE STREAM"},
        {"TU EQUIPO ESTA LISTO","YOUR RIG IS READY"},{"NO SE PUDO INICIAR EL JUEGO","COULD NOT START THE GAME"},
        {"NO HAY JUEGOS QUE COINCIDAN. BORRA LA BUSQUEDA O CAMBIA EL FILTRO.","NO MATCHING GAMES. CLEAR SEARCH OR CHANGE FILTER."},
        {"NO HAY FAVORITOS. SELECCIONA UN JUEGO Y PULSA R3, L3 O F3.","NO FAVORITES YET. SELECT A GAME AND PRESS R3, L3 OR F3."},
        {"PREPARANDO TU SESION DE JUEGO","PREPARING YOUR GAME SESSION"},{"INICIO EXCLUSIVO DE SESION EN LA NUBE","EXCLUSIVE CLOUD SESSION SETUP"},
        {"SOLICITANDO UN EQUIPO A NVIDIA","REQUESTING A RIG FROM NVIDIA"},{"NVIDIA ESTA PREPARANDO TU EQUIPO","NVIDIA IS PREPARING YOUR RIG"},
        {"PULSA X PARA CONECTAR EL STREAM","PRESS X TO CONNECT TO THE STREAM"},{"CIRCULO / B: VOLVER AL CATALOGO","CIRCLE / B: BACK TO CATALOG"},
        {"CANCELAR SOLICITUD Y VOLVER AL CATALOGO","CANCEL REQUEST AND RETURN TO CATALOG"},
        {"NO SE PUDO INICIAR EL STREAM","COULD NOT START THE STREAM"},{"CIRCULO: VOLVER","CIRCLE: BACK"},
        {"CONECTANDO CON NVIDIA...","CONNECTING TO NVIDIA..."},{"PULSA X PARA SOLICITAR UN NUEVO CODIGO","PRESS X TO REQUEST A NEW CODE"},
        {"NO SE PUDO CARGAR: ","CATALOG LOAD FAILED: "},{"PULSA X PARA PROBAR LA CONEXION.","PRESS X TO TEST THE CONNECTION."},
        {"ESTA PRUEBA SOLO CONFIRMA RESPUESTA HTTPS.","THIS TEST ONLY CHECKS HTTPS REACHABILITY."},
        {"CERRAR SESION DE JUEGO","CLOSE GAME SESSION"},{"MENU DE SESION","SESSION MENU"},
        {"REANUDAR JUEGO","RESUME GAME"},{"MINIMIZAR Y VOLVER AL INICIO","MINIMIZE AND RETURN HOME"},
        {"ESTADISTICAS: ACTIVADAS","STATS: ON"},{"ESTADISTICAS: DESACTIVADAS","STATS: OFF"},
        {"GUARDAR Y VOLVER","SAVE AND RETURN"},{"IDIOMA","LANGUAGE"}
        ,{"CENTRO DE JUEGO","GAME HUB"},{"CRUCETA: ELEGIR     X: ABRIR","D-PAD: SELECT     X: OPEN"}
        ,{"JUEGOS COMPATIBLES","SUPPORTED GAMES"},{"CATEGORIA: ","CATEGORY: "}
        ,{"NO SE PUDO CARGAR: HTTP ","CATALOG LOAD FAILED: HTTP "},{"NO SE PUDO CARGAR: ERROR ","CATALOG LOAD FAILED: ERROR "}
        ,{"X: REINTENTAR     CIRCULO: VOLVER","X: RETRY     CIRCLE: BACK"},{"X: INICIAR EL JUEGO SELECCIONADO","X: LAUNCH SELECTED GAME"}
        ,{"ESTADO DEL STREAM","STREAM STATUS"},{"CONSULTANDO REGIONES DE NVIDIA...","CHECKING NVIDIA REGIONS..."}
        ,{"VALIDANDO LA CUENTA...","VALIDATING ACCOUNT..."},{"NVIDIA CREO LA SESION; ESPERANDO EL EQUIPO...","NVIDIA CREATED THE SESSION; WAITING FOR A RIG..."}
        ,{"ESPERANDO UN EQUIPO DE JUEGO","WAITING FOR A GAME RIG"},{"CIRCULO / B: CANCELAR LA SESION Y VOLVER","CIRCLE / B: CANCEL SESSION AND RETURN"}
        ,{"CIRCULO / B: CANCELAR SOLICITUD Y VOLVER AL CATALOGO","CIRCLE / B: CANCEL REQUEST AND RETURN TO CATALOG"}
        ,{"X: REINTENTAR  |  CIRCULO / B: VOLVER AL CATALOGO","X: RETRY  |  CIRCLE / B: BACK TO CATALOG"}
        ,{"CARGA CANCELADA","LOAD CANCELLED"},{"PULSA CIRCULO / B PARA VOLVER AL CATALOGO","PRESS CIRCLE / B TO RETURN TO CATALOG"}
        ,{"GEFORCE NOW","GEFORCE NOW"},{"PERFIL DE STREAMING","STREAMING PROFILE"}
        ,{"ABRE NVIDIA.COM/LOGIN EN TU TELEFONO","OPEN NVIDIA.COM/LOGIN ON YOUR PHONE"}
        ,{"PULSA X PARA SOLICITAR UN NUEVO CODIGO","PRESS X TO REQUEST A NEW CODE"}
        ,{"SESION AUTORIZADA EN ESTA EJECUCION","SIGNED IN FOR THIS APP SESSION"}
        ,{"CANCELAR","CANCEL"},{"FAVORITOS","FAVORITES"},{"TODOS","ALL"}
        ,{"CIRCULO: VOLVER","CIRCLE: BACK"},{"REANUDAR JUEGO","RESUME GAME"}
        ,{"NO SE PUDO INICIAR EL STREAM","COULD NOT START THE STREAM"}
        ,{"LA TRANSMISION SE DETUVO","STREAM STOPPED"},{"LIMITE ","LIMIT "}
        ,{"LATENCIA ","LATENCY "},{"HUECOS RTP ","RTP GAPS "},{"CUADROS DESCARTADOS ","DROPPED FRAMES "}
        ,{"PRUEBA DE MANDO","GAMEPAD TESTER"},{"PROBADOR DE MANDO","GAMEPAD TESTER"}
        ,{"MANDOS DETECTADOS: ","CONTROLLERS DETECTED: "},{"CONECTA UN MANDO USB O DUALSHOCK 4","CONNECT A USB CONTROLLER OR DUALSHOCK 4"}
        ,{"PULSADO","PRESSED"},{"GATILLOS L2 ","TRIGGERS L2 "},{"STICK IZQ ","LEFT STICK "},{"STICK DER ","RIGHT STICK "}
        ,{"IDIOMA / LANGUAGE","LANGUAGE"},{"ESPAÃƒÆ’Ã¢â‚¬ËœOL","SPANISH"},{"ENGLISH","ENGLISH"}
        ,{"MANTENER SESION INICIADA","KEEP ME SIGNED IN"},{"ACTIVADO","ON"},{"DESACTIVADO","OFF"}
        ,{"SESION GUARDADA CIFRADA EN ESTE PS4","SESSION SAVED IN ENCRYPTED LOCAL VAULT"}
        ,{"CATALOGO PUBLICO DE GEFORCE NOW","PUBLIC GEFORCE NOW CATALOG"}
        ,{"INICIA SESION PARA PODER JUGAR","SIGN IN TO PLAY"}
        // --- Anadidas para cerrar los huecos que detecto scripts/audit-translations.ps1.
        // Antes de esto la cobertura era del 48.5%: el dialogo de aviso beta, la pantalla
        // de conexion y el probador de mando quedaban enteros en espanol aunque el usuario
        // eligiera ingles.
        ,{"AVISO IMPORTANTE","IMPORTANT NOTICE"}
        ,{"ESTA APLICACION ES UNA VERSION BETA EN ESTADO DE DESARROLLO.","THIS APPLICATION IS A BETA BUILD UNDER DEVELOPMENT."}
        ,{"PUEDE CONTENER ERRORES Y CAMBIOS SIN AVISO.","IT MAY CONTAIN ERRORS AND CHANGE WITHOUT NOTICE."}
        ,{"USALA BAJO TU PROPIA RESPONSABILIDAD.","USE IT AT YOUR OWN RISK."}
        ,{"CUADRADO: MARCAR / DESMARCAR","SQUARE: TOGGLE"}
        ,{"CRUZ O CIRCULO: ACEPTAR Y CONTINUAR","CROSS OR CIRCLE: ACCEPT AND CONTINUE"}
        ,{"[X] MANTENER SESION INICIADA","[X] KEEP ME SIGNED IN"}
        ,{"[X] NO VOLVER A MOSTRAR ESTE AVISO","[X] DO NOT SHOW THIS NOTICE AGAIN"}
        ,{"CLIENTE NATIVO EXPERIMENTAL DE GEFORCE NOW PARA PLAYSTATION 4.","EXPERIMENTAL NATIVE GEFORCE NOW CLIENT FOR PLAYSTATION 4."}
        ,{"ESTE PROYECTO ES UNA PRUEBA DE COMPATIBILIDAD INDEPENDIENTE Y","THIS PROJECT IS AN INDEPENDENT COMPATIBILITY TEST AND"}
        ,{"NO ESTA AFILIADO NI RESPALDADO POR NVIDIA CORPORATION.","IS NOT AFFILIATED WITH OR ENDORSED BY NVIDIA CORPORATION."}
        ,{"CREADO POR AJ.","CREATED BY AJ."}
        ,{"INTEGRACION DE PROTOCOLO WEBRTC PARA TRANSMISION EN LA NUBE,","WEBRTC PROTOCOL INTEGRATION FOR CLOUD STREAMING,"}
        ,{"DECODIFICACION DE VIDEO H.264 POR SOFTWARE CON PRESENTACION","H.264 SOFTWARE VIDEO DECODING WITH DIRECT"}
        ,{"DIRECTA POR HARDWARE GPU (LIBSCEVIDEOOUT 1080P60), AUDIO","GPU HARDWARE PRESENTATION (LIBSCEVIDEOOUT 1080P60), AUDIO"}
        ,{"NATIVO OPUS A TRAVES DE SCEAUDIOOUT E INGENIERIA DE","NATIVE OPUS THROUGH SCEAUDIOOUT, AND"}
        ,{"SIGUIENTE: CATALOGO Y SESION DE JUEGO","NEXT: CATALOG AND GAME SESSION"}
        ,{"TRANSMISION A 60 FPS CON SALIDA DIRECTA POR HARDWARE (VIDEOOUT).","60 FPS STREAM WITH DIRECT HARDWARE OUTPUT (VIDEOOUT)."}
        ,{"TRANSMISION A 60 FPS OPTIMIZADA PARA PS4.","60 FPS STREAM OPTIMIZED FOR PS4."}
        ,{"ENTRADA DE ULTRA BAJA LATENCIA (SCEPAD / SDL).","ULTRA LOW LATENCY INPUT (SCEPAD / SDL)."}
        ,{"INICIANDO LA CONEXION SEGURA","STARTING SECURE CONNECTION"}
        ,{"ESPERANDO A QUE WEBRTC COMPLETE EL INICIO","WAITING FOR WEBRTC TO COMPLETE SETUP"}
        ,{"ICE, VIDEO Y AUDIO SE ACTIVARAN DESPUES","ICE, VIDEO AND AUDIO WILL START AFTERWARDS"}
        ,{"LA INTERFAZ SE ACTIVARA AL RECIBIR EL STREAM","THE INTERFACE ACTIVATES WHEN THE STREAM ARRIVES"}
        ,{"LA PRIMERA CONEXION PUEDE TARDAR UNOS SEGUNDOS.","THE FIRST CONNECTION MAY TAKE A FEW SECONDS."}
        ,{"SERVIDOR DE NVIDIA LISTO","NVIDIA SERVER READY"}
        ,{"ESTADO DEL SERVICIO","SERVICE STATUS"}
        ,{"COMPROBANDO CONEXION HTTPS CON GFN...","CHECKING HTTPS CONNECTION TO GFN..."}
        ,{"CONSULTANDO REGIONES Y MIDIENDO RTT...","QUERYING REGIONS AND MEASURING RTT..."}
        ,{"MIDIENDO...","MEASURING..."}
        ,{"NO SE PUDO MEDIR. X PARA REINTENTAR","COULD NOT MEASURE. X TO RETRY"}
        ,{"PULSA X","PRESS X"}
        ,{"PULSA X PARA CARGAR EL CATALOGO","PRESS X TO LOAD THE CATALOG"}
        ,{"PULSA X PARA CONECTAR AHORA","PRESS X TO CONNECT NOW"}
        ,{"PULSA X PARA CONECTAR EL STREAM  |  CIRCULO: CANCELAR SESION","PRESS X TO CONNECT THE STREAM  |  CIRCLE: CANCEL SESSION"}
        ,{"GUARDADO","SAVED"}
        ,{"ERROR AL GUARDAR","SAVE FAILED"}
        ,{"RESOLUCION DEL STREAM","STREAM RESOLUTION"}
        ,{"REGION DE GEFORCE NOW","GEFORCE NOW REGION"}
        ,{"ELEGIR REGION","CHOOSE REGION"}
        ,{"AUTOMATICA | NVIDIA ELIGE","AUTOMATIC | NVIDIA CHOOSES"}
        ,{"CRUCETA: ELEGIR  |  IZQ/DER: CAMBIAR  |  REGION: X ABRIR SELECTOR","D-PAD: SELECT  |  LEFT/RIGHT: CHANGE  |  REGION: X OPEN PICKER"}
        ,{"ARRIBA/ABAJO: ELEGIR  |  X: APLICAR  |  CIRCULO: CANCELAR","UP/DOWN: SELECT  |  X: APPLY  |  CIRCLE: CANCEL"}
        ,{"ARRIBA / ABAJO: ELEGIR     X: INICIAR JUEGO     CIRCULO: INICIO","UP / DOWN: SELECT     X: START GAME     CIRCLE: HOME"}
        ,{"X: REINTENTAR  |  CIRCULO: VOLVER AL INICIO","X: RETRY  |  CIRCLE: BACK TO HOME"}
        ,{"X: REPETIR PRUEBA     CIRCULO: VOLVER","X: RUN TEST AGAIN     CIRCLE: BACK"}
        ,{"X: RECONECTAR  |  TRIANGULO: CERRAR SESION  |  ARRIBA/ABAJO: ELEGIR OTRO JUEGO","X: RECONNECT  |  TRIANGLE: SIGN OUT  |  UP/DOWN: CHOOSE ANOTHER GAME"}
        ,{"CIRCULO / B: CANCELAR Y VOLVER AL CATALOGO","CIRCLE / B: CANCEL AND BACK TO CATALOG"}
        ,{"PRUEBA BOTONES, CRUCETA, EJES Y GATILLOS. MANTEN CIRCULO 1 S PARA SALIR.","TEST BUTTONS, D-PAD, AXES AND TRIGGERS. HOLD CIRCLE 1 S TO EXIT."}
        ,{"GATILLO R2","TRIGGER R2"}
        ,{"INICIA SESION Y ABRE EL CATALOGO PARA JUGAR.","SIGN IN AND OPEN THE CATALOG TO PLAY."}
    };
    const auto it=en.find(text); return it==en.end()?text:it->second.c_str();
}
static void label(int x,int y,const char* s,int scale=3) { text(x,y,translateUi(s),scale,color(222,229,239)); }
static bool loadUiFont() {
    FILE* file=fopen("/app0/assets/fonts/Gontserrat-Regular.ttf","rb");
    if(!file) return false;
    if(fseek(file,0,SEEK_END)!=0) { fclose(file); return false; }
    const long length=ftell(file);
    if(length<=0 || length>8*1024*1024 || fseek(file,0,SEEK_SET)!=0) { fclose(file); return false; }
    uiFontBytes.resize(static_cast<size_t>(length));
    if(fread(uiFontBytes.data(),1,uiFontBytes.size(),file)!=uiFontBytes.size()) { fclose(file); uiFontBytes.clear(); return false; }
    fclose(file);
    if(uiFontBytes.empty() || !stbtt_InitFont(&uiFont,uiFontBytes.data(),stbtt_GetFontOffsetForIndex(uiFontBytes.data(),0))) {
        uiFontBytes.clear(); return false;
    }
    uiFontReady=true;
    return true;
}

// Pre-render the 820x360 in-game menu overlay using Gontserrat font directly into BGRA pixels
static void updateVideoOutMenuOverlay() {
    if (!uiFontReady) return;

    constexpr int box_w = 820;
    constexpr int box_h = 300;
    constexpr int pitch = box_w * 4;
    constexpr int border_thick = 3;
    std::vector<uint8_t> buffer(static_cast<size_t>(box_w) * box_h * 4, 0);

    // 1. Draw solid dark background (#0A0E15) with vibrant cyan border (#00A4FF)
    for (int y = 0; y < box_h; ++y) {
        uint8_t* row = buffer.data() + static_cast<size_t>(y) * pitch;
        const bool is_y_border = (y < border_thick || y >= box_h - border_thick);
        for (int x = 0; x < box_w; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 4;
            const bool is_x_border = (x < border_thick || x >= box_w - border_thick);
            if (is_y_border || is_x_border) {
                // Vibrant Cyan border (#00A4FF -> B=255, G=164, R=0, A=255)
                px[0] = 255; px[1] = 164; px[2] = 0; px[3] = 255;
            } else {
                // Dark slate background (#0A0E15 -> B=21, G=14, R=10, A=255)
                px[0] = 21;  px[1] = 14;  px[2] = 10; px[3] = 255;
            }
        }
    }

    // 2. Helper to rasterize text with Gontserrat font directly onto buffer
    auto renderTextToOverlay = [&](int start_x, int start_y, const char* str, int font_px, uint8_t r, uint8_t g, uint8_t b) {
        if (!str || !*str) return;
        const float scale = stbtt_ScaleForPixelHeight(&uiFont, static_cast<float>(font_px));
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&uiFont, &ascent, &descent, &gap);
        const int baseline = start_y + static_cast<int>(std::ceil(ascent * scale));

        const unsigned char* p = reinterpret_cast<const unsigned char*>(str);
        std::vector<Uint32> cps;
        while (*p) cps.push_back(nextUtf8(p));

        int pen_x = start_x;
        for (Uint32 cp : cps) {
            int gw = 0, gh = 0, xoff = 0, yoff = 0, advance = 0, bearing = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&uiFont, scale, scale, static_cast<int>(cp), &gw, &gh, &xoff, &yoff);
            stbtt_GetCodepointHMetrics(&uiFont, static_cast<int>(cp), &advance, &bearing);

            if (bmp) {
                for (int yy = 0; yy < gh; ++yy) {
                    const int dy = baseline + yoff + yy;
                    if (dy < 0 || dy >= box_h) continue;
                    uint8_t* row = buffer.data() + static_cast<size_t>(dy) * pitch;

                    for (int xx = 0; xx < gw; ++xx) {
                        const int dx = pen_x + xoff + xx;
                        if (dx < 0 || dx >= box_w) continue;

                        const uint8_t alpha = bmp[yy * gw + xx];
                        if (alpha == 0) continue;

                        uint8_t* px = row + static_cast<size_t>(dx) * 4;
                        if (alpha == 255) {
                            px[0] = b; px[1] = g; px[2] = r; px[3] = 255;
                        } else {
                            const uint32_t inv = 255 - alpha;
                            px[0] = static_cast<uint8_t>((b * alpha + px[0] * inv) / 255);
                            px[1] = static_cast<uint8_t>((g * alpha + px[1] * inv) / 255);
                            px[2] = static_cast<uint8_t>((r * alpha + px[2] * inv) / 255);
                            px[3] = 255;
                        }
                    }
                }
                stbtt_FreeBitmap(bmp, nullptr);
            }
            pen_x += static_cast<int>(std::round(advance * scale));
        }
    };

    const int pad_x = 44;
    int cur_y = 28;

    // Header
    const char* header = language == 1 ? "GEFORCE NOW MENU" : "MENU GEFORCE NOW";
    renderTextToOverlay(pad_x, cur_y, header, 32, 255, 255, 255);
    cur_y += 56;

    // Line 1: [X] Reanudar juego (Green)
    const char* l1 = language == 1 ? "[X]           RESUME GAME" : "[X]           REANUDAR JUEGO";
    renderTextToOverlay(pad_x, cur_y, l1, 24, 66, 255, 118);
    cur_y += 50;

    // Line 2: [O] Salir al catalogo (Amber/Orange)
    const char* l2 = language == 1 ? "[O]           BACK TO CATALOG (KEEP ALIVE)" : "[O]           SALIR AL CATALOGO (MANTENER SESION)";
    renderTextToOverlay(pad_x, cur_y, l2, 24, 255, 190, 60);
    cur_y += 50;

    // Line 3: [Triangulo] Cerrar sesion GFN (Coral Red)
    const char* l4 = language == 1 ? "[TRIANGLE]    STOP GFN SESSION" : "[TRIANGULO]   CERRAR SESION GFN";
    renderTextToOverlay(pad_x, cur_y, l4, 24, 255, 80, 80);

    opennow::PS4VideoOutRenderer::SetMenuOverlayBuffer(buffer.data(), box_w, box_h, pitch);
}

// Pre-render the full-screen (1920x1080) exit card transition screen using Gontserrat font
static void updateVideoOutExitCard(bool keep_alive) {
    if (!uiFontReady) return;

    constexpr int screen_w = 1920;
    constexpr int screen_h = 1080;
    constexpr int pitch = screen_w * 4;
    std::vector<uint8_t> buffer(static_cast<size_t>(screen_w) * screen_h * 4, 0);

    // 1. Fill 100% full-screen background with Navy slate (#0A0E15 -> B=21, G=14, R=10, A=255)
    for (int y = 0; y < screen_h; ++y) {
        uint8_t* row = buffer.data() + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < screen_w; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 4;
            px[0] = 21; px[1] = 14; px[2] = 10; px[3] = 255;
        }
    }

    // 2. Draw Top Bar (#121A26)
    for (int y = 0; y < 90; ++y) {
        uint8_t* row = buffer.data() + static_cast<size_t>(y) * pitch;
        const bool is_bottom_border = (y >= 87);
        for (int x = 0; x < screen_w; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 4;
            if (is_bottom_border) {
                px[0] = 255; px[1] = 164; px[2] = 0; px[3] = 255; // Cyan border
            } else {
                px[0] = 38; px[1] = 26; px[2] = 18; px[3] = 255;
            }
        }
    }

    // 3. Draw Centered Card Panel 700x260 px (#101824 with Cyan border)
    constexpr int card_w = 700;
    constexpr int card_h = 260;
    const int card_x = (screen_w - card_w) / 2;
    const int card_y = (screen_h - card_h) / 2;
    constexpr int border = 3;

    for (int y = card_y; y < card_y + card_h; ++y) {
        uint8_t* row = buffer.data() + static_cast<size_t>(y) * pitch;
        const bool is_y_border = (y < card_y + border || y >= card_y + card_h - border);
        for (int x = card_x; x < card_x + card_w; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 4;
            const bool is_x_border = (x < card_x + border || x >= card_x + card_w - border);
            if (is_y_border || is_x_border) {
                px[0] = 255; px[1] = 164; px[2] = 0; px[3] = 255; // Cyan border
            } else {
                px[0] = 36; px[1] = 24; px[2] = 16; px[3] = 255; // Darker panel
            }
        }
    }

    // Gontserrat text rasterization helper
    auto renderTextToScreen = [&](int start_x, int start_y, const char* str, int font_px, uint8_t r, uint8_t g, uint8_t b) {
        if (!str || !*str) return;
        const float scale = stbtt_ScaleForPixelHeight(&uiFont, static_cast<float>(font_px));
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&uiFont, &ascent, &descent, &gap);
        const int baseline = start_y + static_cast<int>(std::ceil(ascent * scale));

        const unsigned char* p = reinterpret_cast<const unsigned char*>(str);
        std::vector<Uint32> cps;
        while (*p) cps.push_back(nextUtf8(p));

        int pen_x = start_x;
        for (Uint32 cp : cps) {
            int gw = 0, gh = 0, xoff = 0, yoff = 0, advance = 0, bearing = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&uiFont, scale, scale, static_cast<int>(cp), &gw, &gh, &xoff, &yoff);
            stbtt_GetCodepointHMetrics(&uiFont, static_cast<int>(cp), &advance, &bearing);

            if (bmp) {
                for (int yy = 0; yy < gh; ++yy) {
                    const int dy = baseline + yoff + yy;
                    if (dy < 0 || dy >= screen_h) continue;
                    uint8_t* row = buffer.data() + static_cast<size_t>(dy) * pitch;

                    for (int xx = 0; xx < gw; ++xx) {
                        const int dx = pen_x + xoff + xx;
                        if (dx < 0 || dx >= screen_w) continue;

                        const uint8_t alpha = bmp[yy * gw + xx];
                        if (alpha == 0) continue;

                        uint8_t* px = row + static_cast<size_t>(dx) * 4;
                        if (alpha == 255) {
                            px[0] = b; px[1] = g; px[2] = r; px[3] = 255;
                        } else {
                            const uint32_t inv = 255 - alpha;
                            px[0] = static_cast<uint8_t>((b * alpha + px[0] * inv) / 255);
                            px[1] = static_cast<uint8_t>((g * alpha + px[1] * inv) / 255);
                            px[2] = static_cast<uint8_t>((r * alpha + px[2] * inv) / 255);
                            px[3] = 255;
                        }
                    }
                }
                stbtt_FreeBitmap(bmp, nullptr);
            }
            pen_x += static_cast<int>(std::round(advance * scale));
        }
    };

    // Render Header Text
    const char* appTitle = "AJ | GEFORCE NOW PLAYSTATION 4";
    renderTextToScreen(60, 26, appTitle, 28, 255, 255, 255);

    // Render Centered Card Text
    const char* mainMsg = language == 1 ? "RETURNING TO GAME CATALOG..." : "CARGANDO CATALOGO DE JUEGOS...";
    renderTextToScreen(card_x + 50, card_y + 60, mainMsg, 30, 255, 255, 255);

    if (keep_alive) {
        const char* subMsg = language == 1 ? "SESSION KEPT IN BACKGROUND (RECONNECTABLE)" : "SESION EN SEGUNDO PLANO (RECONECTABLE)";
        renderTextToScreen(card_x + 50, card_y + 140, subMsg, 22, 255, 220, 80);
    } else {
        const char* subMsg = language == 1 ? "STOPPING GFN GAME SESSION..." : "CERRANDO SESION DE JUEGO...";
        renderTextToScreen(card_x + 50, card_y + 140, subMsg, 22, 160, 160, 160);
    }

    // Render Footer Text
    const char* footerMsg = language == 1 ? "Initializing user interface..." : "Iniciando interfaz de usuario...";
    renderTextToScreen(60, 1020, footerMsg, 20, 140, 160, 180);

    opennow::PS4VideoOutRenderer::SetExitCardBuffer(buffer.data(), screen_w, screen_h, pitch);
}
static void drawAccountBadge() {
    std::string account="AJ";
    if(authState.load()==3) {
        SDL_LockMutex(authMutex);
        if(!authSession.user.display_name.empty()) account=authSession.user.display_name;
        else if(!authSession.user.email.empty()) account=authSession.user.email;
        SDL_UnlockMutex(authMutex);
    }
    if(account.size()>28) account.resize(28);
    const std::string caption=(authState.load()==3?(language==0?"CUENTA: ":"ACCOUNT: "):(language==0?"PERFIL: ":"PROFILE: "))+account;
    label(1390,76,caption.c_str(),2);
}
static void nav(int direction) {
    const auto now=std::chrono::steady_clock::now();
    if(lastNavAt.time_since_epoch().count()!=0 && now-lastNavAt<std::chrono::milliseconds(75)) return;
    lastNavAt=now;
    opennow::TraceAppAction("UI_NAV","vertical");
    if(page==1 && settingsRegionPickerVisible) {
        std::lock_guard<std::mutex> lock(settingsRegionsMutex);
        const int count=1+static_cast<int>(settingsRegions.size());
        if(count>1) settingsRegionSelection=(settingsRegionSelection+direction+count)%count;
        opennow::LogAppLifecycleEvent("REGION_PICKER_NAV",("selection="+std::to_string(settingsRegionSelection)).c_str());
        return;
    }
    if (page==0) {
        // =========================================================================================
        // NAVEGACION VERTICAL DEL CENTRO DE JUEGO
        // =========================================================================================
        // La rejilla es de 2 COLUMNAS x 3 FILAS, y las tres filas estan COMPLETAS (seis tarjetas).
        // Eso hace que el calculo sea exacto y sin casos especiales:
        //
        //     fila  = selection / 2        (0, 1 o 2)
        //     col   = selection % 2        (0 o 1)
        //     nueva = ((fila + direccion + 3) % 3) * 2 + col
        //
        // Y como TODAS las filas tienen las dos columnas ocupadas, `nueva` siempre cae dentro del
        // rango [0..5]: no hace falta ninguna correccion.
        //
        // HISTORIA, para no repetir el fallo: esta funcion usaba `rows=4` con un array de tres filas,
        // y ademas tenia una rama especial (`else if(nextRow==2) selection=4`) que existia porque la
        // tercera fila solo tenia una tarjeta. Cuando el menu paso a tener tres filas COMPLETAS, esa
        // rama mandaba la seleccion a la tarjeta 4 SIEMPRE que se bajaba a la ultima fila: el usuario
        // pulsaba abajo y el foco saltaba a una tarjeta que no era la de su columna. Ese era parte del
        // desfase reportado.
        const int row=selection/2, col=selection%2;
        const int nextRow=(row+direction+kHomeRows)%kHomeRows;
        selection=nextRow*2+col;

        // LOG DE VERIFICACION AL MOVER LA CRUCETA.
        // Pide el encargo un log al mover el foco. Se registra el indice Y el TITULO de la tarjeta
        // sobre la que queda el foco: si alguna vez vuelve a haber un desfase entre el mando y la
        // pantalla, en el log estara exactamente que tarjeta cree la app que esta seleccionada.
        //
        // Se llama MENU_FOCUS y no MENU_NAV para no confundirlo con el log del dispatch de la X, que
        // registra la tarjeta que se HA PULSADO. Son dos momentos distintos y conviene distinguirlos.
        {
            HomeCard cards[kHomeCardCount];
            buildHomeCards(cards, authState.load()==3, hasPublishedStream());
            const int index=(selection>=0 && selection<kHomeCardCount)?selection:0;
            char detail[192];
            snprintf(detail,sizeof(detail),"MENU_FOCUS index=%d title=\"%s\" dir=%d",
                     index,cards[index].title,direction);
            opennow::LogAppLifecycleEvent("MENU_FOCUS",detail);
        }
    }
    else if (page==1) selection=(selection+direction+15)%15;
    else if (page==4) selection=(selection+direction+3)%3;
    else if (page==3 && catalogState.load()==2) {
        const int count=catalogVisibleCount;
        if(count>0) {
            catalogSelection=(catalogSelection+direction+count)%count;
            if(catalogSelection<catalogOffset) catalogOffset=catalogSelection;
            if(catalogSelection>=catalogOffset+9) catalogOffset=catalogSelection-8;
        }
    }
    else if(page==5 && streamMenuVisible) streamMenuSelection=(streamMenuSelection+direction+4)%4;
    opennow::LogAppLifecycleEvent("UI_NAV",("page="+std::to_string(page)+" direction="+std::to_string(direction)+" selection="+std::to_string(page==3?catalogSelection:selection)).c_str());
}
static void adjust(int direction) {
    if (page != 1) return;
    saveResult=0;
    opennow::TraceAppAction("SETTINGS_ADJUST","value changed");
    switch(selection) {
    // ORDEN ESTRICTO DE LAS FILAS (debe coincidir con las etiquetas de draw()):
    //   0 IDIOMA | 1 DISPOSITIVO | 2 RESOLUCION | 3 FPS | 4 BITRATE | 5 CALIDAD | 6 BUFFER AUDIO
    //   7 IDIOMA JUEGO | 8 DECODIFICADOR | 9 RESOLUCION DE VIDEO | 10 REALCE | 11 REGION
    //   12 VELOCIDAD | 13 AVISO | 14 GUARDAR
    case 0: language=language==0?1:0; break;
    case 1: inputDevice=(inputDevice+direction+3)%3; break;
    case 2: resolution=(resolution+direction+3)%3; break; // 0=Auto, 1=720p, 2=1080p
    // Fila 3: FPS de transmision. 60 o 30. Presupuesto por cuadro: 16,6 ms a 60, 33,3 ms a 30.
    case 3: {
        fps=(fps==30)?60:30;
        char fpsDetail[64];
        std::snprintf(fpsDetail,sizeof(fpsDetail),"fps=%d frame_budget_us=%d",fps,(fps==30)?33333:16666);
        opennow::LogAppLifecycleEvent("SETTINGS_FPS_CHANGED",fpsDetail);
        break;
    }
    // Fila 4: limite de bitrate, ACOTADO a la ventana segura 25-35 Mbps (objetivo v3.29).
    //
    // POR QUE ACOTADO: fuera de esa ventana el control de flujo se desestabiliza. Por debajo de
    // 25 Mbps el servidor comprime demasiado (borrosidad y bloques). Por encima de 35 Mbps la
    // ventana declarada queda muy lejos del bitrate que el servidor entrega de verdad (~5,8 Mbps
    // medidos), y esa distancia es el margen donde oscila el control de congestion, que es lo que
    // dispara las caidas de rendimiento.
    case 4: {
        static const int kBitrateSteps[]={25,30,35};
        constexpr int kSteps=static_cast<int>(sizeof(kBitrateSteps)/sizeof(kBitrateSteps[0]));
        int idx=0; for(int i=0;i<kSteps;i++) if(kBitrateSteps[i]==bitrate) { idx=i; break; }
        idx=(idx+direction+kSteps)%kSteps;
        bitrate=kBitrateSteps[idx];
        char brDetail[80];
        std::snprintf(brDetail,sizeof(brDetail),"bitrate_mbps=%d window=25-35_mbps",bitrate);
        opennow::LogAppLifecycleEvent("SETTINGS_BITRATE_CHANGED",brDetail);
        break;
    }
    case 5: imageQualityMode=(imageQualityMode+direction+3)%3; break;  // Original/Clarity/Adaptive
    case 6: {
        audioBufferMs += direction*10;
        if(audioBufferMs<20) audioBufferMs=20;
        if(audioBufferMs>80) audioBufferMs=80;
        break;
    }
    case 7: gameLanguage=(gameLanguage+direction+8)%8; break;          // 8 idiomas de juego
    // Fila 8: modo de decodificador. 0=AUTO, 1=HARDWARE, 2=SOFTWARE.
    // Fila 8: decodificador. SIN AJUSTE (punto D del plan): el hardware no es funcional, asi que
    // el case se deja sin accion. No se elimina para no alterar la numeracion del resto de filas.
    case 8: break;
    // Fila 9: resolucion FIJA del framebuffer. 0=720P FIJO, 1=1080P FIJO. Sin modos automaticos.
    // Fila 9: informativa (punto B). Sin ajuste: la resolucion la controla la fila 2.
    case 9: break;
    // Fila 10: realce de nitidez. 0=desactivado, 1=suave, 2=medio, 3=alto.
    case 10: sharpnessLevel=(sharpnessLevel+direction+4)%4; break;
    case 11: break; // Selector de region: se abre con X
    case 12: break; // Prueba de velocidad: se lanza con X
    case 13: break; // Aviso importante: se abre con X
    case 14: break; // Guardar y volver: con X
    default: break;
    }
}
static void moveHorizontal(int direction) {
    const auto now=std::chrono::steady_clock::now();
    if(lastHorizontalAt.time_since_epoch().count()!=0 && now-lastHorizontalAt<std::chrono::milliseconds(145)) return;
    lastHorizontalAt=now;
    opennow::TraceAppAction("UI_NAV","horizontal");
    if(page==0) {
        const int row=selection/2, col=selection%2;
        const int nextCol=(col+direction+2)%2;
        const int candidate=row*2+nextCol;
        // Las dos columnas de cada fila estan ocupadas, asi que el candidato siempre es valido; la
        // comprobacion se mantiene como red de seguridad y usa la constante compartida.
        if(candidate<kHomeCardCount) selection=candidate;

        // Mismo log que en la navegacion vertical: deja constancia de donde queda el foco.
        {
            HomeCard cards[kHomeCardCount];
            buildHomeCards(cards, authState.load()==3, hasPublishedStream());
            const int index=(selection>=0 && selection<kHomeCardCount)?selection:0;
            char detail[192];
            snprintf(detail,sizeof(detail),"MENU_FOCUS index=%d title=\"%s\" dir=%d (horizontal)",
                     index,cards[index].title,direction);
            opennow::LogAppLifecycleEvent("MENU_FOCUS",detail);
        }
    }
    else if(page==1) adjust(direction);
    else if(page==4 && selection<2) selection=(selection+direction+2)%2;
    opennow::LogAppLifecycleEvent("UI_HORIZONTAL",("page="+std::to_string(page)+" direction="+std::to_string(direction)+" selection="+std::to_string(page==3?catalogSelection:selection)+" resolution="+std::to_string(resolution)+" bitrate="+std::to_string(bitrate)+" fps="+std::to_string(fps)).c_str());
}
static int loginWithGfn(void*) {
    opennow::LogAppLifecycleEvent("AUTH_WORKER_STARTED");
    try {
        opennow::GfnClient client;
        const auto providers=client.FetchLoginProviders();
        if(providers.empty()) throw std::runtime_error("NVIDIA no devolvio proveedores de inicio de sesion.");
        auto session=client.LoginWithQrCode(providers.front(),[](const opennow::QrLoginChallenge& challenge) {
            SDL_LockMutex(authMutex);
            authChallenge=challenge;
            SDL_UnlockMutex(authMutex);
            authState.store(2);
        },[] { return authCancelled.load(); });
        session.persistence_enabled=rememberLogin;
        if(rememberLogin) {
            try { client.SaveSession(session); opennow::LogAppLifecycleEvent("AUTH_REMEMBER_SAVED","encrypted_vault=1"); }
            catch(...) { opennow::LogAppLifecycleEvent("AUTH_REMEMBER_SAVE_FAILED","login_remains_active=1"); }
        } else {
            client.ClearSavedSession();
            opennow::LogAppLifecycleEvent("AUTH_REMEMBER_DISABLED");
        }
        SDL_LockMutex(authMutex);
        authSession=session;
        authSession.persistence_enabled=rememberLogin;
        authError.clear();
        SDL_UnlockMutex(authMutex);
        authState.store(3);
        opennow::LogAppLifecycleEvent("AUTH_SUCCEEDED");
    } catch(const std::exception& error) {
        SDL_LockMutex(authMutex);
        authError=error.what();
        SDL_UnlockMutex(authMutex);
        authState.store(authCancelled.load()?0:4);
        opennow::LogAppLifecycleEvent(authCancelled.load()?"AUTH_CANCELLED":"AUTH_FAILED");
    }
    return 0;
}
static void startGfnLogin() {
    if(authThread && authState.load()!=1 && authState.load()!=2) {
        SDL_WaitThread(authThread,NULL); authThread=NULL;
    }
    if(authThread) return;
    authCancelled.store(false);
    authState.store(1);
    opennow::LogAppLifecycleEvent("AUTH_START");
    SDL_LockMutex(authMutex); authError.clear(); authChallenge=opennow::QrLoginChallenge{}; SDL_UnlockMutex(authMutex);
    authThread=SDL_CreateThread(loginWithGfn,"gfn-login",NULL);
    if(!authThread) { authError="No se pudo iniciar el proceso de inicio de sesion."; authState.store(4); }
}
static void cancelGfnLogin() {
    if(authThread && (authState.load()==1 || authState.load()==2)) authCancelled.store(true);
}
// Persistent HTTP/SSL context for probe and speed test
static int g_probeNetPool = -1;
static int g_probeSslId = -1;
static int g_probeHttpId = -1;

static int serviceProbe(void*) {
    int templateId=-1,connectionId=-1,requestId=-1;
    int statusCode=0,errorCode=0;
    uint64_t bytesRead=0;
    int elapsedMs=0;
    uint32_t transferStart=0;
    SDL_LockMutex(networkMutex);
    const char* url=testKind==1
        ? "https://speed.cloudflare.com/__down?bytes=20000000"
        : "https://play.geforcenow.com/mall/";

    if(g_probeHttpId < 0) {
        int rc=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
        if(rc<0 && rc!=static_cast<int>(0x80960003)) { errorCode=rc; goto cleanup; }
        rc=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
        if(rc<0 && rc!=static_cast<int>(0x80960003)) { errorCode=rc; goto cleanup; }
        rc=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
        if(rc<0 && rc!=static_cast<int>(0x80960003)) { errorCode=rc; goto cleanup; }
        g_probeNetPool=sceNetPoolCreate("gfn-ps4-probe",64*1024,0);
        if(g_probeNetPool<0) { errorCode=g_probeNetPool; goto cleanup; }
        g_probeSslId=sceSslInit(SSL_POOLSIZE);
        if(g_probeSslId<0) { errorCode=g_probeSslId; goto cleanup; }
        g_probeHttpId=sceHttpInit(g_probeNetPool,g_probeSslId,LIBHTTP_POOLSIZE);
        if(g_probeHttpId<0) { errorCode=g_probeHttpId; goto cleanup; }
        opennow::LogAppLifecycleEvent("HTTP_POOL_INIT_OK", "serviceProbe persistent pool created");
    } else {
        opennow::LogAppLifecycleEvent("HTTP_POOL_REUSED", "serviceProbe persistent pool reused");
    }

    templateId=sceHttpCreateTemplate(g_probeHttpId,"GFN-PS4-Platform-Test/0.1",ORBIS_HTTP_VERSION_1_1,0);
    if(templateId<0) { errorCode=templateId; goto cleanup; }
    sceHttpSetConnectTimeOut(templateId,12*1000*1000);
    sceHttpSetResolveTimeOut(templateId,12*1000*1000);
    connectionId=sceHttpCreateConnectionWithURL(templateId,url,0);
    if(connectionId<0) { errorCode=connectionId; goto cleanup; }
    requestId=sceHttpCreateRequestWithURL(connectionId,ORBIS_METHOD_GET,url,0);
    if(requestId<0) { errorCode=requestId; goto cleanup; }
    transferStart=SDL_GetTicks();
    {
        int rc=sceHttpSendRequest(requestId,NULL,0);
        if(rc<0) { errorCode=rc; goto cleanup; }
        rc=sceHttpGetStatusCode(requestId,&statusCode);
        if(rc<0) errorCode=rc;
        else if(testKind==1 && (statusCode<200 || statusCode>=300)) errorCode=-10000-statusCode;
        else if(testKind==1 && statusCode>=200 && statusCode<300) {
            char buffer[32768];
            for(;;) {
                int count=sceHttpReadData(requestId,buffer,sizeof(buffer));
                if(count<0) { errorCode=count; break; }
                if(count==0) break;
                bytesRead+=(uint64_t)count;
            }
            elapsedMs=(int)(SDL_GetTicks()-transferStart);
            if(bytesRead==0 && errorCode==0) errorCode=-2;
        }
    }
cleanup:
    if(requestId>=0) sceHttpDeleteRequest(requestId);
    if(connectionId>=0) sceHttpDeleteConnection(connectionId);
    if(templateId>=0) sceHttpDeleteTemplate(templateId);
    SDL_UnlockMutex(networkMutex);
    probeHttpStatus.store(statusCode);
    probeError.store(errorCode);
    probeSpeedBytes.store(bytesRead);
    probeElapsedMs.store(elapsedMs);
    probeSpeedMbps10.store(elapsedMs>0 ? (int)((bytesRead*80ULL)/((uint64_t)elapsedMs*1000ULL)) : 0);
    probeState.store(errorCode==0?2:3);
    return 0;
}
static bool initPs4Mouse() {
    int rc=sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOUSE);
    if(rc<0 && rc!=0x80960003) return false;
    rc=sceMouseInit();
    if(rc<0 && rc!=0x80960003) return false;
    int userId=0;
    if(sceUserServiceGetInitialUser(&userId)<0) userId=0;
    Ps4MouseOpenParam param{};
    // Merged mode lets the system expose a mouse combined with a keyboard.
    param.behaviorFlag=1;
    ps4MouseHandle=sceMouseOpen(userId,0,0,&param);
    ps4MouseReady=ps4MouseHandle>=0;
    return ps4MouseReady;
}
static void pollPs4Mouse() {
    if(!ps4MouseReady || ps4MouseHandle<0 || !hasPublishedStream() || inputDevice==0) return;
    Ps4MouseData data[8]{};
    const int count=sceMouseRead(ps4MouseHandle,data,8);
    if(count<=0) return;
    uint32_t latestButtons=ps4MouseButtons;
    int64_t dx=0,dy=0;
    for(int i=0;i<count && i<8;i++) {
        if(!data[i].connected || (data[i].buttons&0x80000000u)) continue;
        dx+=data[i].xAxis; dy+=data[i].yAxis;
        latestButtons=data[i].buttons&0x1fu;
    }
    const bool blocked=streamMenuVisible||streamSuppressInputUntilNeutral;
    if(dx||dy) {
        const int mx=static_cast<int>(std::clamp<int64_t>(dx,-4096,4096));
        const int my=static_cast<int>(std::clamp<int64_t>(dy,-4096,4096));
        if(!blocked) activeStream->send_mouse_move(static_cast<int16_t>(mx),static_cast<int16_t>(my));
    }
    const bool wasLeft=(ps4MouseButtons&0x01u)!=0,nowLeft=(latestButtons&0x01u)!=0;
    if(wasLeft!=nowLeft) activeStream->send_mouse_left_button(!blocked&&nowLeft);
    ps4MouseButtons=latestButtons;
}
static int fetchCatalogImpl(void*) {
    opennow::LogAppLifecycleEvent("CATALOG_STAGE","worker_entered");
    int rc=0;
    int templateId=-1,connectionId=-1,requestId=-1;
    int statusCode=0,errorCode=0;
    size_t used=0,capacity=512*1024;
    char* body=(char*)malloc(capacity+1);
    const auto fetch_started=std::chrono::steady_clock::now();
    const bool authenticated=authState.load()==3;
    const bool requestFullCatalog=catalogFullRequested.exchange(false,std::memory_order_acq_rel);
    if(!body) {
        catalogError.store(-1); catalogState.store(3);
        opennow::LogAppLifecycleEvent("CATALOG_FAILED","error=allocation");
        return 0;
    }

    if(authenticated) {
        if(requestFullCatalog) {
            opennow::LogAppLifecycleEvent("CATALOG_STAGE","full_catalog_cache_check_begin");
            if(loadCatalogSnapshot(true)) { free(body); return 0; }
            opennow::LogAppLifecycleEvent("CATALOG_STAGE","full_catalog_cache_miss");
            try {
                opennow::AuthSession session;
                SDL_LockMutex(authMutex); session=authSession; SDL_UnlockMutex(authMutex);
                opennow::GfnClient client;
                const std::string vpcId=client.ResolveCatalogVpcId(session);
                std::string cursor;
                int count=0,pages=0;
                do {
                    opennow::CatalogPage result=client.FetchCatalogPage(session,"",cursor,vpcId);
                    ++pages;
                    for(const auto& game:result.games) {
                        if(count>=2400) break;
                        if(game.title.empty()) continue;
                        CatalogGame& target=pendingCatalogGames[count];
                        strncpy(target.title,game.title.c_str(),sizeof(target.title)-1); target.title[sizeof(target.title)-1]='\0';
                        const std::string store=game.store.empty()?"GFN":game.store;
                        strncpy(target.store,store.c_str(),sizeof(target.store)-1); target.store[sizeof(target.store)-1]='\0';
                        const std::string appId=game.launch_app_id.empty()?game.id:game.launch_app_id;
                        strncpy(target.launchAppId,appId.c_str(),sizeof(target.launchAppId)-1); target.launchAppId[sizeof(target.launchAppId)-1]='\0';
                        strncpy(target.imageUrl,game.image_url.c_str(),sizeof(target.imageUrl)-1); target.imageUrl[sizeof(target.imageUrl)-1]='\0';
                        target.linked=game.is_in_library?1:0;
                        ++count;
                    }
                    if(pages==1 || pages%10==0)
                        opennow::LogAppLifecycleEvent("CATALOG_STAGE",("full_catalog_page="+std::to_string(pages)+" games="+std::to_string(count)).c_str());
                    if(count>=2400 || !result.next_cursor) break;
                    cursor=*result.next_cursor;
                } while(pages<50);
                if(count==0) throw std::runtime_error("El catÃƒÆ’Ã‚Â¡logo completo no devolviÃƒÆ’Ã‚Â³ juegos.");
                SDL_LockMutex(authMutex); authSession=session; SDL_UnlockMutex(authMutex);
                saveCatalogSnapshot(pendingCatalogGames,count,true);
                pendingCatalogCount.store(count,std::memory_order_relaxed);
                pendingCatalogIsFull.store(true,std::memory_order_release);
                pendingCatalogReady.store(true,std::memory_order_release);
                catalogHttpStatus.store(200); catalogError.store(0);
                const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-fetch_started).count();
                opennow::LogAppLifecycleEvent("CATALOG_READY",("source=full_catalog games="+std::to_string(count)+" pages="+std::to_string(pages)+" ms="+std::to_string(elapsed)).c_str());
                free(body);
                return 0;
            } catch(const std::exception& error) {
                catalogFullRequested.store(true,std::memory_order_release);
                catalogError.store(-1); catalogState.store(3);
                opennow::LogAppLifecycleEvent("CATALOG_FAILED",("source=full_catalog error="+std::string(error.what())).c_str());
                free(body);
                return 0;
            }
        }
        opennow::LogAppLifecycleEvent("CATALOG_STAGE","account_cache_check_begin");
        if(loadCatalogSnapshot(false)) { opennow::LogAppLifecycleEvent("CATALOG_STAGE","account_cache_hit"); free(body); return 0; }
        opennow::LogAppLifecycleEvent("CATALOG_STAGE","account_cache_miss");
        try {
            opennow::AuthSession session;
            SDL_LockMutex(authMutex); session=authSession; SDL_UnlockMutex(authMutex);
            opennow::LogAppLifecycleEvent("CATALOG_STAGE",(std::string("account_session_copied access=")+
                (session.tokens.access_token.empty()?"missing":"present")+" refresh="+
                (session.tokens.refresh_token.empty()?"missing":"present")).c_str());
            opennow::GfnClient client;
            opennow::LogAppLifecycleEvent("CATALOG_STAGE","gfn_client_constructed");
            int count=0;
            opennow::LogAppLifecycleEvent("CATALOG_STAGE","account_library_request_begin");
            const std::vector<opennow::GameInfo> libraryGames=client.FetchLibraryGames(session);
            opennow::LogAppLifecycleEvent("CATALOG_STAGE",("account_library_request_complete received="+
                std::to_string(libraryGames.size())).c_str());
            for(const auto& game:libraryGames) {
                if(count>=2400) break;
                if(!game.is_in_library || game.launch_app_id.empty() || game.title.empty()) continue;
                strncpy(pendingCatalogGames[count].title,game.title.c_str(),sizeof(pendingCatalogGames[count].title)-1);
                pendingCatalogGames[count].title[sizeof(pendingCatalogGames[count].title)-1]='\0';
                std::string store="GFN";
                if(game.selected_variant_index<game.variants.size() && !game.variants[game.selected_variant_index].store.empty())
                    store=game.variants[game.selected_variant_index].store;
                else if(!game.available_stores.empty() && !game.available_stores.front().empty())
                    store=game.available_stores.front();
                strncpy(pendingCatalogGames[count].store,store.c_str(),sizeof(pendingCatalogGames[count].store)-1);
                pendingCatalogGames[count].store[sizeof(pendingCatalogGames[count].store)-1]='\0';
                strncpy(pendingCatalogGames[count].launchAppId,game.launch_app_id.c_str(),sizeof(pendingCatalogGames[count].launchAppId)-1);
                pendingCatalogGames[count].launchAppId[sizeof(pendingCatalogGames[count].launchAppId)-1]='\0';
                strncpy(pendingCatalogGames[count].imageUrl,game.image_url.c_str(),sizeof(pendingCatalogGames[count].imageUrl)-1);
                pendingCatalogGames[count].imageUrl[sizeof(pendingCatalogGames[count].imageUrl)-1]='\0';
                pendingCatalogGames[count].linked=1;
                ++count;
            }
            SDL_LockMutex(authMutex); authSession=session; SDL_UnlockMutex(authMutex);
            catalogHttpStatus.store(200);
            catalogError.store(count==0?-22:0);
            if(count>0) {
                saveCatalogSnapshot(pendingCatalogGames,count,false);
                pendingCatalogCount.store(count,std::memory_order_relaxed);
                pendingCatalogIsFull.store(false,std::memory_order_release);
                pendingCatalogReady.store(true,std::memory_order_release);
            } else catalogState.store(3);
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-fetch_started).count();
            const std::string detail="source=account_library games="+std::to_string(count)+
                " received="+std::to_string(libraryGames.size())+" requests=1 ms="+std::to_string(elapsed);
            opennow::LogAppLifecycleEvent(count==0?"CATALOG_FAILED":"CATALOG_READY",detail.c_str());
            free(body);
            return 0;
        } catch(const std::exception& error) {
            catalogError.store(-1);
            catalogState.store(3);
            opennow::LogAppLifecycleEvent("CATALOG_FAILED",("source=authenticated error="+
                std::string(error.what())).c_str());
            free(body);
            return 0;
        }
    }

    SDL_LockMutex(networkMutex);
    if(g_probeHttpId < 0) {
        int rc=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
        if(rc<0 && rc!=static_cast<int>(0x80960003)) { errorCode=rc; goto catalog_cleanup; }
        rc=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
        if(rc<0 && rc!=static_cast<int>(0x80960003)) { errorCode=rc; goto catalog_cleanup; }
        rc=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
        if(rc<0 && rc!=static_cast<int>(0x80960003)) { errorCode=rc; goto catalog_cleanup; }
        g_probeNetPool=sceNetPoolCreate("gfn-ps4-probe",64*1024,0);
        if(g_probeNetPool<0) { errorCode=g_probeNetPool; goto catalog_cleanup; }
        g_probeSslId=sceSslInit(SSL_POOLSIZE);
        if(g_probeSslId<0) { errorCode=g_probeSslId; goto catalog_cleanup; }
        g_probeHttpId=sceHttpInit(g_probeNetPool,g_probeSslId,LIBHTTP_POOLSIZE);
        if(g_probeHttpId<0) { errorCode=g_probeHttpId; goto catalog_cleanup; }
        opennow::LogAppLifecycleEvent("HTTP_POOL_INIT_OK", "catalog persistent pool created");
    } else {
        opennow::LogAppLifecycleEvent("HTTP_POOL_REUSED", "catalog persistent pool reused");
    }
    templateId=sceHttpCreateTemplate(g_probeHttpId,"AJ-GFN-PS4-Catalog/0.18",ORBIS_HTTP_VERSION_1_1,0);
    if(templateId<0) { errorCode=templateId; goto catalog_cleanup; }
    sceHttpSetConnectTimeOut(templateId,15*1000*1000);
    sceHttpSetResolveTimeOut(templateId,15*1000*1000);
    connectionId=sceHttpCreateConnectionWithURL(templateId,kPublicCatalogUrl,0);
    if(connectionId<0) { errorCode=connectionId; goto catalog_cleanup; }
    requestId=sceHttpCreateRequestWithURL(connectionId,ORBIS_METHOD_GET,kPublicCatalogUrl,0);
    if(requestId<0) { errorCode=requestId; goto catalog_cleanup; }
    sceHttpAddRequestHeader(requestId,"Accept","application/json",0);
    rc=sceHttpSendRequest(requestId,NULL,0);
    if(rc<0) { errorCode=rc; goto catalog_cleanup; }
    rc=sceHttpGetStatusCode(requestId,&statusCode);
    if(rc<0) { errorCode=rc; goto catalog_cleanup; }
    if(statusCode<200 || statusCode>=300) { errorCode=-10000-statusCode; goto catalog_cleanup; }
    for(;;) {
        if(used+32769>capacity) {
            if(capacity>=2*1024*1024) { errorCode=-12; break; }
            size_t next=capacity*2;
            char* grown=(char*)realloc(body,next+1);
            if(!grown) { errorCode=-12; break; }
            body=grown; capacity=next;
        }
        int count=sceHttpReadData(requestId,body+used,32768);
        if(count<0) { errorCode=count; break; }
        if(count==0) break;
        used+=(size_t)count;
    }
    if(errorCode==0 && used==0) errorCode=-2;
    if(errorCode==0) {
        body[used]='\0';
        cJSON* root=cJSON_ParseWithLength(body,used);
        if(!cJSON_IsArray(root)) errorCode=-22;
        else {
            int count=0;
            for(cJSON* item=root->child; item && count<2400; item=item->next) {
                cJSON* title=cJSON_GetObjectItemCaseSensitive(item,"title");
                cJSON* state=cJSON_GetObjectItemCaseSensitive(item,"status");
                cJSON* store=cJSON_GetObjectItemCaseSensitive(item,"store");
                if(!cJSON_IsString(title) || !title->valuestring || !title->valuestring[0]) continue;
                if(!cJSON_IsString(state) || strcmp(state->valuestring,"AVAILABLE")!=0) continue;
                strncpy(pendingCatalogGames[count].title,title->valuestring,sizeof(pendingCatalogGames[count].title)-1);
                pendingCatalogGames[count].title[sizeof(pendingCatalogGames[count].title)-1]='\0';
                const char* storeName=(cJSON_IsString(store) && store->valuestring)?store->valuestring:"GFN";
                strncpy(pendingCatalogGames[count].store,storeName,sizeof(pendingCatalogGames[count].store)-1);
                pendingCatalogGames[count].store[sizeof(pendingCatalogGames[count].store)-1]='\0';
                pendingCatalogGames[count].launchAppId[0]='\0';
                pendingCatalogGames[count].imageUrl[0]='\0';
                ++count;
            }
            if(count>0) {
                pendingCatalogCount.store(count,std::memory_order_relaxed);
                pendingCatalogReady.store(true,std::memory_order_release);
            }
            cJSON_Delete(root); root=NULL;
            if(count==0) errorCode=-22;
        }
        if(errorCode!=0 && root) cJSON_Delete(root);
    }
catalog_cleanup:
    if(requestId>=0) sceHttpDeleteRequest(requestId);
    if(connectionId>=0) sceHttpDeleteConnection(connectionId);
    if(templateId>=0) sceHttpDeleteTemplate(templateId);
    SDL_UnlockMutex(networkMutex);
    free(body);
    catalogHttpStatus.store(statusCode);
    catalogError.store(errorCode);
    catalogState.store(errorCode==0?2:3);
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-fetch_started).count();
    const int publishedCount=errorCode==0?pendingCatalogCount.load():0;
    const std::string detail="source=public games="+std::to_string(publishedCount)+" http="+std::to_string(statusCode)+" error="+std::to_string(errorCode)+" ms="+std::to_string(elapsed);
    opennow::LogAppLifecycleEvent(errorCode==0?"CATALOG_READY":"CATALOG_FAILED",detail.c_str());
    return 0;
}
// SDL's default worker stack is too small for the authenticated catalog path:
// it performs TLS, JSON parsing and several nested client calls. Keep exceptions
// inside the worker boundary so a malformed response cannot terminate the app.
static int fetchCatalog(void* opaque) {
    try {
        return fetchCatalogImpl(opaque);
    } catch(const std::exception& error) {
        pendingCatalogReady.store(false,std::memory_order_release);
        catalogError.store(-1);
        catalogState.store(3,std::memory_order_release);
        opennow::LogAppLifecycleEvent("CATALOG_FAILED",("exception="+std::string(error.what())).c_str());
    } catch(...) {
        pendingCatalogReady.store(false,std::memory_order_release);
        catalogError.store(-1);
        catalogState.store(3,std::memory_order_release);
        opennow::LogAppLifecycleEvent("CATALOG_FAILED","exception=unknown");
    }
    return 0;
}
static void startCatalogFetch() {
    if(catalogThread && catalogState.load()!=1) { SDL_WaitThread(catalogThread,NULL); catalogThread=NULL; }
    if(catalogThread) return;
    const bool fullRequested=catalogFullRequested.load(std::memory_order_acquire);
    if(authState.load()==3 && catalogState.load()==2 && catalogCount.load()>0 && !fullRequested) return;
    if(!fullRequested) catalogCount.store(0);
    catalogHttpStatus.store(0); catalogError.store(0);
    pendingCatalogCount.store(0); pendingCatalogReady.store(false,std::memory_order_release);
    catalogVisibleCount=0;
    catalogState.store(1); catalogSelection=0; catalogOffset=0;
    opennow::LogAppLifecycleEvent("CATALOG_START",authState.load()==3?"source=authenticated":"source=public");
    catalogThread=SDL_CreateThreadWithStackSize(
        fetchCatalog,"gfn-catalog",kNetworkWorkerStackSize,NULL);
    if(!catalogThread) { catalogError.store(-1); catalogState.store(3); opennow::LogAppLifecycleEvent("CATALOG_FAILED","error=thread_create"); }
}
static void startServiceProbe() {
    if(probeThread && probeState.load()!=1) { SDL_WaitThread(probeThread,NULL); probeThread=NULL; }
    if(probeThread) return;
    testKind=0;
    probeHttpStatus.store(0); probeError.store(0); probeState.store(1);
    probeThread=SDL_CreateThread(serviceProbe,"service-probe",NULL);
    if(!probeThread) { probeError.store(-1); probeState.store(3); }
}
static void startSpeedTest() {
    if(probeThread && probeState.load()!=1) { SDL_WaitThread(probeThread,NULL); probeThread=NULL; }
    if(probeThread) return;
    testKind=1;
    probeHttpStatus.store(0); probeError.store(0); probeState.store(1);
    probeSpeedBytes.store(0); probeElapsedMs.store(0); probeSpeedMbps10.store(0);
    probeThread=SDL_CreateThread(serviceProbe,"speed-test",NULL);
    if(!probeThread) { probeError.store(-1); probeState.store(3); }
}
static int fetchSettingsRegions(void*) {
    opennow::LogAppLifecycleEvent("REGION_FETCH_BEGIN","source=settings_picker");
    try {
        if(authState.load()!=3) throw std::runtime_error("Inicia sesion en GeForce NOW para consultar las regiones.");
        opennow::AuthSession session;
        SDL_LockMutex(authMutex); session=authSession; SDL_UnlockMutex(authMutex);
        opennow::GfnClient client;
        auto regions=client.FetchStreamRegions(session);
        opennow::LogAppLifecycleEvent("REGION_METADATA_READY",("count="+std::to_string(regions.size())).c_str());
        // Present the names immediately; measure their RTT on this worker.
        {
            std::lock_guard<std::mutex> lock(settingsRegionsMutex);
            settingsRegions=regions;
        }
        regions=client.MeasureStreamRegionLatencies(std::move(regions));
        std::stable_sort(regions.begin(),regions.end(),[](const opennow::StreamRegion& a,const opennow::StreamRegion& b) {
            if(a.ping_ms<0) return false;
            if(b.ping_ms<0) return true;
            return a.ping_ms<b.ping_ms;
        });
        SDL_LockMutex(authMutex); authSession=session; SDL_UnlockMutex(authMutex);
        const size_t regionCount=regions.size();
        std::string summary="count="+std::to_string(regionCount);
        for(size_t i=0;i<std::min<size_t>(regionCount,8);++i)
            summary+=" "+regions[i].name+"="+(regions[i].ping_ms>=0?std::to_string(regions[i].ping_ms)+"ms":"unreachable");
        {
            std::lock_guard<std::mutex> lock(settingsRegionsMutex);
            settingsRegions=std::move(regions);
        }
        settingsRegionState.store(regionCount==0?3:2);
        opennow::LogAppLifecycleEvent("STREAM_REGIONS_LOADED",summary.c_str());
        opennow::LogAppLifecycleEvent("REGION_FETCH_COMPLETE",("count="+std::to_string(regionCount)).c_str());
    } catch(const std::exception& error) {
        settingsRegionState.store(3);
        opennow::LogAppLifecycleEvent("STREAM_REGIONS_FAILED",error.what());
    } catch(...) { settingsRegionState.store(3); opennow::LogAppLifecycleEvent("STREAM_REGIONS_FAILED","exception=unknown"); }
    return 0;
}
static void startSettingsRegionFetch() {
    if(settingsRegionThread && settingsRegionState.load()!=1) { SDL_WaitThread(settingsRegionThread,nullptr); settingsRegionThread=nullptr; }
    if(settingsRegionThread || settingsRegionState.load()==2) return;
    settingsRegionState.store(1);
    settingsRegionThread=SDL_CreateThread(fetchSettingsRegions,"gfn-regions",nullptr);
    if(!settingsRegionThread) settingsRegionState.store(3);
}
static std::string settingsRegionLabel() {
    if(streamRegion=="Auto") return language==0?"AUTOMATICA":"AUTOMATIC";
    std::lock_guard<std::mutex> lock(settingsRegionsMutex);
    for(const auto& item:settingsRegions) if(item.url==streamRegion)
        return item.name+(item.ping_ms>=0?" | "+std::to_string(item.ping_ms)+" ms":" | SIN MEDICION");
    return language==0?"MANUAL (URL GUARDADA)":"MANUAL (SAVED URL)";
}
static void commitSettingsRegionSelection() {
    if(settingsRegionSelection==0) streamRegion="Auto";
    else {
        std::lock_guard<std::mutex> lock(settingsRegionsMutex);
        if(static_cast<size_t>(settingsRegionSelection-1)<settingsRegions.size())
            streamRegion=settingsRegions[static_cast<size_t>(settingsRegionSelection-1)].url;
    }
    opennow::LogAppLifecycleEvent("STREAM_REGION_SELECTED",streamRegion=="Auto"?"region=Auto":streamRegion.c_str());
    opennow::TraceAppAction("REGION_PICKER_SELECT",streamRegion=="Auto"?"region=Auto":streamRegion.c_str());
    settingsRegionPickerVisible=false;
}
static void loadSettings() {
    const opennow::StreamSettings stream=opennow::LoadStreamSettings();
    streamRegion=savedStreamRegion=stream.region;

    // Subfase 2.2c: Stream settings defaults (Resolution: 0=Auto, 1=720p, 2=1080p)
    // Default to 720p. With the software H.264 decoder this console cannot hold
    // 1080p60: measured in/dec/outFps=40.09/35.51/0.00 with queueDrops climbing to
    // 384 access units, and when the server did hand over 1080p it immediately
    // stepped down to 1280x720 and 960x540 mid-session. Requesting 720p keeps the
    // stream at the output buffer size, which removes the per-frame CPU scaling
    // entirely (measured 30.7 ms/frame) and cuts decoder work in half.
    resolution=savedResolution=1;
    // El bitrate NO se fuerza a 0: se conserva el ajuste del usuario. Antes se ponia a 0
    // ("auto") y el servidor elegia 12 Mbps para 720p60, lo que producia la imagen borrosa.
    // El valor de partida es 25 Mbps, que es el ajuste guardado por defecto.
    fps=savedFps=60;              // 60 FPS
    networkMode=savedNetworkMode=1; // Clarity / Default

    FILE* file=fopen("/data/gfnps4/settings.cfg","r");
    if(file) {
        int r=0,b=0,f=0,l=0,m=0,d=2,n=1,beta=0;
        // Campos 9-11 anadidos en la 2.89: modo de calidad, buffer de audio y idioma del juego.
        int q=0,ab=40,gl=0,sh=2,dm=0,sm=0;
        // 14 CAMPOS: deben coincidir EXACTAMENTE con los que escribe saveSettings().
        //
        // BUG QUE ESTO CORRIGE: la cadena de formato tenia 13 `%d` mientras el fichero guarda 14
        // campos (el ultimo es scaleMode, la resolucion de video). Con 13 especificadores, el ultimo
        // valor no se leia y `sm` quedaba con el valor de la inicializacion, asi que la comprobacion
        // `fields>=14` NUNCA se cumplia: **la resolucion de video configurada no se cargaba nunca** y
        // siempre arrancaba con la de por defecto.
        const int fields=fscanf(file,"%d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                                &r,&b,&f,&l,&m,&d,&n,&beta,&q,&ab,&gl,&sh,&dm,&sm);
        if(fields>=1 && r>=0 && r<=2) resolution=savedResolution=r;
        if(fields>=2 && b>=0 && b<=100) bitrate=savedBitrate=b;
        if(fields>=3 && f>=30 && f<=120) fps=savedFps=f;
        if(fields>=4 && l>=0 && l<=1) language=savedLanguage=l;
        if(fields>=5 && m>=0 && m<=1) rememberLogin=savedRememberLogin=(m==1);
        if(fields>=6 && d>=0 && d<=2) inputDevice=savedInputDevice=d;
        // Field 8 was added with the beta disclaimer. Missing or malformed means
        // "not yet answered", so the notice is shown again.
        if(fields>=8 && beta==1) betaDisclaimerDontShowAgain=true;
        else betaDisclaimerDontShowAgain=false;
        // Campos nuevos: si el fichero es de una version anterior no existen y se usan los
        // valores por defecto, asi que los ajustes viejos siguen funcionando.
        // El modo de calidad se reordeno en la 2.92b (Original pasa a ser 0). Un valor guardado
        // de una version anterior sigue siendo valido porque el rango es el mismo 0-2.
        if(fields>=9 && q>=0 && q<=2) imageQualityMode=savedImageQualityMode=q;
        if(fields>=10 && ab>=10 && ab<=120) audioBufferMs=savedAudioBufferMs=ab;
        if(fields>=11 && gl>=0 && gl<=7) gameLanguage=savedGameLanguage=gl;
        // REALCE FORZADO A DESACTIVADO.
        //
        // POR QUE SE IGNORA EL VALOR GUARDADO: medido en consola (v3.03 y v3.05), el realce
        // adaptativo lleva el escalado a dispatch_avg_us=25000-33000 y los FPS a 23-29 durante TODA
        // la sesion. El coste esta en las lecturas de memoria por pixel, no en la fuerza, asi que
        // CUALQUIER nivel activo cuesta lo mismo: no hay termino medio entre 60 FPS y 25 FPS.
        //
        // Un ajuste guardado de una version anterior hundia los FPS sin que el usuario lo supiera,
        // porque la configuracion persistida pisa el valor por defecto del codigo. Se ignora a
        // proposito hasta que el realce quepa en el presupuesto (via GPU), y se registra para que
        // quede claro en el log.
        // REALCE DE NITIDEZ: SE RESPETA EL VALOR GUARDADO (corregido en la v3.54)
        // -----------------------------------------------------------------------------------------
        // AQUI ESTABA EL FALLO DE LA "IMAGEN BORROSA":
        //
        //     if(fields>=12) {
        //         if(sh>0) { LogAppLifecycleEvent("SETTINGS_SHARPNESS_OVERRIDE", ...); }
        //         sharpnessLevel=savedSharpnessLevel=0;      // <-- se DESCARTA el valor guardado
        //     }
        //
        // El ajuste "REALCE DE NITIDEZ" **existe en el menu de configuracion, se puede cambiar y se
        // guarda en el fichero**. Y al cargarlo se tiraba a la basura, en silencio, con el motivo
        // `coste_cpu`. El log lo dejaba claro: `STREAM_SHARPNESS_APPLIED sharpness_level=0 percent=0`
        // **siempre**, hiciera lo que hiciera el usuario.
        //
        // El motivo original era razonable: cuando el cliente iba a 15 fps, el realce (que hace unas
        // pocas operaciones mas por pixel) empeoraba la situacion. Pero el efecto secundario es que
        // **el usuario no tenia ninguna forma de mejorar la nitidez**, y la imagen se veia blanda.
        //
        // AHORA se aplica el nivel que el usuario haya elegido (0 por defecto = desactivado, que sigue
        // siendo el valor de partida para no cambiar el comportamiento de quien no lo toque). Se
        // registra el nivel EFECTIVO para que el log no mienta:
        //
        //     SETTINGS_SHARPNESS_LOADED saved=N applied=N
        //
        // Si el usuario nota que le cuesta FPS, puede volver a ponerlo en "DESACTIVADO" desde el menu
        // (fila 10). Es una eleccion suya y visible, no un descarte oculto.
        if(fields>=12 && sh>=0 && sh<=3) {
            sharpnessLevel=savedSharpnessLevel=sh;
            char shDetail[96];
            std::snprintf(shDetail,sizeof(shDetail),"saved=%d applied=%d",sh,sharpnessLevel);
            opennow::LogAppLifecycleEvent("SETTINGS_SHARPNESS_LOADED",shDetail);
        }
        if(fields>=13 && dm>=0 && dm<=2) decoderMode=savedDecoderMode=dm;
        if(fields>=14 && sm>=0 && sm<=1) scaleMode=savedScaleMode=sm;
        fclose(file);
    }
    savedBetaDisclaimerDontShowAgain=betaDisclaimerDontShowAgain;

    // Always overwrite legacy file with cleaned configuration
    mkdir("/data/gfnps4",0777);
    FILE* outFile=fopen("/data/gfnps4/settings.tmp","w");
    if(outFile) {
        int ok=fprintf(outFile,"%d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",resolution,bitrate,fps,language,rememberLogin?1:0,inputDevice,networkMode,betaDisclaimerDontShowAgain?1:0,imageQualityMode,audioBufferMs,gameLanguage,sharpnessLevel,decoderMode,scaleMode)>0;
        if(fflush(outFile)!=0) ok=0;
        if(fclose(outFile)!=0) ok=0;
        if(ok) rename("/data/gfnps4/settings.tmp","/data/gfnps4/settings.cfg");
    }

    opennow::LogAppLifecycleEvent("SETTINGS_DEFAULTS_ENFORCED",
        ("resolution=" + std::string(resolution==0?"auto":(resolution==1?"720p":"1080p")) + " bitrate=auto fps=60 network_mode=clarity").c_str());
}
// Aplica el nivel de realce de nitidez al escalador.
//
// POR QUE ES UNA FUNCION Y NO UN BLOQUE DENTRO DE saveSettings()
// ---------------------------------------------------------------
// Estaba dentro de saveSettings(), que solo se ejecuta cuando el usuario pulsa X en "GUARDAR Y
// VOLVER". Consecuencia medida en consola: en una sesion normal el realce NUNCA se aplicaba, porque
// el registro no mostraba ni SETTINGS_SAVED ni STREAM_SHARPNESS_APPLIED. El ajuste existia, se
// guardaba en el fichero, y no llegaba al escalador.
//
// Es el MISMO patron de fallo que tuvo el bitrate en la 2.88: codigo correcto en un camino que no
// se ejecuta. La leccion es que un ajuste debe aplicarse al ARRANCAR, no solo al guardar.
static void applySharpness() {
    static const int kSharpPercent[]={0, 50, 100, 160};   // desactivado, suave, medio, alto
    const int sPct=(sharpnessLevel>=0 && sharpnessLevel<4)?kSharpPercent[sharpnessLevel]:100;
    opennow::color::SetLumaSharpenPercent(sPct);
    char shDetail[80];
    std::snprintf(shDetail,sizeof(shDetail),
                  "sharpness_level=%d percent=%d",sharpnessLevel,sPct);
    opennow::LogAppLifecycleEvent("STREAM_SHARPNESS_APPLIED",shDetail);
}
static bool saveSettings() {
    opennow::StreamSettings stream=opennow::LoadStreamSettings();
    int streamW = 1280, streamH = 720;
    resolution_to_wh(resolution, streamW, streamH);
    stream.width = streamW;
    stream.height = streamH;
    // FPS DE TRANSMISION: 60 o 30, segun la configuracion del usuario.
    //
    // OBJETIVO v3.29: permitir 30 FPS como opcion estable. A 30 FPS el presupuesto por cuadro es de
    // 33,3 ms (el doble que a 60), y el escalado 540p->720p medido (~13,8 ms con 6 participantes)
    // deja de ser critico. Para sesiones largas o escenas muy complejas, 30 FPS estables se perciben
    // mejor que 60 FPS oscilando entre 30 y 60.
    //
    // El valor se guarda en settings.cfg (campo 2) y se aplica al stream Y a la peticion SDP
    // (a=video.maxFPS), que es donde el servidor lo lee.
    stream.fps = (fps == 30) ? 30 : 60;
    // BITRATE EFECTIVO.
    //
    // Antes se enviaba 0 ("auto") y el servidor de NVIDIA decidia. En la practica elegia 12 Mbps
    // para 720p60, que es bajo: de ahi la imagen borrosa y la actualizacion fea que reporto el
    // usuario. La PS4 medida en esta red da 91,8 Mbps por cable, asi que hay margen de sobra.
    //
    // El ajuste del usuario (variables `bitrate` / `savedBitrate`, pestaÃƒÆ’Ã‚Â±a de resolucion) existia
    // pero NUNCA se usaba para el stream: solo se guardaba en settings.cfg. Ahora si se aplica.
    //
    // El modo de red sigue existiendo: en modo conservador se limita a 15 Mbps para no saturar
    // una conexion mala. Ese es el equivalente al viejo "optimizar para redes de baja calidad".
    const int effectiveBitrateKbps =
        (networkMode==0 ? std::min(bitrate, 15) : bitrate) * 1000;
    stream.bitrate_kbps=effectiveBitrateKbps > 0 ? effectiveBitrateKbps : 0;
    char bitrateDetail[96];
    std::snprintf(bitrateDetail, sizeof(bitrateDetail), "bitrate_kbps=%d network_mode=%d",
                  stream.bitrate_kbps, networkMode);
    opennow::LogAppLifecycleEvent("STREAM_BITRATE_APPLIED", bitrateDetail);
    // CALIDAD DE IMAGEN: como reparte el servidor el bitrate. Solo se usaba "Clarity".
    //   Clarity  -> maxima nitidez
    //   Balanced -> equilibrio nitidez/fluidez
    //   Adaptive -> prioriza fluidez en escenas con movimiento
    // Es el unico ajuste que cambia la nitidez sin tocar el bitrate.
    static const char* const kQualityModes[]={"Original","Clarity","Adaptive"};
    const int qIdx=(imageQualityMode>=0 && imageQualityMode<3)?imageQualityMode:0;
    stream.image_quality_mode=kQualityModes[qIdx];

    // REALCE DE NITIDEZ DEL ESCALADOR.
    //
    // El escalado bilineal suaviza la imagen cuando el servidor entrega 540p y la salida es 720p.
    // El usuario lo describio con precision: "al entrar al juego se ve excelente, y despues empieza
    // a actualizarse la imagen". La diferencia es que al inicio el stream mide 720p y se usa la
    // copia 1:1 SIN procesado; en cuanto baja a 540p entra el escalado y se ve mas blando.
    //
    // El realce de nitidez NO se aplica aqui. Ver la nota en applySharpness(): vivia en este punto,
    // que solo se ejecuta al pulsar X en "GUARDAR Y VOLVER", asi que en una sesion normal nunca
    // corria. Ahora se aplica al arrancar la app, con el resto de ajustes cargados.

    // BUFFER DE AUDIO: menos milisegundos = menos latencia, mas = menos cortes por jitter.
    // Rango util en la practica: 20 a 80 ms.
    stream.audio_buffer_ms = (audioBufferMs>=10 && audioBufferMs<=120) ? audioBufferMs : 40;

    // IDIOMA DEL JUEGO (no de la interfaz): se envia al servidor para que el juego arranque
    // en ese idioma cuando lo soporte.
    static const char* const kGameLanguages[]={"en_US","es_ES","es_MX","fr_FR","de_DE","it_IT","pt_BR","ja_JP"};
    const int lIdx=(gameLanguage>=0 && gameLanguage<8)?gameLanguage:0;
    stream.game_language=kGameLanguages[lIdx];

    char qualityDetail[192];
    std::snprintf(qualityDetail,sizeof(qualityDetail),
                  "image_quality=%s audio_buffer_ms=%d game_language=%s",
                  stream.image_quality_mode.c_str(), stream.audio_buffer_ms,
                  stream.game_language.c_str());
    opennow::LogAppLifecycleEvent("STREAM_QUALITY_APPLIED",qualityDetail);
    stream.region=streamRegion;
    stream.preset_id="custom"; stream.label="AJ Personalizado";
    if(!opennow::SaveStreamSettings(stream)) { saveResult=-1; return false; }
    mkdir("/data/gfnps4",0777);
    FILE* file=fopen("/data/gfnps4/settings.tmp","w");
    if(!file) { saveResult=-1; return false; }
    int ok=fprintf(file,"%d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",resolution,bitrate,fps,language,rememberLogin?1:0,inputDevice,networkMode,betaDisclaimerDontShowAgain?1:0,imageQualityMode,audioBufferMs,gameLanguage,sharpnessLevel,decoderMode,scaleMode)>0;
    if(fflush(file)!=0) ok=0;
    if(fclose(file)!=0) ok=0;
    if(!ok || rename("/data/gfnps4/settings.tmp","/data/gfnps4/settings.cfg")!=0) { saveResult=-1; return false; }
    savedResolution=resolution; savedBitrate=bitrate; savedFps=fps; savedSharpnessLevel=sharpnessLevel; savedDecoderMode=decoderMode; savedScaleMode=scaleMode;
    savedLanguage=language;
    savedRememberLogin=rememberLogin;
    savedInputDevice=inputDevice;
    savedNetworkMode=networkMode;
    savedBetaDisclaimerDontShowAgain=betaDisclaimerDontShowAgain;
    savedStreamRegion=streamRegion;
    saveResult=1;
    opennow::LogAppLifecycleEvent("STREAM_SETTINGS_SAVED",
        ("resolution=" + std::string(resolution==0?"auto":(resolution==1?"720p":"1080p")) +
         " width_height=" + std::to_string(stream.width) + "x" + std::to_string(stream.height) +
         " fps=" + std::to_string(stream.fps) + " bitrate_kbps=auto network_mode=clarity").c_str());
    return true;
}
static void discardSettings() {
    resolution=savedResolution; bitrate=savedBitrate; fps=savedFps; networkMode=savedNetworkMode; language=savedLanguage; rememberLogin=savedRememberLogin; inputDevice=savedInputDevice; streamRegion=savedStreamRegion;
}
static int launchSelectedGame(void*) {
    opennow::AuthSession session;
    std::string createdSessionId;
    std::unique_ptr<opennow::GfnClient> client;
    try {
        client.reset(new opennow::GfnClient());
        SDL_LockMutex(authMutex); session=authSession; SDL_UnlockMutex(authMutex);
        if(!hasPublishedStream()) {
            opennow::LogAppLifecycleEvent("STALE_SESSION_CLEANUP_BEGIN");
            client->CleanupStaleCloudSession(session);
            SDL_LockMutex(authMutex); authSession=session; SDL_UnlockMutex(authMutex);
            opennow::LogAppLifecycleEvent("STALE_SESSION_CLEANUP_COMPLETE");
        }
        launchPollCount=0; launchCloudStatus=0; launchQueuePosition=0; launchSeatEtaMs=0;
        opennow::SessionInfo info=client->StartSession(session,launchAppId,launchStore,launchTitle);
        createdSessionId=info.session_id;
        opennow::LogAppLifecycleEvent("CLOUD_SESSION_CREATED",("status="+std::to_string(info.status)).c_str());
        launchCloudStatus=info.status; launchQueuePosition=info.queue_position; launchSeatEtaMs=info.seat_setup_eta_ms;
        opennow::LogAppLifecycleEvent("CLOUD_SESSION_STATE_PUBLISHED",("status="+std::to_string(info.status)+
            " queue="+std::to_string(info.queue_position)).c_str());
        opennow::LogAppLifecycleEvent("CLOUD_SESSION_WAIT_BEGIN","target_ms=2500 method=sceKernelUsleep");
        while(!launchCancelled.load() && std::chrono::steady_clock::now() - launchStartedAt < std::chrono::seconds(150)) {
            if(info.status>=2 && !info.signaling_url.empty() && !info.session_id.empty() &&
               !info.media_ip.empty() && info.media_port>0) break;
            const auto pollWaitStarted=std::chrono::steady_clock::now();
            while(!launchCancelled.load() &&
                  std::chrono::steady_clock::now() - pollWaitStarted < std::chrono::milliseconds(2500))
                sceKernelUsleep(50*1000);
            if(launchCancelled.load()) break;
            const auto waitMs=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-pollWaitStarted).count();
            if(launchPollCount.load()==0)
                opennow::LogAppLifecycleEvent("CLOUD_SESSION_WAIT_COMPLETE",("wait_ms="+std::to_string(waitMs)).c_str());
            const int priorStatus=launchCloudStatus.load();
            info=client->PollSession(session,info.session_id);
            ++launchPollCount;
            launchCloudStatus=info.status; launchQueuePosition=info.queue_position; launchSeatEtaMs=info.seat_setup_eta_ms;
            const int pollNumber=launchPollCount.load();
            if(pollNumber==1 || pollNumber%10==0 || info.status!=priorStatus) {
                const std::string pollDetail="status="+std::to_string(info.status)+" poll="+std::to_string(pollNumber)+
                    " queue="+std::to_string(info.queue_position)+" wait_ms="+std::to_string(waitMs);
                opennow::LogAppLifecycleEvent("CLOUD_SESSION_POLL",pollDetail.c_str());
            }
            if(info.status==4) throw std::runtime_error("NVIDIA cerro la sesion antes de iniciar el stream.");
        }
        if(launchCancelled.load()) throw std::runtime_error("Inicio de juego cancelado.");
        if(info.status<2) {
            throw std::runtime_error("NVIDIA todavia no asigna el equipo (estado "+std::to_string(info.status)+
                ", cola "+std::to_string(info.queue_position)+"). Cancela o espera y vuelve a intentar.");
        }
        if(info.signaling_url.empty() || info.session_id.empty() || info.media_ip.empty() || info.media_port<=0) {
            if(std::chrono::steady_clock::now()-launchStartedAt>=std::chrono::seconds(150)) {
                throw std::runtime_error("NVIDIA no asigno equipo tras 150 s. Estado " + std::to_string(info.status) +
                    ", cola " + std::to_string(info.queue_position) + ". Cancela y prueba mas tarde.");
            }
            throw std::runtime_error("NVIDIA no devolvio los datos de streaming. Revisa la conexion y disponibilidad del juego.");
        }
        SDL_LockMutex(authMutex); authSession=session; SDL_UnlockMutex(authMutex);
        // Build the potentially allocating copy before taking the UI mutex.
        // If allocation fails, the catch path must not recursively wait on a
        // mutex left locked by an exception during string/vector assignment.
        opennow::SessionInfo readyInfo=info;
        SDL_LockMutex(launchMutex); pendingGameSession=std::move(readyInfo); launchError[0]='\0'; SDL_UnlockMutex(launchMutex);
        launchState.store(2);
        launchReadyAt=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        opennow::LogAppLifecycleEvent("CLOUD_SESSION_READY");
    } catch(const std::exception& ex) {
        if(client && !createdSessionId.empty()) { try { client->StopSession(session,createdSessionId); } catch(...) {} }
        SDL_LockMutex(launchMutex); snprintf(launchError,sizeof(launchError),"%.500s",ex.what()); SDL_UnlockMutex(launchMutex);
        launchState.store(3);
        opennow::LogAppLifecycleEvent(launchCancelled.load()?"CLOUD_SESSION_CANCELLED":"CLOUD_SESSION_FAILED",ex.what());
    } catch(...) {
        if(client && !createdSessionId.empty()) { try { client->StopSession(session,createdSessionId); } catch(...) {} }
        SDL_LockMutex(launchMutex); snprintf(launchError,sizeof(launchError),"Error inesperado al iniciar la sesion de juego."); SDL_UnlockMutex(launchMutex);
        launchState.store(3);
        opennow::LogAppLifecycleEvent("CLOUD_SESSION_FAILED","exception=unknown");
    }
    return 0;
}
static void startSelectedGame() {
    if(authState.load()!=3 || catalogSelection<0 || catalogSelection>=catalogVisibleCount) return;
    const int selectedIndex=catalogVisible[catalogSelection];
    if(selectedIndex<0 || selectedIndex>=catalogCount.load() || !catalogGames[selectedIndex].launchAppId[0]) return;
    if(launchThread && launchState.load()!=1) { SDL_WaitThread(launchThread,NULL); launchThread=NULL; }
    if(launchThread) return;
    launchAppId=catalogGames[selectedIndex].launchAppId;
    launchStore=catalogGames[selectedIndex].store;
    launchTitle=catalogGames[selectedIndex].title;
    launchCoverUrl=catalogGames[selectedIndex].imageUrl;
    launchStartedAt=std::chrono::steady_clock::now();
    launchUiLoopEnteredLogged=false; launchUiPadPollLogged=false;
    launchUiDrawBeginLogged=false; launchUiDrawDoneLogged=false;
    launchUiDrawStage=0;
    page=6;
    opennow::LogAppLifecycleEvent("GAME_LAUNCH_REQUESTED",("store="+launchStore).c_str());
    opennow::trace::StreamEvent("LANZAMIENTO_PEDIDO", ("store="+launchStore).c_str());
    SDL_LockMutex(launchMutex); launchError[0]='\0'; SDL_UnlockMutex(launchMutex);
    launchCancelled.store(false); launchState.store(1);
    opennow::LogAppLifecycleEvent("GAME_LAUNCH_WORKER_START","stack=4MiB");
    launchThread=SDL_CreateThreadWithStackSize(
        launchSelectedGame,"gfn-launch",kNetworkWorkerStackSize,NULL);
    if(!launchThread) { launchState.store(3); SDL_LockMutex(launchMutex); snprintf(launchError,sizeof(launchError),"No se pudo iniciar la tarea de NVIDIA."); SDL_UnlockMutex(launchMutex); }
    else opennow::LogAppLifecycleEvent("GAME_LAUNCH_THREAD_CREATED","worker=gfn-launch");
}
struct StreamStartArgs {
    opennow::SessionInfo info;
    std::string jwt;
};
static int startStreamWorker(void* opaque) {
    struct WorkerExitTrace {
        ~WorkerExitTrace() { opennow::WriteStreamStartupStage("stream_worker_thread_exit"); }
    } workerExitTrace;
    std::unique_ptr<StreamStartArgs> args(static_cast<StreamStartArgs*>(opaque));
    try {
        opennow::WriteStreamStartupStage("constructing_stream_client");
        std::unique_ptr<WebRtcSession> stream(new WebRtcSession(
            args->info.signaling_url, args->jwt, args->info.session_id,
            args->info.media_ip, args->info.media_port, args->info.ice_servers));
        opennow::WriteStreamStartupStage("stream_client_constructed");
        opennow::WriteStreamStartupStage("starting_webrtc_signaling");
        stream->start();
        opennow::WriteStreamStartupStage("stream_worker_publish_begin");
        pendingStream=std::move(stream);
        opennow::WriteStreamStartupStage("stream_worker_publish_complete");
        // Publish immediately after moving the started session. The UI joins
        // this worker before adopting pendingStream. Avoid disk logging here:
        // signaling workers are already active and this is the handoff edge.
        streamStartState.store(2, std::memory_order_release);
        opennow::WriteStreamStartupStage("stream_worker_return_ready");
    }
    catch(const std::exception& ex) {
        opennow::WriteStreamStartupStage("startup_exception");
        SDL_LockMutex(launchMutex); snprintf(launchError,sizeof(launchError),"%.500s",ex.what()); SDL_UnlockMutex(launchMutex);
        streamStartState.store(3, std::memory_order_release);
    }
    catch(...) {
        opennow::WriteStreamStartupStage("startup_unknown_exception");
        SDL_LockMutex(launchMutex); snprintf(launchError,sizeof(launchError),"Error desconocido al negociar WebRTC."); SDL_UnlockMutex(launchMutex);
        streamStartState.store(3, std::memory_order_release);
    }
    return 0;
}
static void joinAndAdoptStreamWorker(bool waitWhileConnecting=false) {
    if(!streamStartThread || (!waitWhileConnecting && streamStartState.load(std::memory_order_acquire)==1)) return;
    opennow::TraceAppAction("STREAM_HANDOFF","ui_join_begin");
    opennow::WriteStreamStartupStage("stream_ui_worker_join_wait_begin");
    opennow::LogAppLifecycleEvent("STREAM_UI_WORKER_JOIN_WAIT_BEGIN");
    SDL_WaitThread(streamStartThread,NULL);
    streamStartThread=NULL;
    opennow::TraceAppAction("STREAM_HANDOFF","ui_join_complete");
    opennow::WriteStreamStartupStage("stream_ui_worker_join_wait_complete");
    opennow::LogAppLifecycleEvent("STREAM_UI_WORKER_JOIN_WAIT_COMPLETE",
        ("state="+std::to_string(streamStartState.load(std::memory_order_acquire))+" pending="+(pendingStream?"yes":"no")).c_str());
    if(streamStartState.load(std::memory_order_acquire)==2) {
        if(pendingStream) {
            opennow::WriteStreamStartupStage("stream_ui_adopt_begin");
            activeStream=std::move(pendingStream);
            opennow::TraceAppAction("STREAM_HANDOFF","session_moved_to_ui_owner");
            opennow::WriteStreamStartupStage("stream_ui_adopt_complete");
            opennow::WriteStreamStartupStage("stream_ui_adopted_after_join");
            opennow::LogAppLifecycleEvent("STREAM_UI_ADOPTED_AFTER_JOIN");
        } else {
            opennow::WriteStreamStartupStage("stream_worker_returned_without_stream");
            SDL_LockMutex(launchMutex);
            snprintf(launchError,sizeof(launchError),"El hilo de conexiÃƒÆ’Ã‚Â³n terminÃƒÆ’Ã‚Â³ sin entregar la sesiÃƒÆ’Ã‚Â³n WebRTC.");
            SDL_UnlockMutex(launchMutex);
            streamStartState.store(3,std::memory_order_release);
        }
    }
}
static bool g_streamColorRangeLogged = false;
static void startPreparedStream() {
    if(launchState.load()!=2) return;
    opennow::SessionInfo info;
    opennow::AuthSession auth;
    SDL_LockMutex(launchMutex); info=pendingGameSession; SDL_UnlockMutex(launchMutex);
    SDL_LockMutex(authMutex); auth=authSession; SDL_UnlockMutex(authMutex);
    const std::string jwt=auth.tokens.id_token.empty()?auth.tokens.access_token:auth.tokens.id_token;
    if(jwt.empty()) { launchState.store(3); SDL_LockMutex(launchMutex); snprintf(launchError,sizeof(launchError),"La sesion de NVIDIA expiro; inicia sesion otra vez."); SDL_UnlockMutex(launchMutex); return; }
    activeSessionId=info.session_id; activeGameTitle=launchTitle;
    streamMenuVisible=false; streamStatsVisible=false; streamMenuChordWasDown=false; streamSuppressInputUntilNeutral=false;
    statsLastSampleMs=0; statsVideoKbps=0; statsVideoFps=0; statsDecodeFps=0; statsSwapFps=0;
    streamHealthLogAt=0;
    statsLastDecoded=0; statsLastPresented=0;
    // Reset per-attempt diagnostics so retries are fully traceable in the same app run.
    streamFirstDrawLogged=false;
    streamFirstLoopLogged=false; streamFirstEventPassLogged=false; streamFirstPadPassLogged=false;
    streamJoinBeginLogged=false; streamJoinDoneLogged=false; streamCoverPassLogged=false;
    streamFrameCallbackLogged=false;

    // Reset VideoOut handoff state for this streaming session
    g_videoOutHandoffDone = false;
    g_streamColorRangeLogged = false;
    s_lastPresentedGeneration = 0;
    g_streamExitPromptActive = false;
    s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
    videoOutDirectActive = g_videoOutDebugFlagDetected && !g_videoOutQuarantined;
    if(videoOutDirectActive) {
        updateVideoOutMenuOverlay();
        char sessionStartDetail[128];
        std::snprintf(sessionStartDetail, sizeof(sessionStartDetail), "session_id=%.80s videoOutDirectActive=1", activeSessionId.c_str());
        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SESSION_START", sessionStartDetail);
    } else if(g_videoOutQuarantined) {
        // La ruta directa fallo de forma persistente antes en esta ejecucion: se va directo a SDL sin
        // volver a intentarlo. Queda en el log para que no parezca que la ruta esta apagada sin motivo.
        opennow::LogAppLifecycleEvent("VIDEOOUT_SKIP_QUARANTINED",
            "motivo=fallo_persistente_previo ruta=SDL reintento=al_reiniciar_la_app");
    }

    activeStreamStartedAt=std::chrono::steady_clock::now();
    opennow::LogAppLifecycleEvent("STREAM_CONNECT_PRESSED");

    // Telemetry: Log applied resolution requested by settings
    {
        int req_w = 1280, req_h = 720;
        resolution_to_wh(resolution, req_w, req_h);
        const char* res_mode = (resolution == 0) ? "auto" : ((resolution == 1) ? "720p_fixed" : "1080p_fixed");
        char resDetail[128];
        std::snprintf(resDetail, sizeof(resDetail), "requested=%dx%d mode=%s", req_w, req_h, res_mode);
        opennow::LogAppLifecycleEvent("STREAM_RESOLUTION_APPLIED", resDetail);
    }

    activeStream.reset();
    pendingStream.reset();
    streamStartState.store(1);
    page=5;
    // Al entrar en una partida se sube la prioridad del hilo principal: el decodificador se pone a la
    // maxima en cada frame, y sin esto el hilo que lee el mando y presenta compite desde abajo (es la
    // causa del input lag). Ver la explicacion completa en `setStreamingThreadPriority`.
    setStreamingThreadPriority(true);
    StreamStartArgs* args=new StreamStartArgs{std::move(info), jwt};
    opennow::WriteStreamStartupStage("thread_starting");
    // WebRTC/crypto/codec constructors are deep on PS4; avoid the tiny
    // platform default stack for this worker.
    opennow::LogAppLifecycleEvent("STREAM_WORKER_START","stack=4MiB");
    opennow::trace::StreamEvent("WORKER_DE_STREAM_INICIADO", "stack=4MiB");
    streamStartThread=SDL_CreateThreadWithStackSize(
        startStreamWorker,"gfn-stream-start",kNetworkWorkerStackSize,args);
    if(!streamStartThread) {
        delete args;
        opennow::WriteStreamStartupStage("thread_creation_failed");
        streamStartState.store(3);
        SDL_LockMutex(launchMutex); snprintf(launchError,sizeof(launchError),"No se pudo iniciar el hilo de conexion."); SDL_UnlockMutex(launchMutex);
    }
}
static void sendGamepadState() {
    if(!hasPublishedStream()) return;
    releaseStaleInputSuppression();
    pollPs4Mouse();

    // Rate-limit the outbound gamepad stream.
    //
    // Field evidence: the main loop runs at 940-945 FPS while streaming
    // (loop_fps=940..945 in the log) and _send_gamepad_input() was called on every
    // single iteration, i.e. ~940 packets/s per controller. Each call takes a
    // recursive mutex and rebuilds the 50-byte payload, so the UI thread spent a
    // large slice of every frame on input bookkeeping. This is what the user sees
    // as "FPS drop to 30-35 while moving the controller, back to 60 when still":
    // stick movement changes the values, which makes every one of those 940 calls
    // take the full payload-building path instead of being nearly free.
    //
    // 125 Hz is the useful ceiling: GeForce NOW samples the remote pad far below
    // this, and it is comfortably above display rate so latency is unaffected.
    constexpr uint64_t kInputPeriodUs = 8000; // 125 Hz
    static uint64_t s_next_input_us = 0;
    const uint64_t now_input_us = getProcessTimeUs();
    if(s_next_input_us == 0 || now_input_us + kInputPeriodUs < s_next_input_us) {
        s_next_input_us = now_input_us; // first call, or resync after a long stall
    }
    if(now_input_us < s_next_input_us) return; // too soon: skip this iteration
    s_next_input_us += kInputPeriodUs;

    // Vigilancia de frescura del estado del mando.
    //
    // El sintoma de la 2.81 fue: el juego abrio bien, y tras ~132 s el mando dejo de
    // responder mientras la app seguia enviando paquetes a 112 Hz con los ejes a cero
    // durante 18,5 s. No habia ningun error en el log, asi que el fallo era silencioso.
    //
    // Esto NO arregla la causa (aun no la conozco), pero la hace visible: si el estado que
    // se va a enviar no ha cambiado en mas de un segundo Y ademas esta todo a cero, se
    // registra una vez por segundo. La proxima vez sabremos si el mando se queda mudo, si
    // la rama de envio es la equivocada, o si el estado se lee pero llega vacio.
    {
        static uint64_t s_last_axes_hash=0;
        static uint64_t s_last_change_us=0;
        static uint64_t s_last_stale_log_us=0;
        const uint64_t axes_hash =
            (static_cast<uint64_t>(ps4PadState.leftStick.x & 0xFFFF) << 48) ^
            (static_cast<uint64_t>(ps4PadState.leftStick.y & 0xFFFF) << 32) ^
            (static_cast<uint64_t>(ps4PadState.rightStick.x & 0xFFFF) << 16) ^
            static_cast<uint64_t>(ps4PadState.rightStick.y & 0xFFFF) ^
            (static_cast<uint64_t>(ps4PadState.buttons) << 16);
        if(axes_hash!=s_last_axes_hash) {
            s_last_axes_hash=axes_hash;
            s_last_change_us=now_input_us;
        } else if(s_last_change_us!=0 && now_input_us-s_last_change_us>1000000ull &&
                  now_input_us-s_last_stale_log_us>1000000ull) {
            s_last_stale_log_us=now_input_us;
            char detail[176];
            snprintf(detail,sizeof(detail),
                     "stale_ms=%llu branch=%d padReady=%d connected=%d buttons=0x%08X axes=%d,%d,%d,%d",
                     static_cast<unsigned long long>((now_input_us-s_last_change_us)/1000ull),
                     inputDevice, ps4PadReady?1:0, ps4PadState.connected?1:0,
                     ps4PadState.buttons,
                     ps4PadState.leftStick.x,ps4PadState.leftStick.y,
                     ps4PadState.rightStick.x,ps4PadState.rightStick.y);
            opennow::LogAppLifecycleEvent("STREAM_INPUT_STALE",detail);
        }
    }

    if(inputDevice==1) {
        if(ps4PadReady) {
            const uint32_t p=ps4PadState.buttons;
            const float lx=calibratedStickValue(ps4PadState.leftStick.x,0,true),ly=calibratedStickValue(ps4PadState.leftStick.y,1,true);
            const float rx=calibratedStickValue(ps4PadState.rightStick.x,2,true),ry=calibratedStickValue(ps4PadState.rightStick.y,3,true);
            const uint32_t gameButtons=ORBIS_PAD_BUTTON_UP|ORBIS_PAD_BUTTON_DOWN|ORBIS_PAD_BUTTON_LEFT|ORBIS_PAD_BUTTON_RIGHT|
                ORBIS_PAD_BUTTON_L1|ORBIS_PAD_BUTTON_R1|ORBIS_PAD_BUTTON_L2|ORBIS_PAD_BUTTON_R2|
                ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3|ORBIS_PAD_BUTTON_CROSS|ORBIS_PAD_BUTTON_CIRCLE|
                ORBIS_PAD_BUTTON_SQUARE|ORBIS_PAD_BUTTON_TRIANGLE|ORBIS_PAD_BUTTON_OPTIONS;
            if(streamSuppressInputUntilNeutral && (p&gameButtons)==0 && ps4PadState.analogButtons.l2==0 &&
               ps4PadState.analogButtons.r2==0 && fabsf(lx)<0.08f && fabsf(ly)<0.08f && fabsf(rx)<0.08f && fabsf(ry)<0.08f)
                streamSuppressInputUntilNeutral=false;
            const bool blockInput=streamMenuVisible||streamSuppressInputUntilNeutral;
            const bool touchDown=(p&ORBIS_PAD_BUTTON_TOUCH_PAD)!=0,touchActive=ps4PadState.touch.fingers>0;
            if(touchActive) {
                if(touchpadMouseActive && !blockInput) {
                    const int dx=std::clamp((static_cast<int>(ps4PadState.touch.touch[0].x)-touchpadLastX)/4,-320,320);
                    const int dy=std::clamp((static_cast<int>(ps4PadState.touch.touch[0].y)-touchpadLastY)/4,-240,240);
                    if(dx||dy) activeStream->send_mouse_move(static_cast<int16_t>(dx),static_cast<int16_t>(dy));
                }
                touchpadLastX=ps4PadState.touch.touch[0].x; touchpadLastY=ps4PadState.touch.touch[0].y;
            }
            if(touchDown!=touchpadMousePressed) {
                if(!blockInput || !touchDown) activeStream->send_mouse_left_button(touchDown);
                touchpadMousePressed=touchDown;
            }
            touchpadMouseActive=touchActive;
        }
        return;
    }
    for(int i=0;i<8;++i) {
        SDL_GameController* pad=gameControllers[i];
        if(!pad || (ps4PadReady && i==0)) continue;
        uint16_t b=0;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_UP)) b|=0x0001;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b|=0x0002;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b|=0x0004;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b|=0x0008;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_LEFTSTICK)) b|=0x0040;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSTICK)) b|=0x0080;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b|=0x0100;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b|=0x0200;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_START)) b|=0x0010;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_BACK)) b|=0x0020;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_GUIDE)) b|=0x0400;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_A)) b|=0x1000;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_B)) b|=0x2000;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_X)) b|=0x4000;
        if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_Y)) b|=0x8000;
        if(i==0) {
            const bool chord=(b&0x8040)==0x8040;
            const bool forcedChord = chord && (b&0x0010)==0x0010;
            if(forcedChord) {
                if(s_forcedExitChordSince == std::chrono::steady_clock::time_point{}) {
                    s_forcedExitChordSince = std::chrono::steady_clock::now();
                } else if(std::chrono::steady_clock::now() - s_forcedExitChordSince >= std::chrono::milliseconds(2000)) {
                    s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_EXIT_FORCED_HOTKEY", "source=l3_triangle_options");
                    leaveStream();
                    return;
                }
            } else if(!ps4PadReady) {
                s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
            }

            if(chord && !streamMenuChordWasDown) {
                if(page==5) {
                    g_streamExitPromptActive = !g_streamExitPromptActive;
                    s_menuChordReleasedSinceOpen = false;
                    if(g_streamExitPromptActive) {
                        updateVideoOutMenuOverlay();
                        s_menuActionsPrevButtons = b;
                    }
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(g_streamExitPromptActive);
                    opennow::LogAppLifecycleEvent(g_streamExitPromptActive ? "STREAM_MENU_OPEN" : "STREAM_MENU_CLOSE",
                                                  "source=dualshock_l3_triangle");
                    suppressStreamInputBriefly();
                } else {
                    streamMenuVisible=!streamMenuVisible; streamMenuSelection=0;
                    opennow::LogAppLifecycleEvent("STREAM_MENU_TOGGLE","source=dualshock_l3_triangle");
                }
            }
            if(!chord) {
                s_menuChordReleasedSinceOpen = true;
            }
            streamMenuChordWasDown=chord;

            if(page==5 && g_streamExitPromptActive && s_menuChordReleasedSinceOpen) {
                const uint16_t menu_edges = b & ~s_menuActionsPrevButtons;

                if(menu_edges & 0x1000) { // [X] Reanudar juego
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_RESUME", "source=sdl_cross");
                    suppressStreamInputBriefly();
                } else if(menu_edges & 0x2000) { // [O] Salir al catalogo (dejar viva)
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_EXIT_CATALOG", "source=sdl_circle keep_alive=1");
                    leaveStream(false);
                    return;
                } else if(menu_edges & 0x8000) { // [Triangulo] Cerrar sesion GFN
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_STOP_SESSION", "source=sdl_triangle");
                    leaveStream(true);
                    return;
                }
            }
            s_menuActionsPrevButtons = b;
        }
        const int rawLx=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX),rawLy=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY);
        const int rawRx=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTX),rawRy=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTY);
        const float lx=i==0?calibratedStickValue(rawLx,0,false):rawLx/32767.0f;
        const float ly=i==0?calibratedStickValue(rawLy,1,false):rawLy/32767.0f;
        const float rx=i==0?calibratedStickValue(rawRx,2,false):rawRx/32767.0f;
        const float ry=i==0?calibratedStickValue(rawRy,3,false):rawRy/32767.0f;
        const int rawLt=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT),rawRt=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        const uint8_t lt=i==0?calibratedTriggerValue(rawLt,0,false):static_cast<uint8_t>(std::clamp((rawLt+32768)*255/65535,0,255));
        const uint8_t rt=i==0?calibratedTriggerValue(rawRt,1,false):static_cast<uint8_t>(std::clamp((rawRt+32768)*255/65535,0,255));
        // Consume only the actual local shortcut chord; send either button normally otherwise.
        if(streamMenuChordWasDown) b=static_cast<uint16_t>(b&~0x8040);
        if(i==0 && streamSuppressInputUntilNeutral && b==0 && lt==0 && rt==0 &&
           fabsf(lx)<0.08f && fabsf(ly)<0.08f && fabsf(rx)<0.08f && fabsf(ry)<0.08f)
            streamSuppressInputUntilNeutral=false;
        const bool blockInput=streamMenuVisible || streamSuppressInputUntilNeutral;
        if(blockInput) b=0;
        else if(g_streamExitPromptActive) b=static_cast<uint16_t>(b&~(0x1000|0x2000|0x4000|0x8000));
        activeStream->send_gamepad_input(static_cast<uint8_t>(i),1,b,
            blockInput?0:lt,blockInput?0:rt,
            blockInput?0.0f:lx,blockInput?0.0f:ly,
            blockInput?0.0f:rx,blockInput?0.0f:ry);
    }
    for(int i=0;i<8;++i) {
        SDL_Joystick* pad=genericJoysticks[i];
        if(!pad || (ps4PadReady && i==0) || SDL_GameControllerFromInstanceID(SDL_JoystickInstanceID(pad))) continue;
        OrbisPadData freshPs4{};
        const bool useFreshPs4=i==0 && readFreshSdlPs4State(pad,freshPs4);
        uint16_t b=0;
        const Uint8 hat=SDL_JoystickNumHats(pad)>0?SDL_JoystickGetHat(pad,0):0;
        if((hat&SDL_HAT_UP) || (SDL_JoystickNumButtons(pad)>13 && SDL_JoystickGetButton(pad,13))) b|=0x0001;
        if((hat&SDL_HAT_DOWN) || (SDL_JoystickNumButtons(pad)>14 && SDL_JoystickGetButton(pad,14))) b|=0x0002;
        if((hat&SDL_HAT_LEFT) || (SDL_JoystickNumButtons(pad)>15 && SDL_JoystickGetButton(pad,15))) b|=0x0004;
        if((hat&SDL_HAT_RIGHT) || (SDL_JoystickNumButtons(pad)>16 && SDL_JoystickGetButton(pad,16))) b|=0x0008;
        if(SDL_JoystickNumButtons(pad)>11 && SDL_JoystickGetButton(pad,11)) b|=0x0040;
        if(SDL_JoystickNumButtons(pad)>12 && SDL_JoystickGetButton(pad,12)) b|=0x0080;
        if(SDL_JoystickNumButtons(pad)>4 && SDL_JoystickGetButton(pad,4)) b|=0x0100;
        if(SDL_JoystickNumButtons(pad)>5 && SDL_JoystickGetButton(pad,5)) b|=0x0200;
        if(SDL_JoystickNumButtons(pad)>9 && SDL_JoystickGetButton(pad,9)) b|=0x0010;
        if(SDL_JoystickNumButtons(pad)>17 && SDL_JoystickGetButton(pad,17)) b|=0x0020;
        if(SDL_JoystickNumButtons(pad)>0 && SDL_JoystickGetButton(pad,0)) b|=0x1000;
        if(SDL_JoystickNumButtons(pad)>1 && SDL_JoystickGetButton(pad,1)) b|=0x2000;
        if(SDL_JoystickNumButtons(pad)>2 && SDL_JoystickGetButton(pad,2)) b|=0x4000;
        if(SDL_JoystickNumButtons(pad)>3 && SDL_JoystickGetButton(pad,3)) b|=0x8000;
        if(i==0) {
            const bool chord=(b&0x8040)==0x8040;
            const bool forcedChord = chord && (b&0x0010)==0x0010;
            if(forcedChord) {
                if(s_forcedExitChordSince == std::chrono::steady_clock::time_point{}) {
                    s_forcedExitChordSince = std::chrono::steady_clock::now();
                } else if(std::chrono::steady_clock::now() - s_forcedExitChordSince >= std::chrono::milliseconds(2000)) {
                    s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_EXIT_FORCED_HOTKEY", "source=l3_triangle_options");
                    leaveStream();
                    return;
                }
            } else if(!ps4PadReady) {
                s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
            }

            if(chord && !streamMenuChordWasDown) {
                if(page==5) {
                    const bool opening = !g_streamExitPromptActive;
                    g_streamExitPromptActive = !g_streamExitPromptActive;
                    s_menuChordReleasedSinceOpen = false;
                    if(g_streamExitPromptActive) {
                        updateVideoOutMenuOverlay();
                        s_menuActionsPrevButtons = b;
                    } else {
                        // Closing with the same chord must re-arm the action edge
                        // detector, otherwise the next menu open sees stale edges
                        // and the following button press is swallowed.
                        s_menuActionsPrevButtons = 0;
                    }
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(g_streamExitPromptActive);
                    opennow::LogAppLifecycleEvent(g_streamExitPromptActive ? "STREAM_MENU_OPEN" : "STREAM_MENU_CLOSE",
                                                  "source=joystick_l3_triangle");
                    // Suppress input only while OPENING. Suppressing on close left
                    // the game unresponsive right when the player wanted to resume,
                    // which the user described as the game "pausing".
                    if(opening) suppressStreamInputBriefly();
                } else {
                    streamMenuVisible=!streamMenuVisible; streamMenuSelection=0;
                    opennow::LogAppLifecycleEvent("STREAM_MENU_TOGGLE","source=joystick_l3_triangle");
                }
            }
            if(!chord) {
                s_menuChordReleasedSinceOpen = true;
            }
            streamMenuChordWasDown=chord;

            if(page==5 && g_streamExitPromptActive && s_menuChordReleasedSinceOpen) {
                const uint16_t menu_edges = b & ~s_menuActionsPrevButtons;
                s_menuActionsPrevButtons = b;

                if(menu_edges & 0x1000) { // [X] Reanudar
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_RESUME", "source=joystick_cross");
                    suppressStreamInputBriefly();
                } else if(menu_edges & 0x2000) { // [O] Salir al catalogo
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_EXIT_CATALOG", "source=joystick_circle keep_alive=1");
                    leaveStream(false);
                    return;
                } else if(menu_edges & 0x8000) { // [Triangulo] Cerrar sesion GFN
                    g_streamExitPromptActive = false;
                    opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                    opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_STOP_SESSION", "source=joystick_triangle");
                    leaveStream(true);
                    return;
                }
            } else {
                s_menuActionsPrevButtons = b;
            }
        }
        auto axis=[&](int n)->float {
            if(useFreshPs4) {
                const uint8_t raw=n==0?freshPs4.leftStick.x:n==1?freshPs4.leftStick.y:n==2?freshPs4.rightStick.x:freshPs4.rightStick.y;
                return calibratedStickValue(raw,n,true);
            }
            const int mapped=genericStickAxis(n);
            if(SDL_JoystickNumAxes(pad)<=mapped) return 0.0f;
            const int raw=SDL_JoystickGetAxis(pad,mapped);
            return i==0?calibratedStickValue(raw,n,false):std::clamp(raw/32767.0f,-1.0f,1.0f);
        };
        const int triggerAxisL=genericTriggerAxis(0),triggerAxisR=genericTriggerAxis(1);
        const int16_t ta=useFreshPs4?freshPs4.analogButtons.l2:(SDL_JoystickNumAxes(pad)>triggerAxisL?SDL_JoystickGetAxis(pad,triggerAxisL):0);
        const int16_t tb=useFreshPs4?freshPs4.analogButtons.r2:(SDL_JoystickNumAxes(pad)>triggerAxisR?SDL_JoystickGetAxis(pad,triggerAxisR):0);
        uint8_t lt=useFreshPs4?calibratedTriggerValue(ta,0,true):genericTriggerValue(ta,0,i);
        uint8_t rt=useFreshPs4?calibratedTriggerValue(tb,1,true):genericTriggerValue(tb,1,i);
        // Keep analog travel when SDL exposes it; use the corresponding digital button only as fallback.
        const int l2Rest=genericTriggerRestValue(0,i),r2Rest=genericTriggerRestValue(1,i);
        if(!useFreshPs4 && SDL_JoystickNumButtons(pad)>18 && SDL_JoystickGetButton(pad,18) && std::abs(static_cast<int>(ta)-l2Rest)<128) lt=255;
        if(!useFreshPs4 && SDL_JoystickNumButtons(pad)>19 && SDL_JoystickGetButton(pad,19) && std::abs(static_cast<int>(tb)-r2Rest)<128) rt=255;
        if(streamMenuChordWasDown) b=static_cast<uint16_t>(b&~0x8040);
        if(i==0 && streamSuppressInputUntilNeutral && b==0 && lt==0 && rt==0 &&
           fabsf(axis(0))<0.08f && fabsf(axis(1))<0.08f && fabsf(axis(2))<0.08f && fabsf(axis(3))<0.08f)
            streamSuppressInputUntilNeutral=false;
        const bool blockInput=streamMenuVisible || streamSuppressInputUntilNeutral;
        if(blockInput) b=0;
        else if(g_streamExitPromptActive) b=static_cast<uint16_t>(b&~(0x1000|0x2000|0x4000|0x8000));
        activeStream->send_gamepad_input(static_cast<uint8_t>(i),1,b,
            blockInput?0:lt,blockInput?0:rt,
            blockInput?0.0f:axis(0),blockInput?0.0f:axis(1),
            blockInput?0.0f:axis(2),blockInput?0.0f:axis(3));
    }
    if(ps4PadReady) {
        const uint32_t p=ps4PadState.buttons;
        uint16_t b=0;
        if(p&ORBIS_PAD_BUTTON_UP) b|=0x0001;
        if(p&ORBIS_PAD_BUTTON_DOWN) b|=0x0002;
        if(p&ORBIS_PAD_BUTTON_LEFT) b|=0x0004;
        if(p&ORBIS_PAD_BUTTON_RIGHT) b|=0x0008;
        if(p&ORBIS_PAD_BUTTON_L3) b|=0x0040;
        if(p&ORBIS_PAD_BUTTON_R3) b|=0x0080;
        if(p&ORBIS_PAD_BUTTON_L1) b|=0x0100;
        if(p&ORBIS_PAD_BUTTON_R1) b|=0x0200;
        if(p&ORBIS_PAD_BUTTON_OPTIONS) b|=0x0010;
        if(p&ORBIS_PAD_BUTTON_TOUCH_PAD) b|=0x0020;
        if(p&ORBIS_PAD_BUTTON_CROSS) b|=0x1000;
        if(p&ORBIS_PAD_BUTTON_CIRCLE) b|=0x2000;
        if(p&ORBIS_PAD_BUTTON_SQUARE) b|=0x4000;
        if(p&ORBIS_PAD_BUTTON_TRIANGLE) b|=0x8000;
        const bool chord=(p&(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_TRIANGLE))==(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_TRIANGLE);
        if(chord) b=static_cast<uint16_t>(b&~0x8040);
        const float lx=calibratedStickValue(ps4PadState.leftStick.x,0,true),ly=calibratedStickValue(ps4PadState.leftStick.y,1,true);
        const float rx=calibratedStickValue(ps4PadState.rightStick.x,2,true),ry=calibratedStickValue(ps4PadState.rightStick.y,3,true);
        const uint8_t lt=calibratedTriggerValue(ps4PadState.analogButtons.l2,0,true),rt=calibratedTriggerValue(ps4PadState.analogButtons.r2,1,true);
        if(streamSuppressInputUntilNeutral && b==0 && lt==0 && rt==0 && fabsf(lx)<0.08f && fabsf(ly)<0.08f && fabsf(rx)<0.08f && fabsf(ry)<0.08f)
            streamSuppressInputUntilNeutral=false;
        const bool blockInput=streamMenuVisible||streamSuppressInputUntilNeutral;
        const bool touchDown=(p&ORBIS_PAD_BUTTON_TOUCH_PAD)!=0;
        const bool touchActive=ps4PadState.touch.fingers>0;
        if(touchActive) {
            if(touchpadMouseActive && !blockInput) {
                const int dx=std::clamp((static_cast<int>(ps4PadState.touch.touch[0].x)-touchpadLastX)/4,-320,320);
                const int dy=std::clamp((static_cast<int>(ps4PadState.touch.touch[0].y)-touchpadLastY)/4,-240,240);
                if(dx||dy) activeStream->send_mouse_move(static_cast<int16_t>(dx),static_cast<int16_t>(dy));
            }
            touchpadLastX=ps4PadState.touch.touch[0].x; touchpadLastY=ps4PadState.touch.touch[0].y;
        }
        if(touchDown!=touchpadMousePressed) {
            if(!blockInput || !touchDown) activeStream->send_mouse_left_button(touchDown);
            touchpadMousePressed=touchDown;
        }
        touchpadMouseActive=touchActive;
        if(blockInput) b=0;
        else if(g_streamExitPromptActive) b=static_cast<uint16_t>(b&~(0x1000|0x2000|0x4000|0x8000));
        activeStream->send_gamepad_input(0,1,b,blockInput?0:lt,blockInput?0:rt,
            blockInput?0.0f:lx,blockInput?0.0f:ly,blockInput?0.0f:rx,blockInput?0.0f:ry);
    }
}
struct StopSessionArgs { opennow::AuthSession auth; std::string session_id; };
static int stopCloudSession(void* opaque) {
    std::unique_ptr<StopSessionArgs> args(static_cast<StopSessionArgs*>(opaque));
    try { opennow::GfnClient client; client.StopSession(args->auth,args->session_id); } catch(...) {}
    return 0;
}
static void cancelPreparedLaunch() {
    if(launchState.load()==1) {
        launchCancelled.store(true);
        opennow::LogAppLifecycleEvent("GAME_LAUNCH_CANCEL_REQUESTED","source=full_screen_load");
        return;
    }
    if(launchState.load()==2) {
        StopSessionArgs* args=new StopSessionArgs();
        SDL_LockMutex(authMutex); args->auth=authSession; SDL_UnlockMutex(authMutex);
        SDL_LockMutex(launchMutex); args->session_id=pendingGameSession.session_id; SDL_UnlockMutex(launchMutex);
        if(!args->session_id.empty()) {
            if(stopThread) { SDL_WaitThread(stopThread,NULL); stopThread=NULL; }
            stopThread=SDL_CreateThread(stopCloudSession,"gfn-stop",args);
            if(!stopThread) delete args;
        } else delete args;
        opennow::LogAppLifecycleEvent("CLOUD_SESSION_STOP_REQUESTED","source=full_screen_load");
    }
    launchState.store(0);
    page=3;
}
static void leaveStream(bool close_cloud_session) {
    // Al salir de la partida se DEVUELVE el hilo principal a su prioridad original. Si no, los menus
    // seguirian con prioridad alta y le quitarian CPU a la red sin ninguna ganancia (el motivo de
    // subirla es la contencion con el decodificador, que deja de existir al cerrar la sesion).
    setStreamingThreadPriority(false);
    if(streamStartState.load()==1) return;
    joinAndAdoptStreamWorker();
    if(hasPublishedStream()) { activeStream->stop(); activeStream.reset(); }
    if(g_videoOutHandoffDone) {
        updateVideoOutExitCard(!close_cloud_session);
        opennow::PS4VideoOutRenderer::BlitExitCardAndFlip();
        opennow::LogAppLifecycleEvent("VIDEOOUT_BEFORE_SHUTDOWN", "transition_card_submitted=1");
        opennow::PS4VideoOutRenderer::Shutdown();
        opennow::LogAppLifecycleEvent("VIDEOOUT_AFTER_SHUTDOWN", "handle_closed=1");
        restoreSdlFromVideoOut();
        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_RESTORE_SDL", "reason=session_ended");
    } else {
        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_SKIP_RESTORE", "reason=no_handoff_was_done");
    }
    g_videoOutHandoffDone = false;
    videoOutDirectActive = false;
    s_lastPresentedGeneration = 0;
    g_streamExitPromptActive = false;
    streamMenuChordWasDown = false;
    s_menuChordReleasedSinceOpen = false;
    s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
    opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_RESET", "videoOutHandoffDone=0 videoOutDirectActive=0");
    streamStartState.store(0);
    page=3; launchState.store(0);

    if(!close_cloud_session) {
        // [O] Salir al catalogo: mantiene la sesion de GFN viva en la nube
        g_activeRecoverableSession = !activeSessionId.empty();
        opennow::LogAppLifecycleEvent("STREAM_EXIT_KEEP_SESSION_ALIVE", ("session_id=" + activeSessionId).c_str());
        return;
    }

    g_activeRecoverableSession = false;
    if(activeSessionId.empty()) return;
    if(stopThread) { SDL_WaitThread(stopThread,NULL); stopThread=NULL; }
    StopSessionArgs* args=new StopSessionArgs();
    SDL_LockMutex(authMutex); args->auth=authSession; SDL_UnlockMutex(authMutex);
    args->session_id=activeSessionId; activeSessionId.clear();
    stopThread=SDL_CreateThread(stopCloudSession,"gfn-stop",args);
    if(!stopThread) delete args;
}
static void signOutGfnAccount() {
    opennow::LogAppLifecycleEvent("AUTH_SIGNOUT_REQUESTED");
    cancelGfnLogin();
    if(authThread) { SDL_WaitThread(authThread,NULL); authThread=NULL; }
    if(settingsRegionThread) { SDL_WaitThread(settingsRegionThread,NULL); settingsRegionThread=nullptr; }
    if(hasPublishedStream()) leaveStream();
    if(stopThread) { SDL_WaitThread(stopThread,NULL); stopThread=NULL; }
    try { opennow::GfnClient client; client.ClearSavedSession(); }
    catch(...) { opennow::LogAppLifecycleEvent("AUTH_SIGNOUT_VAULT_CLEAR_FAILED"); }
    SDL_LockMutex(authMutex); authSession=opennow::AuthSession{}; authChallenge=opennow::QrLoginChallenge{}; authError.clear(); SDL_UnlockMutex(authMutex);
    authState.store(0); catalogState.store(0); catalogCount.store(0); catalogVisibleCount=0;
    std::remove(kAccountCatalogSnapshotPath); std::remove(kAccountCatalogSnapshotTempPath);
    std::remove(kFullCatalogSnapshotPath); std::remove(kFullCatalogSnapshotTempPath);
    { std::lock_guard<std::mutex> lock(settingsRegionsMutex); settingsRegions.clear(); }
    settingsRegionState.store(0);
    page=4; selection=0;
    opennow::LogAppLifecycleEvent("AUTH_SIGNOUT_COMPLETE");
}
static void backFromCatalog() {
    if(launchState.load()==1) {
        launchCancelled.store(true);
        opennow::LogAppLifecycleEvent("GAME_LAUNCH_CANCEL_REQUESTED");
        return;
    }
    if(launchState.load()==2) {
        leaveStream();
        opennow::LogAppLifecycleEvent("CLOUD_SESSION_STOP_REQUESTED");
        return;
    }
    page=0; selection=0;
}
static void activate() {
    opennow::LogAppLifecycleEvent("UI_ACTIVATE",("page="+std::to_string(page)+" selection="+std::to_string(selection)).c_str());
    opennow::TraceAppAction("UI_ACTIVATE",("page="+std::to_string(page)+" selection="+std::to_string(selection)).c_str());
    if(page==1 && settingsRegionPickerVisible) { commitSettingsRegionSelection(); return; }
    if (page==0) {
        // =========================================================================================
        // DISPATCH DE LA X EN EL CENTRO DE JUEGO
        // =========================================================================================
        // ESTE BLOQUE ERA LA CAUSA DE LOS DOS FALLOS REPORTADOS. Tenia los indices escritos a mano:
        //
        //     if(selection==0) ... else if(selection==1) ... else if(selection==4) page=7;
        //     ... else if(selection==5 && !hasPublishedStream()) page=8;   <-- ACERCA DE
        //
        // Cuando la tabla del dibujado paso a tener cinco tarjetas, la rama de "ACERCA DE" pedia
        // `selection==5` o `selection==6`, numeros que ya no se producian: era CODIGO MUERTO y la
        // opcion desaparecio del menu. Y las ramas que SI existian apuntaban a la tarjeta equivocada,
        // que es el desfase que se veia en pantalla.
        //
        // AHORA se lee la MISMA tabla que dibuja las tarjetas. El indice que se ilumina y el que se
        // ejecuta son, por construccion, el mismo: no hay ningun numero que pueda desincronizarse.
        HomeCard cards[kHomeCardCount];
        buildHomeCards(cards, authState.load()==3, hasPublishedStream());
        const int index=(selection>=0 && selection<kHomeCardCount)?selection:0;

        // LOG DE VERIFICACION. Deja en el log QUE tarjeta cree la app que se ha pulsado. Si alguna vez
        // vuelve a haber un desfase entre el mando y la pantalla, aqui estara la prueba exacta.
        {
            char detail[192];
            snprintf(detail,sizeof(detail),"MENU_NAV index=%d title=\"%s\"",index,cards[index].title);
            opennow::LogAppLifecycleEvent("MENU_NAV",detail);
        }

        switch(cards[index].action) {
            case HomeAction::kServiceProbe:   page=2; startServiceProbe(); break;
            case HomeAction::kSettings:       page=1; break;
            case HomeAction::kLogin:
                page=4;
                if(authState.load()!=3) startGfnLogin();
                break;
            case HomeAction::kCatalog:        page=3; startCatalogFetch(); break;
            case HomeAction::kControllerTest: page=7; break;
            case HomeAction::kAbout:          page=8; break;
        }
        // La seleccion vuelve a la primera tarjeta al salir del menu. Se reinicia tambien la
        // animacion (ver mas abajo): si no, el resalte se quedaba en la tarjeta anterior y parecia
        // que el foco estaba "atrapado" en una opcion equivocada.
        selection=0;
        homeSelectionAnimReset();
    }
    else if (page==2) { startServiceProbe(); }
    else if (page==1 && selection==2) { resolution=(resolution+1)%3; }

    else if (page==1 && selection==11) {
        opennow::LogAppLifecycleEvent("REGION_PICKER_OPEN","source=settings; fetch=async");
        opennow::TraceAppAction("REGION_PICKER_OPEN","source=settings");
        startSettingsRegionFetch();
        settingsRegionSelection=0;
        if(streamRegion!="Auto") {
            std::lock_guard<std::mutex> lock(settingsRegionsMutex);
            for(size_t i=0;i<settingsRegions.size();++i) if(settingsRegions[i].url==streamRegion) { settingsRegionSelection=static_cast<int>(i)+1; break; }
        }
        settingsRegionPickerVisible=true;
    }
    else if (page==1 && selection==12) startSpeedTest();
    // Fila 7: modo de decodificador (AUTO / HARDWARE / SOFTWARE).
    else if (page==1 && selection==7) {
        decoderMode=(decoderMode+1)%3;
        opennow::LogAppLifecycleEvent("SETTINGS_DECODER_CHANGED",
            (std::string("mode=")+std::to_string(decoderMode)).c_str());
        // Al elegir HARDWARE se intenta solicitar el recurso del decodificador por ARBITRACION.
        //
        // POR QUE AQUI Y NO EN EL ARRANQUE: en la 3.11 la llamada estaba en el arranque y la
        // aplicacion dejaba de arrancar (el log se cortaba tras BOOT_WINDOW_SIZE, con "Se produjo un
        // error en la aplicacion"), porque las firmas de arbitracion estan DEDUCIDAS y no
        // verificadas. Detras de una accion explicita del usuario, el arranque es SIEMPRE seguro y,
        // si la llamada terminase el proceso, se sabria que la causa exacta fue esta accion.
        //
        // El intento se registra ANTES de llamar para que quede constancia aunque no regrese.
        if (decoderMode==1) {
            // PROBE DEL DECODIFICADOR POR HARDWARE (API v2) con la secuencia completa.
            //
            // NO se llama a la arbitracion (libSceVideoDecoderArbitration): sus firmas NO estan
            // verificadas y fueron justo lo que dejo la aplicacion sin arrancar en la 3.11. Las
            // firmas de `sceVideodec2*` SI lo estan (coinciden con prosper), asi que se usan
            // directamente y son las que de verdad seleccionan el decodificador por hardware: la v1
            // solo probaba `codecType` contra OTRA API, y por eso devolvia 0x80C10001 siempre.
            //
            // Se ejecuta aqui y no en el arranque por la misma leccion: detras de una accion
            // explicita, el arranque es seguro y el intento queda registrado paso a paso.
            opennow::LogAppLifecycleEvent("HWVDEC_ATTEMPT",
                                          "trigger=user_selected_hardware api=v1_then_v2");
            // v1 primero (su modulo carga), v2 despues (con guardia).
            const opennow::videodec1_probe::VideoDecodeV1ProbeResult pr1 =
                opennow::videodec1_probe::RunVideoDecodeV1Probe();
            (void)pr1;
            const opennow::videodec2_probe::HardwareDecodeProbeResult pr =
                opennow::videodec2_probe::RunHardwareDecodeProbe();
            (void)pr;
        }
    }
    // Fila 8: resolucion FIJA del framebuffer (720P / 1080P).
    else if (page==1 && selection==8) {
        scaleMode=(scaleMode+1)%2;
        opennow::LogAppLifecycleEvent("SETTINGS_FRAMEBUFFER_RESOLUTION_CHANGED",
            (std::string("mode=")+std::to_string(scaleMode)+
             " name="+(scaleMode==1?"1080p_fixed":"720p_fixed")).c_str());
    }
    // Fila 9: realce de nitidez del escalador. Se cicla con izquierda/derecha; el valor se guarda
    // con el resto de ajustes y se envia al renderizador al arrancar el stream.
    else if (page==1 && selection==9) {
        sharpnessLevel=(sharpnessLevel+1)%4;
        opennow::LogAppLifecycleEvent("SETTINGS_SHARPNESS_CHANGED",
            (std::string("level=")+std::to_string(sharpnessLevel)).c_str());
    }
    // Fila 13: volver a mostrar el aviso de version beta.
    //
    // Existe porque el aviso se puede silenciar para siempre ("NO VOLVER A MOSTRAR ESTE
    // AVISO") y hasta ahora no habia ninguna forma de recuperarlo desde la app: una vez
    // marcado, el texto de advertencia quedaba inaccesible. Aqui se vuelve a mostrar sin
    // tocar la preferencia guardada, para que quien lo active siga sin verlo al arrancar.
    else if (page==1 && selection==13) {
        betaDisclaimerVisible = true;
        opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_REOPENED", "source=settings_important_read");
    }
    else if (page==1 && selection==14) { if(saveSettings()) { page=0; selection=0; } }
    else if (page==3) {
        if(launchState.load()==1) { launchCancelled.store(true); }
        else if(launchState.load()==2) startPreparedStream();
        else if(g_activeRecoverableSession && !activeSessionId.empty() && !pendingGameSession.session_id.empty()) {
            opennow::LogAppLifecycleEvent("ACTIVE_SESSION_BANNER_RECONNECT", ("session_id=" + activeSessionId).c_str());
            g_activeRecoverableSession = false;
            launchState.store(2);
            startPreparedStream();
        }
        else if(catalogState.load()==2 && authState.load()==3 && catalogSelection<catalogVisibleCount) {
            if(g_activeRecoverableSession && !activeSessionId.empty()) {
                // If user selected a DIFFERENT game while an active recoverable session existed, clean up previous first
                leaveStream(true);
                g_activeRecoverableSession = false;
            }
            startSelectedGame();
        }
        else startCatalogFetch();
    }
    else if (page==4 && selection==0) {
        if(authState.load()==3) signOutGfnAccount();
        else if(authState.load()==1 || authState.load()==2) cancelGfnLogin();
        else startGfnLogin();
    }
    else if (page==4 && selection==1) { cancelGfnLogin(); page=0; selection=2; }
    else if(page==4 && selection==2) {
        const bool previous=rememberLogin; rememberLogin=!rememberLogin;
        if(!saveSettings()) { rememberLogin=previous; savedRememberLogin=previous; }
        else if(!rememberLogin) { try { opennow::GfnClient client; client.ClearSavedSession(); } catch(...) {} }
        opennow::LogAppLifecycleEvent("AUTH_REMEMBER_TOGGLE",rememberLogin?"enabled":"disabled");
    }
    else if(page==7) { /* The tester exits only after holding Circle. */ }
    else if(page==8) { page=0; selection=0; }
    else if(page==6) {
        if(launchState.load()==2) startPreparedStream();
        else if(launchState.load()==3) startSelectedGame();
    }
}
static void pollNativePad() {
    if(!ps4PadReady) return;
    ps4PadPrevious=ps4PadState;
    const int rc=scePadReadState(ps4PadHandle,&ps4PadState);
    if(rc<0) {
        // UN FALLO AQUI ERA INVISIBLE, y es la principal sospecha del sintoma "el mando deja
        // de responder": si scePadReadState empieza a fallar, ps4PadState se queda con el
        // ultimo valor valido y la app sigue enviando ese estado congelado al servidor. El
        // log de la 2.81 lo muestra: a los 132 s los ejes pasan a 0/0/0/0 y se quedan asi
        // 18,5 segundos hasta cerrar la app, sin ningun error registrado.
        //
        // Se registra el PRIMER fallo siempre (para tener el instante exacto) y despues uno
        // de cada 500, para no inundar el log.
        static uint32_t s_readFailCount=0;
        static bool s_firstFailLogged=false;
        ++s_readFailCount;
        if(!s_firstFailLogged || s_readFailCount%500==0) {
            s_firstFailLogged=true;
            char detail[144];
            snprintf(detail,sizeof(detail),
                     "rc=0x%08X handle=0x%08X count=%u frozen_buttons=0x%08X",
                     static_cast<unsigned>(rc),static_cast<unsigned>(ps4PadHandle),
                     s_readFailCount,ps4PadState.buttons);
            opennow::LogAppLifecycleEvent("NATIVE_PAD_READ_FAIL",detail);
        }
        return;
    }
    // Lectura correcta: se comprueba que el mando siga reportandose conectado. Un mando
    // desconectado puede seguir dando rc=0 con datos a cero, que es justo el sintoma visto.
    static bool s_connectedLogged=true;
    if(!ps4PadState.connected && s_connectedLogged) {
        s_connectedLogged=false;
        opennow::LogAppLifecycleEvent("NATIVE_PAD_DISCONNECTED","source=scePadReadState connected=0");
    } else if(ps4PadState.connected && !s_connectedLogged) {
        s_connectedLogged=true;
        opennow::LogAppLifecycleEvent("NATIVE_PAD_RECONNECTED","source=scePadReadState connected=1");
    }
    const uint32_t now=ps4PadState.buttons, before=ps4PadPrevious.buttons;
    if(now!=before) {
        char detail[96]; snprintf(detail,sizeof(detail),"buttons changed=0x%08X pressed=0x%08X released=0x%08X",
                                  now,now&~before,before&~now);
        opennow::TraceAppAction("NATIVE_PAD",detail);
    }
    auto down=[&](uint32_t mask){ return (now&mask)!=0; };
    auto pressed=[&](uint32_t mask){ return (now&mask)!=0 && (before&mask)==0; };
    // Beta disclaimer owns all input until accepted, so no navigation can happen
    // behind the modal.
    if(betaDisclaimerVisible) {
        if(pressed(ORBIS_PAD_BUTTON_SQUARE)) {
            betaDisclaimerDontShowAgain=!betaDisclaimerDontShowAgain;
            opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_TOGGLE",
                betaDisclaimerDontShowAgain?"dont_show_again=1":"dont_show_again=0");
        } else if(pressed(ORBIS_PAD_BUTTON_CROSS) || pressed(ORBIS_PAD_BUTTON_CIRCLE)) {
            betaDisclaimerVisible=false;
            // Persist the choice: saveSettings() already owns settings.cfg, so the
            // preference survives without a second writer to the same file.
            if(!saveSettings()) {
                opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_SAVE_FAILED","reason=save_settings");
            }
            opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_ACCEPTED",
                betaDisclaimerDontShowAgain?"dont_show_again=1":"dont_show_again=0");
        }
        return;
    }
    if(page==7) {
        if(pressed(ORBIS_PAD_BUTTON_SQUARE) && beginInputCalibration())
            opennow::LogAppLifecycleEvent("GAMEPAD_CALIBRATION_ACTION","source=scePad_square");
        if(down(ORBIS_PAD_BUTTON_CIRCLE)) {
            if(!testerCircleHeld) { testerCircleHeld=true; testerCircleSince=std::chrono::steady_clock::now(); }
            else if(std::chrono::steady_clock::now()-testerCircleSince>=std::chrono::milliseconds(900)) {
                // Al salir del probador, el foco vuelve a SU tarjeta. Antes era `selection=4` escrito
                // a mano: correcto con la tabla de hoy, pero apuntaria a otra tarjeta si la tabla
                // cambiara de orden. Buscando por accion eso no puede pasar.
                page=0; selection=homeIndexFor(HomeAction::kControllerTest); testerCircleHeld=false;
                opennow::LogAppLifecycleEvent("GAMEPAD_TEST_EXIT","method=hold_circle");
            }
        } else testerCircleHeld=false;
        return;
    }
    if(page==1 && settingsRegionPickerVisible) {
        if(pressed(ORBIS_PAD_BUTTON_UP)) nav(-1);
        else if(pressed(ORBIS_PAD_BUTTON_DOWN)) nav(1);
        if(pressed(ORBIS_PAD_BUTTON_CROSS)) commitSettingsRegionSelection();
        else if(pressed(ORBIS_PAD_BUTTON_CIRCLE)) { settingsRegionPickerVisible=false; opennow::LogAppLifecycleEvent("REGION_PICKER_CLOSE","source=cancel"); }
        return;
    }
    if(page==5 && streamStartState.load()==3 && (pressed(ORBIS_PAD_BUTTON_CIRCLE) || pressed(ORBIS_PAD_BUTTON_CROSS))) {
        opennow::LogAppLifecycleEvent("STREAM_ERROR_DISMISS","source=scePad_startup_error");
        leaveStream();
        return;
    }
    if(page==5) {
        if(streamStartState.load(std::memory_order_acquire)!=2) return;
        if(!hasPublishedStream()) return;
        if(hasPublishedStream() && activeStream->is_terminal()) {
            if(pressed(ORBIS_PAD_BUTTON_CIRCLE) || pressed(ORBIS_PAD_BUTTON_CROSS)) {
                opennow::LogAppLifecycleEvent("STREAM_ERROR_DISMISS","source=scePad");
                leaveStream();
            }
            return;
        }
        const uint32_t chord=ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_TRIANGLE;
        const uint32_t forcedExitChord = chord | ORBIS_PAD_BUTTON_OPTIONS;
        if((now & forcedExitChord) == forcedExitChord) {
            if(s_forcedExitChordSince == std::chrono::steady_clock::time_point{}) {
                s_forcedExitChordSince = std::chrono::steady_clock::now();
            } else if(std::chrono::steady_clock::now() - s_forcedExitChordSince >= std::chrono::milliseconds(2000)) {
                s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
                g_streamExitPromptActive = false;
                opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                opennow::LogAppLifecycleEvent("STREAM_EXIT_FORCED_HOTKEY", "source=l3_triangle_options");
                leaveStream();
                return;
            }
        } else {
            s_forcedExitChordSince = std::chrono::steady_clock::time_point{};
        }
        const bool chordNow = (now&chord)==chord;
        if(chordNow && (before&chord)!=chord) {
            if(!g_streamExitPromptActive) {
                g_streamExitPromptActive = true;
                s_menuChordReleasedSinceOpen = false;
                updateVideoOutMenuOverlay();
                opennow::PS4VideoOutRenderer::SetInGameMenuActive(true);
                opennow::LogAppLifecycleEvent("STREAM_MENU_OPEN", "source=scePad_l3_triangle");
                opennow::LogAppLifecycleEvent("STREAM_EXIT_PROMPT", "input_filtered_all_actions=1");
                // Suppress only while opening, so closing with the same chord
                // leaves the game immediately responsive.
                suppressStreamInputBriefly();
            } else {
                g_streamExitPromptActive = false;
                s_menuActionsPrevButtons = 0; // Re-arm the action edge detector.
                opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                opennow::LogAppLifecycleEvent("STREAM_MENU_CLOSE", "source=scePad_l3_triangle_toggle");
            }
        }
        if(!chordNow) {
            s_menuChordReleasedSinceOpen = true;
        }
        streamMenuChordWasDown = chordNow;

        if(g_streamExitPromptActive && s_menuChordReleasedSinceOpen) {
            if(pressed(ORBIS_PAD_BUTTON_CROSS)) { // [X] Reanudar
                g_streamExitPromptActive = false;
                opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_RESUME", "source=scePad_cross");
                suppressStreamInputBriefly();
                return;
            } else if(pressed(ORBIS_PAD_BUTTON_CIRCLE)) { // [O] Salir al catalogo (mantener sesion viva)
                g_streamExitPromptActive = false;
                opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_LEAVE_ALIVE", "source=scePad_circle");
                leaveStream(false);
                return;
            } else if(pressed(ORBIS_PAD_BUTTON_SQUARE)) { // [Cuadrado] Alternar HUD
                opennow::PS4VideoOutRenderer::ToggleStatsHud();
                const bool hudNow = opennow::PS4VideoOutRenderer::IsStatsHudActive();
                opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_TOGGLE_HUD", ("enabled=" + std::to_string(hudNow ? 1 : 0)).c_str());
                suppressStreamInputBriefly();
                return;
            } else if(pressed(ORBIS_PAD_BUTTON_TRIANGLE)) { // [Triangulo] Cerrar sesion GFN
                g_streamExitPromptActive = false;
                opennow::PS4VideoOutRenderer::SetInGameMenuActive(false);
                opennow::LogAppLifecycleEvent("STREAM_MENU_ACTION_STOP_SESSION", "source=scePad_triangle");
                leaveStream(true);
                return;
            }
        }
        return;
    }
    if(pressed(ORBIS_PAD_BUTTON_UP)) nav(-1);
    else if(pressed(ORBIS_PAD_BUTTON_DOWN)) nav(1);
    if(pressed(ORBIS_PAD_BUTTON_LEFT)) moveHorizontal(-1);
    else if(pressed(ORBIS_PAD_BUTTON_RIGHT)) moveHorizontal(1);
    if(pressed(ORBIS_PAD_BUTTON_TRIANGLE) && page==3) {
        if(g_activeRecoverableSession && !activeSessionId.empty()) {
            opennow::LogAppLifecycleEvent("ACTIVE_SESSION_BANNER_DISCARD", ("session_id=" + activeSessionId).c_str());
            leaveStream(true); // Close session on GFN
            g_activeRecoverableSession = false;
        } else {
            openCatalogSearchKeyboard();
        }
    }
    if(pressed(ORBIS_PAD_BUTTON_R1) && page==3) cycleCatalogFilter(1);
    else if(pressed(ORBIS_PAD_BUTTON_L1) && page==3) cycleCatalogFilter(-1);
    if((pressed(ORBIS_PAD_BUTTON_L3)||pressed(ORBIS_PAD_BUTTON_R3)) && page==3) toggleSelectedCatalogFavorite();
    if(pressed(ORBIS_PAD_BUTTON_SQUARE) && page==3 && catalogSearchActive) eraseCatalogSearchChar();
    if(pressed(ORBIS_PAD_BUTTON_CROSS)) { if(page!=7) activate(); }
    if(pressed(ORBIS_PAD_BUTTON_CIRCLE) && page!=7) {
        if(page==1 && settingsRegionPickerVisible) { settingsRegionPickerVisible=false; }
        else if(page==1) { discardSettings(); page=0; selection=0; }
        else if(page==3) backFromCatalog();
        else if(page==6) cancelPreparedLaunch();
        else { if(page==4) cancelGfnLogin(); page=0; selection=0; }
    }
    const int axes[4]={static_cast<int>(ps4PadState.leftStick.x)-128,static_cast<int>(ps4PadState.leftStick.y)-128,
                       static_cast<int>(ps4PadState.rightStick.x)-128,static_cast<int>(ps4PadState.rightStick.y)-128};
    if(page!=5) {
        const bool left=axes[0]<-62,right=axes[0]>62,up=axes[1]<-62,downAxis=axes[1]>62;
        if(left && !ps4PadAxisLatch[0]) moveHorizontal(-1);
        if(right && !ps4PadAxisLatch[1]) moveHorizontal(1);
        if(up && !ps4PadAxisLatch[2]) nav(-1);
        if(downAxis && !ps4PadAxisLatch[3]) nav(1);
        ps4PadAxisLatch[0]=left; ps4PadAxisLatch[1]=right; ps4PadAxisLatch[2]=up; ps4PadAxisLatch[3]=downAxis;
    }
}
static void activateStreamMenu() {
    opennow::LogAppLifecycleEvent("STREAM_MENU_ACTIVATE",("selection="+std::to_string(streamMenuSelection)).c_str());
    switch(streamMenuSelection) {
    case 0: streamMenuVisible=false; suppressStreamInputBriefly(); statsLastSampleMs=0; break;
    case 1: streamMenuVisible=false; suppressStreamInputBriefly(); statsLastSampleMs=0; page=0; selection=5; opennow::LogAppLifecycleEvent("STREAM_MINIMIZED"); break;
    case 2: streamStatsVisible=!streamStatsVisible; streamMenuVisible=false; suppressStreamInputBriefly(); statsLastSampleMs=0; break;
    case 3: streamMenuVisible=false; leaveStream(); break;
    }
}
// Strict input trap for the beta disclaimer.
//
// The modal is rendered on the home page, so SDL key/joystick events reach
// handleKey()/nav()/activate() unless they are consumed here. The previous guard
// lived in pollNativePad(), which returns immediately when ps4PadReady is false
// (handle<0 in the field log), so on this setup every button press leaked
// straight through to the page underneath and navigated the UI behind the modal.
//
// Returns true when the event was consumed and must not reach the UI.
static bool consumeDisclaimerEvent(const SDL_Event& ev) {
    if(!betaDisclaimerVisible) return false;
    const bool isButtonDown =
        (ev.type==SDL_JOYBUTTONDOWN && !SDL_GameControllerFromInstanceID(ev.jbutton.which)) ||
        ev.type==SDL_CONTROLLERBUTTONDOWN;
    if(isButtonDown) {
        const int button = (ev.type==SDL_JOYBUTTONDOWN)
            ? static_cast<int>(ev.jbutton.button)
            : static_cast<int>(ev.cbutton.button);
        // SDL-PS4 exposes the DualShock 4 layout: 0=Cruz, 1=Circulo, 2=Cuadrado.
        if(button==2) {
            betaDisclaimerDontShowAgain=!betaDisclaimerDontShowAgain;
            opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_TOGGLE",
                ("dont_show_again=" + std::to_string(betaDisclaimerDontShowAgain?1:0) +
                 " source=sdl_joystick").c_str());
        } else if(button==0 || button==1) {
            // Persist first, hide only on success. Hiding unconditionally meant a
            // failed write silently lost the preference and the modal reappeared
            // on the next boot with no trace of why.
            if(saveSettings()) {
                betaDisclaimerVisible=false;
                opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_ACCEPTED",
                    ("dont_show_again=" + std::to_string(betaDisclaimerDontShowAgain?1:0) +
                     " source=sdl_joystick persisted=1").c_str());
            } else {
                opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_SAVE_FAILED",
                    "reason=save_settings; modal_stays_visible=1");
            }
        }
        opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_INPUT_SWALLOWED",
            ("type=button_down button=" + std::to_string(button)).c_str());
        return true;
    }
    switch(ev.type) {
    case SDL_JOYBUTTONUP:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_JOYHATMOTION:
    case SDL_JOYAXISMOTION:
    case SDL_CONTROLLERAXISMOTION:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        // Swallow supporting events too: a stray axis or key must not navigate.
        return true;
    default:
        return false;
    }
}

static void handleKey(SDL_Keycode key) {
    if(page==1 && settingsRegionPickerVisible) {
        if(key==SDLK_UP || key==SDLK_w) nav(-1);
        else if(key==SDLK_DOWN || key==SDLK_s) nav(1);
        else if(key==SDLK_RETURN || key==SDLK_SPACE) commitSettingsRegionSelection();
        else if(key==SDLK_ESCAPE || key==SDLK_BACKSPACE) settingsRegionPickerVisible=false;
        return;
    }
    if(page==3 && key==SDLK_F3) { toggleSelectedCatalogFavorite(); return; }
    if(page==3 && catalogSearchActive) {
        if(key==SDLK_BACKSPACE) eraseCatalogSearchChar();
        else if(key==SDLK_ESCAPE || key==SDLK_RETURN || key==SDLK_KP_ENTER) toggleCatalogSearch();
        else if(key==SDLK_TAB) cycleCatalogFilter((SDL_GetModState()&KMOD_SHIFT)?-1:1);
        else if(key==SDLK_UP) nav(-1);
        else if(key==SDLK_DOWN) nav(1);
        return;
    }
    if (key==SDLK_UP || key==SDLK_w) nav(-1);
    else if (key==SDLK_DOWN || key==SDLK_s) nav(1);
    else if (key==SDLK_LEFT || key==SDLK_a) moveHorizontal(-1);
    else if (key==SDLK_RIGHT || key==SDLK_d) moveHorizontal(1);
    else if (key==SDLK_RETURN || key==SDLK_SPACE) activate();
    else if(page==3 && key==SDLK_TAB) cycleCatalogFilter((SDL_GetModState()&KMOD_SHIFT)?-1:1);
    else if(page==3 && (key==SDLK_y || key==SDLK_SLASH)) toggleCatalogSearch();
    else if (key==SDLK_ESCAPE || key==SDLK_BACKSPACE) {
        if(page==1) discardSettings();
        if(page==4) cancelGfnLogin();
        if(page==6) { cancelPreparedLaunch(); return; }
        page=0; selection=0;
    }
}
static void handleMousePosition(int x,int y) {
    if(page==0) {
        // EL HIT-TEST DEL RATON TIENE QUE USAR LA MISMA REJILLA QUE EL DIBUJADO.
        // Estaba con `ys[]={265,440,615,790}`: CUATRO filas, cuando la rejilla tiene TRES. Con un
        // clic por debajo de y=755 (que no es ninguna tarjeta) `i` podia llegar a 6 u 8, sacando la
        // seleccion del rango valido. Ahora las dos constantes salen del mismo sitio.
        const int xs[2]={120,980};
        const int ys[kHomeRows]={265,440,615};
        for(int i=0;i<kHomeCardCount;i++) {
            const int row=i/2,col=i%2;
            if(x>=xs[col] && x<xs[col]+820 && y>=ys[row] && y<ys[row]+140) { selection=i; break; }
        }
    }
    else if(page==1) {
        // 7 filas, coincidiendo con el dibujado (y=252+i*58, alto 50). Si estos numeros
        // no coinciden con los del panel, el clic de raton selecciona la fila equivocada.
        // Mismas constantes que el dibujado (kRowTop=268, kRowStep=46, kRowH=42). Si no coinciden,
    // el clic de raton selecciona la fila equivocada.
    for(int i=0;i<15;i++) { const int rowY=268+i*42; if(y>=rowY && y<rowY+38) { selection=i; break; } }
    } else if(page==3 && catalogState.load()==2) {
        if(y>=292 && y<360 && x<850) { catalogSearchActive=true; SDL_StartTextInput(); }
        else if(y>=292 && y<360 && x>=850) cycleCatalogFilter();
        else if(y>=390 && y<912) {
            const int row=(y-390)/58;
            const int index=catalogOffset+row;
            if(index<catalogVisibleCount) catalogSelection=index;
        }
    } else selection=0;
}

static void panel(int x,int y,int w,int h,bool active) {
    if(active) {
        // Tarjeta elegida: fondo algo mas claro, un halo tenue por dentro y la barra de acento a la
        // izquierda. El halo se simula con dos rectangulos de distinto alpha, que sale mas barato que
        // cualquier degradado real (que exigiria dibujar linea a linea).
        fill(x,y,w,h,color(28,40,56));
        fillAlpha(x+2,y+2,w-4,h-4,color(38,58,44),90);
        fill(x,y,7,h,color(118,255,66));
        outline(x,y,w,h,color(118,255,66));
        // Linea superior mas brillante: da la sensacion de que la tarjeta "recibe luz".
        fill(x+7,y+2,w-9,2,color(180,255,150));
    } else {
        // Tarjeta en reposo: un pelo mas oscura por abajo que por arriba, para que no parezca plana.
        fill(x,y,w,h,color(23,31,45));
        fillAlpha(x,y+h-6,w,6,color(14,19,28),120);
        outline(x,y,w,h,color(49,62,79));
    }
}

// =============================================================================================
// TRANSICION ENTRE PANTALLAS
// =============================================================================================
// Al cambiar de pagina se dibuja un velo oscuro que se desvanece en ~180 ms. Sirve para dos cosas:
//   1. Tapa el corte seco entre pantallas, que con este estilo de dibujado (todo se repinta cada
//      frame) se nota mucho.
//   2. Da continuidad: el ojo interpreta el cambio como una transicion, no como un salto.
//
// POR QUE UN VELO Y NO UN FUNDIDO CRUZADO DE VERDAD: un fundido cruzado exigiria dibujar las DOS
// pantallas a la vez, y eso DOBLARIA el trabajo de dibujado justo en el momento del cambio. El velo
// cuesta UNA llamada a SDL_RenderFillRect por frame y el efecto se percibe igual.
//
// El progreso es puro calculo sobre el reloj: no asigna memoria ni crea texturas, asi que el coste
// por frame es constante y despreciable.
static int g_transitionFromPage=-1;
static Uint32 g_transitionStartedAt=0;
static const Uint32 kTransitionMs=180;

static void updatePageTransition(int currentPage) {
    if(currentPage!=g_transitionFromPage) {
        g_transitionFromPage=currentPage;
        g_transitionStartedAt=SDL_GetTicks();
    }
}
// 255 = pantalla tapada del todo; 0 = sin velo. Se redondea hacia arriba para que el primer frame
// tras el cambio ya cubra, y no se vea un fogonazo de la pantalla nueva.
static int pageTransitionAlpha() {
    if(g_transitionStartedAt==0) return 0;
    const Uint32 elapsed=SDL_GetTicks()-g_transitionStartedAt;
    if(elapsed>=kTransitionMs) return 0;
    const float progress=1.0f-static_cast<float>(elapsed)/static_cast<float>(kTransitionMs);
    int alpha=static_cast<int>(progress*232.0f);
    if(alpha<0) alpha=0;
    if(alpha>232) alpha=232;
    return alpha;
}

// =================================================================================================
// SUB-ETAPAS DEL DIBUJADO (v3.50) — AMBITO DE FICHERO
// =================================================================================================
// AMBITO DE FICHERO, no dentro de `main()`: las usan tanto `main()` (la etapa STAGE_DRAW) como
// `draw()`, que es una funcion aparte. Puestas dentro de `main()` no serian visibles desde `draw()`.
//
// MOTIVO: el cierre inesperado que se reporto dejo esta traza en `/data/gfnps4/last_stage.txt`:
//
//     stage=5 name=DRAW beat=1037
//
// `STAGE_DRAW` cubre TODA la funcion `draw()`, que es la mas larga del cliente: el camino del menu,
// el camino del stream, la conversion y copia del video, los textos del HUD y la presentacion. Saber
// que murio "dibujando" no acota casi nada.
//
// Con estas sub-etapas, la siguiente traza dira CUAL de las partes fue. Se escriben en el MISMO
// `last_stage.txt` que ya existe, asi que no hace falta ningun mecanismo nuevo: es el que el proyecto
// ya usa para los cierres duros.
//
// Los valores empiezan en 12 para no chocar con el enum de `main()` (que llega hasta 11).
enum : int {
    STAGE_DRAW_MENU_BEGIN    = 12,   // empieza el camino del menu
    STAGE_DRAW_STREAM_BEGIN  = 13,   // empieza el camino del stream
    STAGE_DRAW_STREAM_CLEAR  = 14,   // limpieza del lienzo del stream
    STAGE_DRAW_STREAM_VIDEO  = 15,   // callback: conversion + copia del frame de video
    STAGE_DRAW_STREAM_HUD    = 16,   // textos del HUD del stream
    STAGE_DRAW_MENU_HEADER   = 17,   // cabecera del menu
    STAGE_DRAW_MENU_CONTENT  = 18,   // contenido de la pagina
    STAGE_DRAW_MENU_FOOTER   = 19,   // pie de pagina
    STAGE_DRAW_PRESENT       = 20,   // dentro de presentFrame()

    // =============================================================================================
    // SUB-ETAPAS DENTRO DEL DIBUJADO DEL VIDEO (v3.61)
    // =============================================================================================
    // MOTIVO: la traza de la v3.51 fue
    //
    //     last_stage.txt -> stage=15 name=DRAW_STREAM_VIDEO beat=269
    //
    // **El cierre esta DENTRO del dibujado del video**, que es el avance mas concreto de toda la
    // investigacion del crash. Pero `DRAW_STREAM_VIDEO` cubre tres trabajos distintos, y cada uno
    // tiene una causa y un arreglo diferentes:
    //
    //   - capturar el frame del `AVFrameHolder`
    //   - la **conversion de color y/o el escalado** (`color_simd`)
    //   - la **subida a textura** (`SDL_UpdateTexture`, que copia la superficie entera)
    //   - el `SDL_RenderCopy` al lienzo
    //
    // Con estas marcas, la PROXIMA traza dira cual de los cuatro. Y hay una razon para sospechar de unos
    // mas que de otros: la version que fallo (v3.51) escalaba a 1920x1080 y subia **8,3 MB** por frame,
    // frente a los 3,7 MB de la ruta de resolucion nativa. Es decir, **cuanto mas grande el buffer,
    // antes murio** (beat 1037 -> 497 -> 269 segun se agrandaba el trabajo por frame).
    STAGE_VIDEO_RENDER_ENTER   = 21,  // entrada a drawLatest
    STAGE_VIDEO_RENDER_SCALE   = 22,  // conversion de color / escalado
    STAGE_VIDEO_RENDER_UPLOAD  = 23,  // SDL_UpdateTexture (copia a la superficie interna)
    STAGE_VIDEO_RENDER_COPY    = 24,  // SDL_RenderCopy al lienzo
    STAGE_VIDEO_RENDER_FALLBACK= 25   // ruta clasica de SDL (YUV)
};

static void draw() {
    // =============================================================================================
    // MEDICION DEL TIEMPO DE DIBUJADO, POR FASES
    // =============================================================================================
    // POR QUE SE AÑADE ESTO: la v3.41 bajo a 19 fps y el log solo decia `render_max_ms=51`. Ese
    // numero dice CUANTO tarda el frame, pero NO EN QUE SE VA. Sin saberlo, optimizar es adivinar, y
    // ya he adivinado mal una vez en esta misma serie de cambios.
    //
    // Con esto, el log dice exactamente donde esta el presupuesto:
    //
    //     UI_DRAW_PHASES page=0 clear_us=120 header_us=340 content_us=900 footer_us=50 present_us=12000 total_us=13410
    //
    // COSTE: cuatro llamadas a `getProcessTimeUs()` (una lectura de reloj, sub-microsegundo) y un
    // `snprintf` bloqueado a una linea por segundo. Despreciable frente al frame.
    const uint64_t drawT0 = getProcessTimeUs();
    static uint64_t s_phaseAccum[5] = {0,0,0,0,0};   // clear, cabecera, contenido, pie, present
    static uint64_t s_phaseFrames = 0;
    static uint64_t s_phaseLastLogUs = 0;
    uint64_t drawTPhase = drawT0;

    // =============================================================================================
    // CONTADORES DE PRIMITIVAS (v3.69): se ponen a cero al empezar el frame.
    // =============================================================================================
    // `UI_DRAW_PHASES` dice CUANTO cuesta cada fase; estos contadores dicen POR QUE. Saber que una
    // pantalla gasta 12 ms en el contenido no sirve para arreglarlo si no se sabe si eso son 4 cajas o
    // 60 etiquetas — y **cada primitiva es una copia de memoria en la CPU** con el renderizador
    // software de PS4. Se registran junto a las fases, una linea por segundo.
    g_frameFills = 0;
    g_frameLabels = 0;
    g_frameRects = 0;
    // En modo VideoOut directo, releaseSdlForVideoOut() destruye el renderizador SDL y deja
    // `renderer = nullptr`, pero draw() se sigue llamando cada vuelta del bucle.
    //
    // ATENCION - HISTORIA DE DOS ERRORES, para no repetir ninguno:
    //   1) Sin ninguna guarda, las ramas SDL (aviso beta, pantalla de conexion) desreferencian
    //      nulo y la app MUERE sin traza. Ese fue el crash de la 2.82.
    //   2) Poniendo `if(!renderer) return;` al PRINCIPIO de todo, la funcion sale antes de
    //      llegar al bloque de video directo, que es el UNICO sitio que llama a Present().
    //      Resultado: imagen CONGELADA, audio bien, el bucle gira a 63.000 FPS y solo se
    //      presenta un frame. Eso fue el fallo de la 2.84.
    //
    // La solucion correcta es no salir de la funcion: se marca que no hay SDL y cada bloque
    // que dibuja con SDL se lo salta. El camino de video directo sigue ejecutandose.
    const bool sdlAvailable = (renderer != nullptr);

    // Beta disclaimer modal. Drawn as the very first layer so it stays on top of
    // whatever page is active, and input is intercepted before navigation runs.
    if(sdlAvailable && betaDisclaimerVisible) {
        SDL_SetRenderDrawColor(renderer,10,14,21,255);
        SDL_RenderClear(renderer);
        fill(0,0,W,12,color(118,255,66));
        panel(260,270,1400,520,true);
        label(320,330,"AVISO IMPORTANTE",4);
        label(320,420,"ESTA APLICACION ES UNA VERSION BETA EN ESTADO DE DESARROLLO.",3);
        label(320,485,"PUEDE CONTENER ERRORES Y CAMBIOS SIN AVISO.",2);
        label(320,545,"USALA BAJO TU PROPIA RESPONSABILIDAD.",2);
        label(320,640,betaDisclaimerDontShowAgain?"[X] NO VOLVER A MOSTRAR ESTE AVISO":"[  ] NO VOLVER A MOSTRAR ESTE AVISO",3);
        label(320,700,"CUADRADO: MARCAR / DESMARCAR",2);
        label(320,740,"CRUZ O CIRCULO: ACEPTAR Y CONTINUAR",2);
        presentFrame();
        return;
    }
    if(sdlAvailable && page==5 && streamStartState.load()==1) {
        static bool firstConnectDrawLogged=false;
        if(!firstConnectDrawLogged) opennow::WriteStreamStartupStage("stream_connect_wait_draw_begin");
        SDL_SetRenderDrawColor(renderer,10,14,21,255); SDL_RenderClear(renderer);
        fill(0,0,W,12,color(118,255,66));
        label(120,92,"PREPARANDO TU SESION DE JUEGO",4);
        const SDL_Rect coverCard={120,160,1680,485};
        if(catalogCoverTexture) {
            int imageWidth=0,imageHeight=0;
            SDL_QueryTexture(catalogCoverTexture,nullptr,nullptr,&imageWidth,&imageHeight);
            if(imageWidth>0 && imageHeight>0) {
                const float sourceAspect=static_cast<float>(imageWidth)/imageHeight;
                const float targetAspect=static_cast<float>(coverCard.w)/coverCard.h;
                SDL_Rect source={0,0,imageWidth,imageHeight};
                if(sourceAspect>targetAspect) {
                    source.w=static_cast<int>(imageHeight*targetAspect);
                    source.x=(imageWidth-source.w)/2;
                } else {
                    source.h=static_cast<int>(imageWidth/targetAspect);
                    source.y=(imageHeight-source.h)/2;
                }
                SDL_SetTextureBlendMode(catalogCoverTexture,SDL_BLENDMODE_BLEND);
                SDL_SetTextureAlphaMod(catalogCoverTexture,135);
                SDL_RenderCopy(renderer,catalogCoverTexture,&source,&coverCard);
                SDL_SetTextureAlphaMod(catalogCoverTexture,255);
            }
        }
        SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer,10,14,21,172);
        SDL_RenderFillRect(renderer,&coverCard);
        SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
        outline(coverCard.x,coverCard.y,coverCard.w,coverCard.h,color(49,62,79));
        label(165,194,"GEFORCE NOW",2);
        label(165,245,activeGameTitle.c_str(),4);
        char selectedProfile[96];
        const int launchResolution=resolution>=0 && resolution<=2 ? resolution : 0;
        const int launchBitrate=networkMode==0?std::min(bitrate,15):bitrate;
        snprintf(selectedProfile,sizeof(selectedProfile),"PERFIL: %s / %d MBPS / %d FPS",resolutions[launchResolution],launchBitrate,fps);
        label(165,289,selectedProfile,2);
        panel(120,375,1680,270,false);
        fill(165,428,18,18,color(118,255,66));
        label(215,422,"SERVIDOR DE NVIDIA LISTO",2);
        fill(165,493,18,18,color(255,190,60));
        label(215,487,"CONECTANDO LA SEÃƒÆ’Ã¢â‚¬ËœALIZACION SEGURA",2);
        fill(165,558,18,18,color(49,62,79));
        label(215,552,"ICE, VIDEO Y AUDIO SE ACTIVARAN DESPUES",2);
        label(120,735,"LA PRIMERA CONEXION PUEDE TARDAR UNOS SEGUNDOS.",2);
        presentFrame();
        if(!firstConnectDrawLogged) { opennow::WriteStreamStartupStage("stream_connect_wait_draw_complete"); firstConnectDrawLogged=true; }
        return;
    }
    if(sdlAvailable && page==5 && streamStartState.load()==3) {
        SDL_SetRenderDrawColor(renderer,10,14,21,255); SDL_RenderClear(renderer);
        label(120,120,"NO SE PUDO INICIAR EL STREAM",4);
        char error[180]; SDL_LockMutex(launchMutex); snprintf(error,sizeof(error),"%.150s",launchError); SDL_UnlockMutex(launchMutex);
        label(120,205,error,2);
        label(120,265,"CIRCULO / B: VOLVER AL CATALOGO",2);
        presentFrame(); return;
    }
    if(sdlAvailable && page==5 && !hasPublishedStream()) {
        static bool firstPendingOwnerDrawLogged=false;
        if(!firstPendingOwnerDrawLogged) opennow::WriteStreamStartupStage("stream_pending_owner_draw_begin");
        SDL_SetRenderDrawColor(renderer,10,14,21,255); SDL_RenderClear(renderer);
        panel(120,375,1680,270,false);
        label(165,420,"INICIANDO LA CONEXION SEGURA",3);
        label(165,486,"ESPERANDO A QUE WEBRTC COMPLETE EL INICIO",2);
        label(165,548,"LA INTERFAZ SE ACTIVARA AL RECIBIR EL STREAM",2);
        presentFrame();
        if(!firstPendingOwnerDrawLogged) { opennow::WriteStreamStartupStage("stream_pending_owner_draw_complete"); firstPendingOwnerDrawLogged=true; }
        return;
    }
    // ESTE BLOQUE CONTIENE EL CAMINO DE VIDEO DIRECTO, no solo dibujado SDL.
    //
    // Historia de tres intentos, para que no haya un cuarto:
    //   1) 2.82: sin proteccion, las ramas SDL desreferenciaban renderer nulo -> CRASH.
    //   2) 2.84: `if(!renderer) return;` al principio de draw() -> salia antes de llegar aqui
    //            -> IMAGEN CONGELADA, 1 solo flip.
    //   3) 2.85: protegi el bloque con `sdlAvailable && ...`, y este bloque TAMBIEN contiene
    //            la llamada a Present() (linea ~3844). Con renderer nulo se saltaba entero
    //            -> IMAGEN CONGELADA otra vez, y ahora el mando tampoco llegaba a activarse
    //            porque el handshake de input depende de que el flujo de video avance.
    //
    // La condicion correcta: entrar si hay SDL **o** si estamos en modo VideoOut directo.
    // Este bloque hace dos cosas distintas (dibujar la interfaz y presentar el video) y no se
    // pueden tratar como una sola.
    if((sdlAvailable || (videoOutDirectActive && g_videoOutHandoffDone)) && page==5 && hasPublishedStream()) {
        // Contador de entradas a este bloque. Distingue de un vistazo los dos fallos opuestos que
        // ya hemos tenido: si el contador no sube, el bloque no se alcanza y la imagen esta
        // congelada aunque no haya crash. Se registra una vez por segundo.
        {
            static uint64_t s_streamBlockEntries = 0;
            static uint64_t s_lastBlockLogMs = 0;
            ++s_streamBlockEntries;
            const uint64_t blockNowMs = getProcessTimeUs() / 1000;
            if(blockNowMs - s_lastBlockLogMs >= 1000) {
                s_lastBlockLogMs = blockNowMs;
                char blockDetail[128];
                std::snprintf(blockDetail, sizeof(blockDetail),
                              "entries=%llu sdl=%d direct=%d handoff=%d",
                              static_cast<unsigned long long>(s_streamBlockEntries),
                              sdlAvailable?1:0, videoOutDirectActive?1:0, g_videoOutHandoffDone?1:0);
                opennow::LogAppLifecycleEvent("STREAM_DRAW_BLOCK_ENTRY", blockDetail);
            }
        }
        opennow::SetCurrentStage(STAGE_DRAW_STREAM_BEGIN);
        if(!streamFirstDrawLogged) {
            opennow::WriteStreamStartupStage("stream_ui_first_draw_begin");
            opennow::LogAppLifecycleEvent("STREAM_UI_FIRST_DRAW_BEGIN");
            streamFirstDrawLogged=true;
        }
        // The game HUD is static while both native overlays are hidden. Keep
        // its already-uploaded GLES texture and spend this iteration only on
        // the newest decoded frame instead of rasterizing a full SDL screen.
        if(pigletVideoActive && pigletStaticStreamOverlayReady &&
           !streamStatsVisible && !streamMenuVisible && !activeStream->is_terminal()) {
            AVFrameHolder::instance().get([](AVFrame* frame,uint64_t generation,bool) {
                activeStream->draw(nullptr,W,H,frame,generation);
            });
            presentFrame();
            return;
        }

        bool videoOutFramePresented = false;

        // =============================================================================================
        // LIMPIEZA DEL LIENZO **ANTES** DE DIBUJAR EL VIDEO (no despues)
        // =============================================================================================
        // Aqui es donde tiene que ir: el lienzo esta todavia mostrando el frame ANTERIOR, asi que se
        // borra ahora para que los textos del HUD no se superpongan entre frames; y a continuacion, en
        // el callback de abajo, se dibuja el frame nuevo ENCIMA. Antes se hacia al reves —se dibujaba
        // el video y luego se borraba— y eso dejaba la pantalla en negro.
        //
        // El color de limpieza es `(0,0,0,255)` OPACO a proposito: con `SDL_BLENDMODE_NONE` el alpha se
        // ignora, y poner 0 solo confundiria a quien lea el codigo.
        // =================================================================================================
        // EL CALLBACK DEL FRAME NO PUEDE ESTAR DENTRO DE LA GUARDA DE LA INTERFAZ (v4.24)
        // =================================================================================================
        // ESTE ERA EL ERROR DE LA v4.20, Y CONGELABA LA IMAGEN. Se explica aqui porque es facil de
        // repetir y porque el propio codigo ya avisaba de ello.
        //
        // En la v4.20 se puso `if(sdlDibujaInterfaz)` alrededor de todo el bloque del stream para que la
        // interfaz SDL no se dibujara con `renderer = NULL`. **Eso arreglo el cierre, pero encerro
        // tambien la llamada a `AVFrameHolder::get()` — que es donde vive `Present()`.** En la ruta
        // directa `renderer` y `g_ownCanvas` son NULL, asi que `sdlDibujaInterfaz` es **false**, el
        // callback **nunca se ejecuta**, y por tanto **`Present()` no se llama nunca**.
        //
        // **Resultado: imagen congelada con audio, mando y decodificador perfectamente vivos.** Y los
        // logs de la v4.22 lo confirman sin ambiguedad:
        //
        //     UI_LOOP_PERF loop_fps=62 slow_frames=0        <- el bucle va a 62 fps
        //     STREAM_DRAW_BLOCK_ENTRY entries=2425          <- la app vivio (antes moria a las 221)
        //     VIDEOOUT_FLIP_SUBMIT        1                 <- una sola ventana de 120 flips
        //     SESSION_DUMP_OK  total_calls=1                <- Present() registro UN frame y nunca mas
        //     STREAM_VIDEO_RENDER_FRAME_CALLBACK_BEGIN    0 <- el callback nunca empieza
        //     VIDEOOUT_PRESENT_STALL / TIMEOUT            0 <- el camino de reintento ni se toca
        //
        // **Y el propio proyecto tenia escrito el aviso, justo encima de este bloque:**
        //
        //     "Este bloque hace dos cosas distintas (dibujar la interfaz y presentar el video)
        //      y no se pueden tratar como una sola."
        //
        // La separacion correcta es esta: **el callback del frame va SIEMPRE** (porque dentro se
        // presenta el video en la ruta directa), y **lo que se guarda es solo el dibujado de la
        // interfaz SDL**, que ya tiene ademas su propia comprobacion de `renderer` unas lineas mas abajo.
        const bool sdlDibujaInterfaz = (renderer != nullptr && g_ownCanvas != nullptr);
        if(sdlDibujaInterfaz) {
            opennow::SetCurrentStage(STAGE_DRAW_STREAM_CLEAR);
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer,0,0,0,255);
            SDL_RenderClear(renderer);
            // Marca: el video SI se va a dibujar en este frame (el callback de abajo lo hace). Los bloques
            // de mas abajo la usan para NO volver a limpiar, que es lo que borraba el frame.
            g_streamVideoDrawnThisFrame = 1;
        }

        AVFrameHolder::instance().get([&](AVFrame* frame, uint64_t generation, bool reused) {
            static bool noFrameLogged=false, frameLogged=false;
            if(!frame && !noFrameLogged) {
                opennow::WriteStreamStartupStage("stream_ui_no_frame_yet");
                opennow::LogAppLifecycleEvent("STREAM_UI_NO_FRAME_YET");
                noFrameLogged=true;
            } else if(frame && !frameLogged) {
                opennow::WriteStreamStartupStage("stream_ui_frame_available");
                opennow::LogAppLifecycleEvent("STREAM_UI_FRAME_AVAILABLE");
                frameLogged=true;
            }

            if(frame && !g_streamColorRangeLogged) {
                char colorDetail[96];
                std::snprintf(colorDetail, sizeof(colorDetail), "format=%d color_range=%d colorspace=%d",
                              frame->format, static_cast<int>(frame->color_range), static_cast<int>(frame->colorspace));
                opennow::LogAppLifecycleEvent("FRAME_COLOR_RANGE", colorDetail);
                g_streamColorRangeLogged = true;

                const int req_w = (resolution == 2) ? 1920 : 1280;
                const int req_h = (resolution == 2) ? 1080 : 720;
                const char* res_mode = (resolution == 0) ? "auto" : ((resolution == 1) ? "720p_fixed" : "1080p_fixed");
                char frameResDetail[160];
                std::snprintf(frameResDetail, sizeof(frameResDetail),
                              "requested=%dx%d actual=%dx%d mode=%s",
                              req_w, req_h, frame->width, frame->height, res_mode);
                opennow::LogAppLifecycleEvent("STREAM_RESOLUTION_APPLIED", frameResDetail);
                opennow::trace::StreamEvent("RESOLUCION_APLICADA", frameResDetail);

                // Automatic software-decoder fallback. If the decoder did not hand
                // back hardware frames, this console cannot hold 1080p60: the
                // software decoder measured 35.5 FPS with queueDrops climbing to
                // 384 access units, and the server stepped the stream down to
                // 1280x720 and 960x540 mid-session anyway. Persist 720p so the NEXT
                // session negotiates a stream that matches the output buffer, which
                // removes the per-frame CPU scaling (30.7 ms/frame) entirely.
                const bool hardwareDecoder = hasPublishedStream() && activeStream->uses_hardware_frames();
                if(!hardwareDecoder && resolution == 2) {
                    resolution = 1;
                    const bool saved = saveSettings();
                    char fallbackDetail[192];
                    std::snprintf(fallbackDetail, sizeof(fallbackDetail),
                                  "reason=software_decoder_inadequate_from=1080p to=720p "
                                  "decoder_hw=0 persisted=%d applied=next_session",
                                  saved ? 1 : 0);
                    opennow::LogAppLifecycleEvent("STREAM_RESOLUTION_AUTO_FALLBACK", fallbackDetail);
                } else {
                    opennow::LogAppLifecycleEvent("STREAM_RESOLUTION_KEPT",
                        (std::string("resolution=") + (resolution == 2 ? "1080p" : "720p") +
                         " decoder_hw=" + (hardwareDecoder ? "1" : "0")).c_str());
                }
            }

            // Subfase 2.2b: Handoff limpio SDL -> VideoOut al recibir el primer frame decodificado
            if(videoOutDirectActive && !activeStream->is_terminal() && frame && frame->width > 0 && frame->height > 0) {
                if(!g_videoOutHandoffDone) {
                    char triggerDetail[96];
                    std::snprintf(triggerDetail, sizeof(triggerDetail), "width=%d height=%d format=%d",
                                  frame->width, frame->height, frame->format);
                    opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_TRIGGER_FRAME", triggerDetail);
                    opennow::trace::StreamEvent("TRASPASO_DISPARADO", triggerDetail);

                    const uint64_t t_handoff_start = getProcessTimeUs();

                    // Paso 1: Liberar SDL (destruye renderer, window y cierra BUS_MAIN en libSceVideoOut)
                    releaseSdlForVideoOut();

                    // Paso 2: Inicializar PS4VideoOutRenderer directo en BUS_MAIN
                    const uint64_t t_vo_start = getProcessTimeUs();
                    // =====================================================================================
                    // EL FRAMEBUFFER SE AJUSTA AL TAMANO REAL DEL STREAM (v3.97). ESTO ELIMINA EL ESCALADO.
                    // =====================================================================================
                    // CAMBIO DE FONDO, y con la evidencia del propio proyecto delante
                    // (`docs/investigacion/ANALISIS-DOLPHINPS4-DETALLADO.md` §6.5, valorado como **la mejor
                    // relacion valor/riesgo** del informe, con 65 % de probabilidad de exito):
                    //
                    //   *"Registrar los buffers de VideoOut a 960x540 (la resolucion que el servidor entrega
                    //   siempre) y no convertir en CPU. El escalado a la pantalla lo hace el scaler de
                    //   hardware de VideoOut."*
                    //
                    //   *"Elimina el 82,6 % del coste de CPU del camino critico (de 13.766 us a ~0)."*
                    //
                    //   *"Es una reactivacion, no un desarrollo."*
                    //
                    // LA CLAVE TECNICA (resuelve la unica duda que quedaba): *"El escalado se decide AL
                    // REGISTRAR el buffer, con `sceVideoOutSetBufferAttribute(attr, fmt, tiling, aspect,
                    // width, height, pitch)`, NO en el flip."* Es decir: el comentario de este proyecto que
                    // decia que "VideoOut no puede escalar en el flip" **es correcto y no se contradice** con
                    // que escale: son dos momentos distintos.
                    //
                    // QUE CAMBIA RESPECTO A ANTES:
                    //   ANTES: framebuffer fijo 1280x720; con el servidor a 960x540 -> **escalado en CPU**
                    //          de 8.523-13.766 us por frame (el 51-83 % del presupuesto de 16.666 us).
                    //   AHORA: framebuffer = tamanio del primer frame = **960x540** -> `needs_scaling` es
                    //          **falso** -> la conversion es 1:1 y **el coste de escalado es CERO**.
                    //          VideoOut lleva el buffer a la pantalla con su escalador de hardware.
                    //
                    // POR QUE ES SEGURO, y no es la reactivacion ciega del modo AUTO que se retiro:
                    //   1. **El bucle de 433 adopciones NO puede ocurrir**: se comprobo que no queda
                    //      ningun camino de adopcion dinamica en el codigo, y `Initialize()` se llama
                    //      **una sola vez por sesion** (aqui). El modo AUTO se elimino en la v3.28 y
                    //      `Initialize()` acepta ahora cada resolucion de su tabla **de forma exacta**
                    //      (el redondeo a 1280x720 era justo lo que provocaba el bucle).
                    //   2. **Nunca un framebuffer MAYOR que 1280x720.** El proyecto midio que un
                    //      framebuffer de 1080p **cerro la aplicacion con 1 solo flip** (frente a 119
                    //      flips estables en 720p), y la causa mas probable es el trafico de memoria:
                    //      1920x1080x4 = 7,91 MB por frame a 60 Hz (475 MB/s) frente a 3,69 MB (210 MB/s).
                    //      Aqui el tope es 1280x720, que es el tamano **verificado estable**, y en el caso
                    //      real (960x540) el trafico baja a **2,07 MB por frame (124 MB/s)**.
                    //   3. **Solo puede BAJAR el coste.** El codigo actual del renderizador ya convierte
                    //      1:1 sin escalar cuando los tamanios coinciden; este cambio solo hace que
                    //      coincidan mas a menudo. Si el servidor entregase 1280x720, el comportamiento
                    //      es exactamente el de antes.
                    //   4. Si el servidor entregase algo MENOR que 960x540, se mantiene 960x540 (no se baja
                    //      mas): asi el framebuffer nunca es tan pequeno que el escalador de hardware tenga
                    //      que hacer un salto grande y pierda nitidez de forma apreciable.
                    // =====================================================================================
                    // FRAMEBUFFER FIJO EN 1280x720. REVERSION DEL CAMBIO DE LA v3.97. (v3.98)
                    // =====================================================================================
                    // EN LA v3.97 puse el framebuffer al tamanio real del frame (960x540), para que
                    // `needs_scaling` fuera falso y el **escalado en CPU costara CERO**. Era la propuesta
                    // que la investigacion del proyecto valora como la mejor relacion valor/riesgo
                    // (`ANALISIS-DOLPHINPS4-DETALLADO.md` §6.5: *"elimina el 82,6 % del coste de CPU"*).
                    //
                    // **LA REVIERTO, y el motivo es un dato que encontre al revisarla:**
                    //
                    //     static const SupportedSize kSupported[] = {
                    //         {1920, 1080},   // 1080P FIJO
                    //         {1280, 720},    // 720P FIJO
                    //         {960,  540},    // 540p, por si en el futuro se expone como opcion fija
                    //         {854,  480},    // 480p
                    //     };
                    //
                    // Ese comentario es concluyente: **960x540 (y 854x480) estan en la tabla pero NUNCA
                    // se han probado** — "por si en el futuro se expone" es una entrada preparada, y ese
                    // futuro no llego porque **la adopcion se elimino en la v3.28**. Los DOS tamanios que
                    // SI tienen evidencia de consola son:
                    //
                    //     framebuffer 1920x1080 -> 1 flip    -> SE CERRO
                    //     framebuffer 1280x720  -> 119 flips -> sesion estable
                    //
                    // Y los **dos unicos ejemplos de `sceVideoOutRegisterBuffers` del proyecto** (los
                    // probes de Vulkan y GNM de DolphinPS4) registran **1920x1080**, el tamanio del panel.
                    // Eso apunta a que **el buffer tiene que ser del tamanio del panel**.
                    //
                    // LA PRUEBA DEFINITIVA, en el emulador de referencia del propio proyecto
                    // (`references/emuladores/prosper-main/.../present_extent.hpp:62-64`):
                    //
                    //   *"a pass whose target is the flipped front buffer or any registered scanout has
                    //   its extent pinned to the present extent before it renders (live_renderer.cpp:5060
                    //   sets `native_w/native_h` from `present_width()/present_height()` for `is_vo`), and
                    //   the [byte-size] contract applies where that extent is non-zero, so
                    //   `px_front`/`px_vo` ALWAYS FIT."*
                    //
                    // Traducido: **el `present extent` es el tamanio EXACTO en bytes del buffer
                    // registrado** (`present_width()` devuelve `snapshot.width`, que es el del buffer), la
                    // publicacion exige `frame_bytes == want_bytes` — **coincidencia exacta** — y **no hay
                    // ningun punto donde un frame de tamanio distinto se escale**.
                    //
                    // Es decir: **el buffer define el extent de presentacion, y no se estira.** Un buffer
                    // de 960x540 seria un extent de 960x540 y el video **no llenaria la pantalla**.
                    //
                    // EL RIESGO CONCRETO DE HABERLO DEJADO ASI, por tanto, no era hipotetico: si un
                    // buffer de 960x540 **no se estira**, el video se veria **PEQUENO EN UNA ESQUINA** —
                    // que es exactamente el sintoma que el usuario ha pedido evitar.
                    //
                    // DECISION: **el escalado en CPU de 8.523-13.766 us CABE en el presupuesto de
                    // 16.666 us** (medido por el proyecto) y su resultado **esta verificado en consola**.
                    // Cambiarlo por una ganancia no verificada que puede producir el sintoma que se pide
                    // evitar es un mal intercambio. **Ante duda, lo probado.**
                    //
                    // =====================================================================================
                    // PERO LA EVIDENCIA NO ES UNANIME, Y HAY QUE DECIRLO (v3.99)
                    // =====================================================================================
                    // Despues de escribir lo de arriba encontre DOS pruebas que apuntan en DIRECCIONES
                    // CONTRARIAS, y este es el sitio donde tienen que quedar para que nadie repita la
                    // investigacion:
                    //
                    //   **A favor de que NO se estira** (lo que sostiene la decision):
                    //     - El emulador de referencia (prosper, `present_extent.hpp:62-64`): el extent esta
                    //       "pinned" al del buffer y la publicacion exige coincidencia EXACTA de bytes.
                    //     - El driver SDL-PS4 registra sus buffers **al tamanio del display**, no al de la
                    //       ventana. Si VideoOut estirara cualquier buffer, no necesitaria hacer eso: le
                    //       bastaria registrar el de la ventana.
                    //
                    //   **A favor de que SI se estira** (lo que permitiria el buffer de 540p):
                    //     - El propio analisis del proyecto (`PS4-V3.56` §1) dice: *"ni SDL ni el camino
                    //       directo aprovechan el escalador de hardware"*, con la ruta directa escalando en
                    //       CPU a 720p **sobre un panel de 1080p**. Es decir: **el proyecto observo un
                    //       buffer de 720p sobre un panel de 1080p y no reporto que se viera en una
                    //       esquina** — si VideoOut no estirara, 1280x720 dentro de 1920x1080 seria el 66 %
                    //       del ancho y se habria notado.
                    //
                    //   **Y el experimento que lo habria zanjado no se ejecuto.** `PS4-V3.56` §2 lo dice
                    //   con todas las letras: *"No se puede deducir leyendo codigo: depende de como se
                    //   comporte VideoOut en este firmware con GoldHEN"*, y su sonda
                    //   (`src/probes/videoout_scaler_probe.c`) **se cerro sin escribir ni una linea de
                    //   log**, asi que los dos desenlaces posibles siguen abiertos:
                    //       (a) se presenta ESCALADO a pantalla completa -> el escalador existe
                    //       (b) se presenta pequeno en una esquina      -> no hay escalado automatico
                    //
                    // **COMO QUEDA ESTO:** se mantiene 1280x720, que es **el unico tamanio con evidencia
                    // directa de estabilidad en consola (119 flips)** y el unico cuyo resultado visual no
                    // depende de la pregunta abierta: sea cual sea la respuesta, un buffer de 720p sobre un
                    // panel de 1080p es una configuracion que **el proyecto ya ejecuto sin incidencias**.
                    // Un buffer de 540p, en cambio, **solo es correcto bajo la respuesta (a)**, y bajo la
                    // (b) produce el sintoma que hay que evitar. **Se elige lo que funciona en los dos
                    // casos.**
                    //
                    // Si algun dia se quiere recuperar ese 82,6 %, el camino esta en `src/probes/`: ejecutar
                    // la sonda, mirar la pantalla, y con la respuesta (a) cambiar las dos lineas de abajo.
                    int fbW = 1280, fbH = 720;
                    resolution_to_wh(resolution, fbW, fbH);
                    // Se registra el tamanio elegido y si va a haber escalado, para poder comprobarlo
                    // en el log sin ambiguedad.
                    {
                        char fbDetail[200];
                        std::snprintf(fbDetail, sizeof(fbDetail),
                                      "framebuffer=%dx%d frame_servidor=%dx%d escalado_cpu=%s "
                                      "motivo=tamanio_verificado_estable_119_flips",
                                      fbW, fbH, frame->width, frame->height,
                                      (fbW == frame->width && fbH == frame->height) ? "CERO" : "activo");
                        opennow::LogAppLifecycleEvent("VIDEOOUT_FRAMEBUFFER_SIZE", fbDetail);
                    }
                    const bool initOk = opennow::PS4VideoOutRenderer::Initialize(fbW, fbH);
                    const uint64_t vo_elapsed_us = getProcessTimeUs() - t_vo_start;

                    if(initOk) {
                        g_videoOutHandoffDone = true;
                        char voDetail[64];
                        std::snprintf(voDetail, sizeof(voDetail), "elapsed_us=%llu", static_cast<unsigned long long>(vo_elapsed_us));
                        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_VIDEOOUT_OPEN_OK", voDetail);

                        const uint64_t total_handoff_us = getProcessTimeUs() - t_handoff_start;
                        char totalDetail[64];
                        std::snprintf(totalDetail, sizeof(totalDetail), "elapsed_us=%llu", static_cast<unsigned long long>(total_handoff_us));
                        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_TOTAL_US", totalDetail);

                        // VerificaciÃƒÆ’Ã‚Â³n 2: Verificar subsistemas de entrada
                        const int sdlJoyActive = (SDL_WasInit(SDL_INIT_JOYSTICK) != 0) ? 1 : 0;
                        const int gamepadActive = (ps4PadReady || controllerCount > 0) ? 1 : 0;
                        char inputDetail[96];
                        std::snprintf(inputDetail, sizeof(inputDetail), "sdl_joystick_active=%d gamepad_active=%d",
                                      sdlJoyActive, gamepadActive);
                        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_INPUT_CHECK", inputDetail);

                        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_COMPLETE", "mode=direct_hardware_60fps");
                        opennow::trace::StreamEvent("TRASPASO_COMPLETO", "mode=direct_hardware_60fps");
                        // =============================================================================
                        // TRAZA DEL CIERRE DE LA RUTA DIRECTA (v4.16)
                        // =============================================================================
                        // La ruta directa se activo por defecto en la v3.93 y **se cerraba a los ~2 s**
                        // (`last_stage.txt` = `stage=6 PRESENT`, con un latido de 460 s, justo despues de
                        // este punto). **No se identifico el mecanismo y no se va a inventar:** lo que se
                        // hace es **rodear el traspaso de marcas** para que, si vuelve a ocurrir, el log
                        // diga exactamente en que punto se quedo.
                        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_POST_UI_STATE",
                            (std::string("renderer=") + (renderer?"vivo":"NULO") +
                             " lienzo=" + (g_ownCanvas?"vivo":"NULO") +
                             " ventana=" + (window?"viva":"NULA") +
                             " pagina=" + std::to_string(page)).c_str());
                    } else {
                        opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_FAIL", "fallback=sdl_software");
                        g_videoOutHandoffDone = false;
                        videoOutDirectActive = false; // Degradacion automatica a SDL
                        // =================================================================================
                        // CUARENTENA TAMBIEN AQUI (v4.02). ESTE CAMINO SE HABIA QUEDADO SIN ELLA.
                        // =================================================================================
                        // En la v3.94 puse cuarentena para el fallo PERSISTENTE de flips (3 seguidos ->
                        // `NeedsFullRecreate`), pero **deje este camino sin ella**, y es el MAS probable:
                        // aqui se llega cuando `PS4VideoOutRenderer::Initialize()` devuelve false, es
                        // decir cuando el traspaso SDL -> VideoOut no cuaja (sin memoria directa, sin
                        // handle, o buffers rechazados).
                        //
                        // SIN la marca, el siguiente stream volvia a poner `videoOutDirectActive = true`
                        // (linea del `startPreparedStream`), volvia a intentar el traspaso, y **volvia a
                        // fallar** — una y otra vez, una por cada entrada a jugar, con su `releaseSdl`
                        // y su `restoreSdl` de por medio. No es un cierre, pero **es un tiron en cada
                        // entrada y un intento condenado a fallar**.
                        //
                        // CON la marca, tras el primer fallo de traspaso la app se queda en SDL durante
                        // el resto de la ejecucion. **Si se reinicia la app se reintenta**, que es lo
                        // razonable: el fallo pudo ser de esa sesion (por ejemplo, memoria directa
                        // ocupada por otro proceso en ese momento).
                        g_videoOutQuarantined = true;
                        opennow::LogAppLifecycleEvent("VIDEOOUT_QUARANTINED",
                            "reason=handoff_init_failed alcance=resto_de_la_ejecucion "
                            "siguiente_stream=SDL reinicio_app=reintenta_directo");
                        restoreSdlFromVideoOut();     // Restaurar SDL inmediatamente
                    }
                }

                if(g_videoOutHandoffDone && videoOutDirectActive) {
                    static uint64_t s_last_present_attempt_ms = 0;
                    static uint32_t s_present_retry_count = 0;
                    static uint64_t s_last_stats_push_ms = 0;
                    const uint64_t now_ms = getProcessTimeUs() / 1000;

                    if(now_ms - s_last_stats_push_ms >= 500) {
                        s_last_stats_push_ms = now_ms;
                        const auto net = activeStream->get_network_counters();
                        const int rtt = activeStream->get_network_rtt_ms();
                        opennow::PS4VideoOutRenderer::UpdateLiveStats(
                            statsVideoFps > 0 ? statsVideoFps : 60,
                            statsDecodeFps > 0 ? statsDecodeFps : 60,
                            statsVideoKbps > 0 ? statsVideoKbps : 0,
                            rtt >= 0 ? rtt : 0,
                            frame ? frame->width : 1280,
                            frame ? frame->height : 720
                        );
                    }

                    // =====================================================================================
                    // GATE DE PRESENTACION ENDURECIDO (v4.23). ESTE ERA EL DEFECTO DE FONDO.
                    // =====================================================================================
                    // COMO ESTABA, y por que es fragil:
                    //
                    //     if (reused || generation == s_lastPresentedGeneration) { sleep; return; }  // NO presenta
                    //     if (!reused && generation != s_lastPresentedGeneration) { Present(frame); }
                    //
                    // **La decision de presentar dependia de comparar `generation` con una variable que el
                    // PROPIO CAMINO DE REINTENTO reescribe** (`s_lastPresentedGeneration = generation` en
                    // el bloque de `!presented_ok`, unas lineas mas abajo). Es decir: **si UNA sola vez se
                    // escribe la generacion de un frame que NO se presento, la presentacion se para para
                    // siempre**, porque a partir de ahi esa generacion "ya consta como presentada".
                    //
                    // Y ese es exactamente el sintoma medido en consola, de los logs de la v4.17:
                    //
                    //     VIDEOOUT_HANDOFF_POST_UI_STATE   renderer=NULO lienzo=NULO ventana=NULA
                    //     VIDEOOUT_FLIP_SUBMIT             (1 sola ventana de 120 -> muy pocos flips)
                    //     STREAM_VIDEO_RENDER_FRAME_CALLBACK_COMPLETE   0   <- el callback nunca completa
                    //     session_frames.csv              total_calls=0     <- Present() no registra nada
                    //     ... y aun asi: audio OK, mando OK, decodificador lleno
                    //
                    // **Imagen congelada con audio y mando vivos.** El resto de la aplicacion funciona; lo
                    // unico que se para es la presentacion.
                    //
                    // LA CORRECCION: **presentar siempre que haya un frame PUBLICADO (`!reused`)**. El flag
                    // `reused` ya distingue exactamente los dos casos que importan, y lo pone
                    // `AVFrameQueue::pop()` (`AVFrameHolder.cpp`):
                    //
                    //     reused = false  -> hay un frame nuevo en la cola y se ha adoptado  -> PRESENTAR
                    //     reused = true   -> la cola esta VACIA, o toca mantener el anterior -> no presentar
                    //
                    // Es decir: **`reused` es la unica condicion necesaria y suficiente**, y no depende de
                    // ningun contador que otra rama pueda dejar desincronizado. La comparacion de
                    // generaciones se deja SOLO como diagnostico (para el log de descarte), no como puerta.
                    //
                    // Y `Present()` ya tiene su propio gobierno de 60 Hz por dentro, asi que presentar
                    // siempre que haya frame nuevo no puede desbocar el ritmo de flips.
                    {
                        static uint64_t s_gatePresent = 0, s_gateSkipEmpty = 0;
                        static uint64_t s_gateSkipMismaGen = 0, s_gateLastLogMs = 0;
                        const uint64_t gateNowMs = getProcessTimeUs() / 1000;
                        if(reused) {
                            ++s_gateSkipEmpty;
                        } else {
                            ++s_gatePresent;
                            if(generation == s_lastPresentedGeneration) ++s_gateSkipMismaGen;
                        }
                        if(s_gateLastLogMs == 0 || (gateNowMs - s_gateLastLogMs) >= 1000) {
                            s_gateLastLogMs = gateNowMs;
                            char gd[192];
                            std::snprintf(gd, sizeof(gd),
                                          "presentar=%llu cola_vacia=%llu misma_gen_pero_presentado=%llu "
                                          "gen_actual=%llu ultima_presentada=%llu",
                                          static_cast<unsigned long long>(s_gatePresent),
                                          static_cast<unsigned long long>(s_gateSkipEmpty),
                                          static_cast<unsigned long long>(s_gateSkipMismaGen),
                                          static_cast<unsigned long long>(generation),
                                          static_cast<unsigned long long>(s_lastPresentedGeneration));
                            opennow::LogAppLifecycleEvent("VIDEOOUT_PRESENT_GATE", gd);
                        }
                    }

                    if(reused) {
                        // Solo cuando NO hay frame nuevo: la cola esta vacia (o toca mantener el
                        // anterior). Antes esto tambien se disparaba por una generacion repetida, y ahi
                        // estaba el atasco.
                        sceKernelUsleep(500);
                        return;
                    }

                    {
                        // =============================================================================
                        // UNA FILA POR FRAME EN LA TRAZA DETALLADA (v4.25)
                        // =============================================================================
                        // ESTE ES EL DATO QUE FALTABA. El log periodico dice "loop_fps=62" o "20 fps",
                        // pero **no dice que frame concreto consumio el presupuesto ni por que**. Estas
                        // columnas responden a esa pregunta directamente:
                        //
                        //   gen / reused   -> si la puerta de presentacion dejo pasar el frame o no
                        //   src / dst      -> lo que entrego el servidor y a lo que se convirtio
                        //   scaled         -> 1 si hubo escalado en CPU (el enemigo de los 60 fps)
                        //   pres_us        -> lo que costo presentar  (presupuesto = 16.666)
                        //   over           -> 1 si paso del presupuesto
                        //
                        // Con eso, "por que no van a 60" se lee en el CSV: cuantas filas tienen
                        // `over=1`, y si `scaled` o `pres_us` lo explican.
                        const uint64_t t_present0 = getProcessTimeUs();
                        const bool presented_ok = opennow::PS4VideoOutRenderer::Present(frame);
                        const uint64_t t_present1 = getProcessTimeUs();
                        const uint64_t present_us = (t_present1 > t_present0) ? (t_present1 - t_present0) : 0;
                        // La resolucion real del framebuffer directo, que es el destino de la conversion.
                        int dstW = frame ? frame->width : 0;
                        int dstH = frame ? frame->height : 0;
                        opennow::PS4VideoOutRenderer::GetFramebufferSize(dstW, dstH);
                        const int escalado = (frame && (frame->width != dstW || frame->height != dstH)) ? 1 : 0;
                        opennow::trace::FrameRow(
                            opennow::trace::FrameRowsRecorded(),
                            generation, 0,
                            frame ? frame->width : 0, frame ? frame->height : 0,
                            dstW, dstH, escalado,
                            g_traceDecodeQueue, g_traceDecodeFps, present_us);
                        // Traza: el primer Present() del camino directo ya se ha ejecutado y ha vuelto.
                        // Si el cierre estuviera DENTRO de Present(), esta marca no apareceria nunca.
                        {
                            static bool s_directPresentLogged = false;
                            if(!s_directPresentLogged) {
                                s_directPresentLogged = true;
                                opennow::LogAppLifecycleEvent("VIDEOOUT_HANDOFF_POST_FIRST_PRESENT",
                                    presented_ok ? "ok=1 el_cierre_no_esta_en_Present"
                                                 : "ok=0 Present_devolvio_false");
                            }
                        }
                        if(!presented_ok) {
                            s_present_retry_count++;
                            // The DCE rejected the frame because its flip queue is
                            // saturated. Without an explicit yield here the UI loop
                            // spins at thousands of FPS (loop_fps=15000+ measured),
                            // starving the pad poll thread and flooding the input
                            // channel. Yield one millisecond and retry.
                            static uint32_t s_stall_count = 0;
                            ++s_stall_count;
                            sceKernelUsleep(1000);
                            if(s_stall_count % 240 == 1) {
                                char stallDetail[96];
                                std::snprintf(stallDetail, sizeof(stallDetail),
                                              "busy=%u retries=%u yield_us=1000", s_stall_count, s_present_retry_count);
                                opennow::LogAppLifecycleEvent("VIDEOOUT_PRESENT_STALL", stallDetail);
                            }
                            if(s_last_present_attempt_ms != 0 && now_ms - s_last_present_attempt_ms > 1000) {
                                // =====================================================================
                                // NO MARCAR COMO PRESENTADO UN FRAME QUE NO SE PRESENTO (v4.23)
                                // =====================================================================
                                // Aqui habia `s_lastPresentedGeneration = generation;`. Es decir: tras
                                // **un segundo sin poder presentar**, se daba ese frame por presentado y
                                // **se ponia la marca de reintento a cero**, con lo que el siguiente
                                // intento volvia a empezar de cero sobre el MISMO frame.
                                //
                                // **Ese `s_lastPresentedGeneration = generation` es la via por la que la
                                // presentacion se queda muerta:** si el frame que se marca nunca llego a
                                // la pantalla, a partir de ahi esa generacion consta como presentada y,
                                // con la puerta anterior, **ya no se volvia a presentar nada**. Cuadra con
                                // el sintoma exacto: **imagen congelada, audio y mando vivos**.
                                //
                                // Y este `else` era ademas un bucle: `s_last_present_attempt_ms = now_ms`
                                // significa que el siguiente frame **no** entraba en esta rama y caia en
                                // `return; // Retry on next frame` para siempre.
                                //
                                // LO QUE SE HACE AHORA: no se toca `s_lastPresentedGeneration` (el frame
                                // sigue constando como NO presentado, que es la verdad), se deja el aviso
                                // y **se sigue adelante con el frame actual** en vez de reintentarlo
                                // indefinidamente. La marca de reintento se empuja hacia delante para que
                                // el aviso no se repita en cada frame.
                                s_present_retry_count = 0;
                                s_last_present_attempt_ms = now_ms;
                                opennow::LogAppLifecycleEvent("VIDEOOUT_PRESENT_TIMEOUT",
                                    "forcing_advance=1 sin_marcar_como_presentado=1");
                            } else {
                                if(s_last_present_attempt_ms == 0) s_last_present_attempt_ms = now_ms;
                                return; // Retry on next frame
                            }
                        } else {
                            s_lastPresentedGeneration = generation;
                            s_present_retry_count = 0;
                            s_last_present_attempt_ms = now_ms;
                            videoOutFramePresented = true;
                        }

                        // Persistent flip failure means the display pipe is gone.
                        // Retrying cannot revive it: tear down VideoOut completely
                        // and rebuild SDL, which is where the old code left the
                        // screen black.
                        if(opennow::PS4VideoOutRenderer::NeedsFullRecreate()) {
                            // =============================================================================
                            // MISMA RECUPERACION QUE EL VIGILANTE (v4.31): UNA SOLA IMPLEMENTACION
                            // =============================================================================
                            // Este cuerpo estaba duplicado aqui, palabra por palabra, y la copia del
                            // vigilante es la que estaba incompleta. **Tenerlo dos veces es lo que
                            // permitio que se desincronizaran**, asi que ahora hay una sola funcion y
                            // la llaman los dos disparadores:
                            //
                            //   - aqui, por **fallo persistente de flips** (3 seguidos)
                            //   - en el bucle principal, por **presentacion sin avanzar 3 segundos**
                            recoverFromDirectPathFailure("reason=persistent_flip_failure", /*desdeElCallback=*/true);
                            return; // SDL owns presentation again from this frame
                        }
                    }
                    return; // En modo VideoOut, omitir el pipeline de software SDL
                }
            }

            if(frame && !streamFrameCallbackLogged) {
                opennow::WriteStreamStartupStage("video_render_frame_callback_begin");
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FRAME_CALLBACK_BEGIN",
                    ("generation="+std::to_string(generation)+" format="+std::to_string(frame->format)+
                     " width="+std::to_string(frame->width)+" height="+std::to_string(frame->height)).c_str());
            }
            if(renderer) {
                // =====================================================================================
                // MEDICION DEL DIBUJADO DEL VIDEO, POR SEPARADO DEL RESTO DEL FRAME
                // =====================================================================================
                // POR QUE HACE FALTA ESTA MEDICION CONCRETA:
                //
                // El desglose `UI_DRAW_PHASES` dice que en el stream `present_us` vale 49.870 us y en el
                // menu 5.394 us. La diferencia (44.476 us) es "el video"... pero eso es una INFERENCIA,
                // no una medida: el menu dibuja MAS interfaz y sin embargo es 10 veces mas rapido.
                //
                // Y no vale cualquier hipotesis sobre los pixeles: copiar 2.073.600 px (el blit a
                // 1080p) son ~16 MB de trafico de memoria, que a los anchos de banda de una PS4 son
                // del orden de 1-2 ms, NO 44. Asi que **hay que medir donde se va de verdad**, en lugar
                // de seguir suponiendo. Ya he adivinado mal una vez en esta serie de cambios.
                //
                // Esta marca separa las tres partes:
                //   video_draw_us : la copia de la textura al lienzo (dentro de activeStream->draw)
                //   La subida de la textura (SDL_UpdateYUVTexture) va DENTRO de draw, asi que queda
                //   incluida aqui; si el coste estuviera en la subida, se vera al comparar con el
                //   tamano del frame.
                opennow::SetCurrentStage(STAGE_DRAW_STREAM_VIDEO);
                const uint64_t t_video0 = getProcessTimeUs();
                activeStream->draw(nullptr,W,H,frame,generation);
                const uint64_t t_video1 = getProcessTimeUs();
                {
                    static uint64_t s_videoAccumUs = 0;
                    static uint64_t s_videoFrames = 0;
                    static uint64_t s_videoLastLogUs = 0;
                    s_videoAccumUs += (t_video1 > t_video0) ? (t_video1 - t_video0) : 0;
                    ++s_videoFrames;
                    if(s_videoLastLogUs == 0 || (t_video0 - s_videoLastLogUs) >= 1000000ULL) {
                        s_videoLastLogUs = t_video0;
                        char vd[200];
                        std::snprintf(vd, sizeof(vd),
                                      "video_draw_us=%llu frames=%llu src=%dx%d dst=%dx%d",
                                      static_cast<unsigned long long>(s_videoAccumUs / (s_videoFrames ? s_videoFrames : 1)),
                                      static_cast<unsigned long long>(s_videoFrames),
                                      frame ? frame->width : 0, frame ? frame->height : 0, W, H);
                        opennow::LogAppLifecycleEvent("STREAM_VIDEO_DRAW_US", vd);
                        s_videoAccumUs = 0;
                        s_videoFrames = 0;
                    }
                }
            }
            if(frame && !streamFrameCallbackLogged) {
                opennow::WriteStreamStartupStage("video_render_frame_callback_complete");
                opennow::LogAppLifecycleEvent("STREAM_VIDEO_RENDER_FRAME_CALLBACK_COMPLETE");
                streamFrameCallbackLogged=true;
            }
        });   // fin del callback del frame
        // NOTA (v4.24): aqui NO va ninguna llave de cierre de la interfaz. La guarda
        // `if(sdlDibujaInterfaz)` se aplica ahora **antes** del callback, solo a la limpieza del lienzo,
        // y el resto del dibujado de la interfaz ya tiene su propia comprobacion de `renderer`.
        // Poner aqui un `}` cerraba el bloque del stream y dejaba el callback fuera de sitio.

        // =================================================================================================
        // LA PRESENTACION DIRECTA SE COMPLETA EN EL CALLBACK, NO AQUI
        // =================================================================================================
        // `Present()` se llama DENTRO del callback de arriba (es el unico sitio, `main.cpp`), y eso es lo
        // correcto: el callback es el unico punto donde hay un frame disponible. Lo que se deja aqui es
        // el gobierno del ritmo para el caso en que **no** se presento un frame nuevo.
        if(videoOutFramePresented) {
            // This early return skips the frame-budget limiter further down, which
            // is why the loop measured 940-945 FPS while streaming. The UI thread
            // was burning CPU that the software decoder needed, and it got worse
            // the moment the sticks moved because every one of ~940 gamepad sends
            // per second then took its full payload-building path.
            //
            // Pace against an absolute deadline rather than sleeping a fixed 16 ms:
            // Present() already spent most of the frame, so a fixed sleep would
            // add a whole extra period and drop to ~30 FPS - exactly the symptom
            // reported ("35 FPS while moving").
            constexpr Uint32 kPresentFramePeriodMs = 16;
            static Uint32 s_next_present_slot = 0;
            const Uint32 now_present_ms = SDL_GetTicks();
            if(s_next_present_slot == 0 ||
               static_cast<Sint32>(now_present_ms - s_next_present_slot) > 0) {
                s_next_present_slot = now_present_ms;
            }
            if(static_cast<Sint32>(s_next_present_slot - now_present_ms) > 0) {
                SDL_Delay(s_next_present_slot - now_present_ms);
            }
            s_next_present_slot += kPresentFramePeriodMs;
            return;
        }

        // Direct VideoOut mode owns presentation, and Present() has its own 60 Hz
        // governor. This branch is taken when no new frame was published at all
        // (the decoder is behind), so hold the rest of the current frame slot
        // instead of free-running.
        if(videoOutDirectActive && g_videoOutHandoffDone) {
            constexpr Uint32 kIdleFramePeriodMs = 16;
            static Uint32 s_next_idle_slot = 0;
            const Uint32 now_idle_ms = SDL_GetTicks();
            if(s_next_idle_slot == 0 ||
               static_cast<Sint32>(now_idle_ms - s_next_idle_slot) > 0) {
                s_next_idle_slot = now_idle_ms;
            }
            if(static_cast<Sint32>(s_next_idle_slot - now_idle_ms) > 0) {
                SDL_Delay(s_next_idle_slot - now_idle_ms);
            }
            s_next_idle_slot += kIdleFramePeriodMs;
            return;
        }

        if(!renderer) return;

        // =============================================================================================
        // LA LIMPIEZA DE PANTALLA BORRABA EL VIDEO. ESTE ERA EL FALLO DE LA "PANTALLA NEGRA".
        // =============================================================================================
        // El orden de operaciones en este camino era:
        //
        //     1) `AVFrameHolder::get(...)` -> `activeStream->draw(...)`   <-- SE DIBUJA EL VIDEO
        //     2) SDL_SetRenderDrawColor(renderer,0,0,0,0);
        //        SDL_RenderClear(renderer);                              <-- Y SE BORRA ACTO SEGURO
        //     3) ... textos del HUD ...
        //     4) presentFrame()
        //
        // `SDL_RenderClear` pinta TODO el lienzo con el color de dibujado actual, que era `(0,0,0,0)`.
        // Con `SDL_BLENDMODE_NONE` —el modo por defecto, y el que se acaba de fijar en la linea de
        // arriba— ese color se escribe como **NEGRO OPACO**.
        //
        // Resultado: exactamente el sintoma reportado. **El audio se oye, el video se decodifica, se
        // sube a textura, se copia... y la pantalla queda negra**, porque el frame que se presentaba
        // era el negro de esta limpieza. Los textos del HUD (dibujados DESPUES) si se veian, y por eso
        // parecia que "la UI funciona pero el video no".
        //
        // El log lo confirma: `STREAM_VIDEO_RENDER_FIRST_COPY_COMPLETE` aparece (la copia se ejecuta
        // bien) y `presented` crece. Todo el pipeline funcionaba; el frame se tiraba DESPUES de
        // dibujarlo.
        //
        // LA CORRECCION: si el video se ha dibujado en este frame, NO limpiar despues. La limpieza
        // solo se hace cuando NO hay video que preservar, que es el caso de la pantalla de "LA
        // TRANSMISION SE DETUVO".
        const bool videoDrawnThisFrame = (g_streamVideoDrawnThisFrame != 0);

        if(activeStream->is_terminal()) {
            if(!videoDrawnThisFrame) {
                SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer,0,0,0,255);
                SDL_RenderClear(renderer);
            }
            SDL_SetRenderDrawColor(renderer,10,14,21,255);
            SDL_Rect banner={0,0,W,130}; SDL_RenderFillRect(renderer,&banner);
            label(60,24,"LA TRANSMISION SE DETUVO",3);
            std::string detail=activeStream->get_debug_info();
            const std::string stateKey="State: ";
            const size_t stateAt=detail.find(stateKey);
            std::string state="SIN DETALLE";
            if(stateAt!=std::string::npos) {
                const size_t begin=stateAt+stateKey.size();
                const size_t end=detail.find('\n',begin);
                state=detail.substr(begin,end==std::string::npos?end:end-begin);
                if(state.size()>125) state.resize(125);
            }
            label(60,68,state.c_str(),2);
            label(60,104,"CIRCULO / B: VOLVER AL CATALOGO",2);
        } else {
            // =========================================================================================
            // EL HUD DEL JUEGO YA NO SE DIBUJA DURANTE LA PARTIDA (v3.63)
            // =========================================================================================
            // Aqui estaba:
            //
            //     char title[200];
            //     snprintf(title, sizeof(title), "AJ | %.130s | CIRCULO: JUEGO | L3+TRIANGULO: MENU", ...);
            //     label(20, 12, title, 2);        // <-- SE DIBUJABA SIEMPRE
            //
            // Esa linea se pintaba **en cada frame de la partida**, en la esquina superior izquierda,
            // **encima del juego**. Es exactamente lo que se reporto: *"el texto superior izquierdo
            // se queda pegado en la pantalla tapando el juego"*.
            //
            // Y tenia un efecto secundario peor de lo que parece: el texto se dibuja con
            // `makeTextTexture`, que en un renderizador software **copia la textura del glifo al lienzo
            // en cada frame**, y ademas **obliga a limpiar el lienzo antes** (para que no se
            // superpongan las letras de un frame con las del siguiente). Es decir: el HUD no solo
            // tapaba el juego, **forzaba trabajo extra en el camino del video**.
            //
            // Ahora solo se dibuja cuando el usuario ha pedido ver la interfaz: con las estadisticas
            // o el menu de sesion en pantalla. El resto del tiempo **el lienzo es 100 % video**.
            //
            // NOTA: la condicion se escribe con las dos variables directamente en lugar de usar el
            // alias `uiOverStreamVisible`, que se declara mas abajo en la funcion (solo aplica al pie
            // de pagina de las pantallas de menu). Repetir la condicion aqui evita mover esa
            // declaracion y tocar el camino del menu.
            if(streamStatsVisible || streamMenuVisible) {
                char title[200];
                snprintf(title,sizeof(title),"AJ | %.130s | CIRCULO: JUEGO | L3+TRIANGULO: MENU",activeGameTitle.c_str());
                label(20,12,title,2);
            }
        }
        const Uint32 now=SDL_GetTicks();
        const auto perf=activeStream->get_video_performance();
        // =================================================================================================
        // ULTIMO VALOR CONOCIDO DE LOS CONTADORES DE VIDEO, PARA LA TRAZA DETALLADA (v4.25)
        // =================================================================================================
        // La fila por frame del CSV necesita la profundidad de la cola y los fps del decodificador, y
        // esos contadores solo se consultan aqui (cada 500 ms). Se guardan en variables de fichero para
        // que la fila del CSV los use **con su ultimo valor real** en vez de un cero que parezca un
        // dato. Es mas honesto un valor con hasta 500 ms de antiguedad que un cero.
        g_traceDecodeQueue = static_cast<int>(perf.decode_queue_size);
        {
            static uint64_t s_decPrevFrames = 0;
            static Uint32 s_decPrevMs = 0;
            if (s_decPrevMs != 0 && now > s_decPrevMs) {
                const uint64_t delta = (perf.decoded_frames >= s_decPrevFrames)
                                     ? (perf.decoded_frames - s_decPrevFrames) : 0;
                g_traceDecodeFps = static_cast<int>((delta * 1000u) / (now - s_decPrevMs));
            }
            s_decPrevFrames = perf.decoded_frames;
            s_decPrevMs = now;
        }
        if(streamHealthLogAt==0 || static_cast<Uint32>(now-streamHealthLogAt)>=1000u) {
            streamHealthLogAt=now;
            const auto health=activeStream->get_transport_health();
            const auto net=activeStream->get_network_counters();
            const auto streamElapsed=activeStreamStartedAt.time_since_epoch().count()
                ?std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-activeStreamStartedAt).count():0;
            char detail[240];
            snprintf(detail,sizeof(detail),
                "elapsed_ms=%u decoded=%llu presented=%llu access_units=%llu decode_queue=%zu/%zu rtp_gaps=%u rtp_drops=%u peer_completed=%d video_started=%d renderer=%s",
                static_cast<unsigned>(std::max<long long>(0,streamElapsed)),
                static_cast<unsigned long long>(perf.decoded_frames),
                static_cast<unsigned long long>(perf.presented_frames),
                static_cast<unsigned long long>(perf.access_units),
                perf.decode_queue_size,perf.decode_queue_high_water,
                net.sequence_gaps,net.access_units_dropped,
                health.peer_completed?1:0,health.video_started?1:0,
                pigletVideoActive?"piglet":"sdl_videoout");
            opennow::LogAppLifecycleEvent("STREAM_HEALTH_TICK",detail);
            // =============================================================================================
            // COMO LEER ESTA LINEA: `decoded` vs `presented` NO significa "el decodificador va a 30 fps"
            // =============================================================================================
            // MEDIDO en consola sobre `APP VIDEO access_unit_progress` (v4.27), y corrige una lectura que
            // yo mismo hice mal antes:
            //
            //     3.684 ms ->   120 unidades,  115 frames
            //    26.996 ms -> 2.160 unidades, 2.155 frames
            //
            //     ENTRAN unidades : 87,5 /s
            //     SALEN frames    : 87,5 /s        <- IDENTICOS
            //
            // **El decodificador NO es el cuello de botella: produce 87,5 fps, muy por encima de los 60
            // que hacen falta, y su coste es de 108-118 us por lote.** Ademas `queueDrops=0` y
            // `decodeErrors=0`: no se pierde ni un frame por el camino.
            //
            // **Por tanto, cuando `presented` va muy por debajo de `decoded`, el frame se pierde DESPUES
            // del decodificador — en la presentacion.** Eso descarta de una vez la red, el decodificador
            // y la longitud de la cola, y deja la presentacion como unico sospechoso. Es exactamente lo
            // que senalan las dos causas ya identificadas y corregidas: la puerta de presentacion (v4.23)
            // y el callback encerrado en la guarda (v4.24).
            //
            // Se escribe aqui porque la diferencia `decoded-presented` es el dato mas visible de todo el
            // log y **es facil atribuirla al decodificador, que es justo lo contrario de lo que pasa.**
        }
        if(statsLastSampleMs==0) { statsLastSampleMs=now; statsLastBytes=perf.access_unit_bytes; statsLastPresented=perf.presented_frames; statsLastDecoded=perf.decoded_frames; }
        else if(now-statsLastSampleMs>=1000) {
            const Uint32 elapsed=now-statsLastSampleMs;
            const uint64_t bytes=perf.access_unit_bytes>=statsLastBytes?perf.access_unit_bytes-statsLastBytes:0;
            const uint64_t frames=perf.presented_frames>=statsLastPresented?perf.presented_frames-statsLastPresented:0;
            const uint64_t decoded=perf.decoded_frames>=statsLastDecoded?perf.decoded_frames-statsLastDecoded:0;
            statsVideoKbps=static_cast<int>((bytes*8)/std::max<Uint32>(1,elapsed));
            statsVideoFps=static_cast<int>((frames*1000)/std::max<Uint32>(1,elapsed));
            statsDecodeFps=static_cast<int>((decoded*1000)/std::max<Uint32>(1,elapsed));
            statsSwapFps=pigletVideoActive?static_cast<int>(PS4PigletVideoRenderer::GetSwapFps()):0;
            statsLastSampleMs=now; statsLastBytes=perf.access_unit_bytes; statsLastPresented=perf.presented_frames; statsLastDecoded=perf.decoded_frames;
        }
        if(streamStatsVisible) {
            const int rtt=activeStream->get_network_rtt_ms();
            const auto net=activeStream->get_network_counters();
            SDL_SetRenderDrawColor(renderer,8,13,22,238); SDL_Rect stats={24,74,1260,228}; SDL_RenderFillRect(renderer,&stats);
            SDL_SetRenderDrawColor(renderer,118,255,66,255); SDL_RenderDrawRect(renderer,&stats);
            char row[160];
            snprintf(row,sizeof(row),"RECIBIDO %dP | MAX %dP | DEC %d FPS | PRESENT %d FPS | %d KBPS",activeStream->stream_height(),resolution?1080:720,statsDecodeFps,statsVideoFps,statsVideoKbps); label(42,86,row,2);
            snprintf(row,sizeof(row),"LATENCIA %s MS  |  HUECOS RTP %u",rtt>=0?std::to_string(rtt).c_str():"--",net.sequence_gaps); label(42,120,row,2);
            snprintf(row,sizeof(row),"PAQUETES RTP %u  |  CUADROS DESCARTADOS %u",net.packets_received,net.access_units_dropped); label(42,154,row,2);
            snprintf(row,sizeof(row),"LIMITE %d MBPS  |  CONSUMO ACTUAL %d KBPS  |  ICE/UDP",bitrate,statsVideoKbps); label(42,188,row,2);
            const std::string audio=activeStream->get_audio_debug_info();
            auto audioMetric=[&](const char* key)->unsigned long long { const size_t pos=audio.find(key); return pos==std::string::npos?0ULL:std::strtoull(audio.c_str()+pos+strlen(key),nullptr,10); };
            snprintf(row,sizeof(row),"AUDIO RX %llu  |  DEC %llu  |  SALIDAS %llu  |  DESC %llu",audioMetric("rx="),audioMetric("decoded="),audioMetric("samples="),audioMetric("drops=")); label(42,222,row,2);
            const auto elapsed=activeStreamStartedAt.time_since_epoch().count()?std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-activeStreamStartedAt).count():0;
            snprintf(row,sizeof(row),"SESION %02lld:%02lld  |  RESTANTE NO INFORMADO  |  RENDER %s",static_cast<long long>(elapsed/60),static_cast<long long>(elapsed%60),rendererHardwareAccelerated?"GPU":"SOFTWARE"); label(42,256,row,2);
        }
        if(streamMenuVisible) {
            SDL_SetRenderDrawColor(renderer,8,13,22,245); SDL_Rect menu={540,300,840,470}; SDL_RenderFillRect(renderer,&menu);
            SDL_SetRenderDrawColor(renderer,118,255,66,255); SDL_RenderDrawRect(renderer,&menu);
            label(600,330,"MENU DE SESION",4);
            const char* items[]={"REANUDAR JUEGO","MINIMIZAR Y VOLVER AL INICIO",streamStatsVisible?"ESTADISTICAS: ACTIVADAS":"ESTADISTICAS: DESACTIVADAS","CERRAR SESION DE JUEGO"};
            for(int i=0;i<4;i++) { const int y=410+i*76; panel(590,y,740,62,streamMenuSelection==i); label(620,y+18,items[i],2); }
            label(600,730,"L3 + TRIANGULO: ABRIR / CERRAR MENU   |   X: ELEGIR",2);
        }
        opennow::SetCurrentStage(STAGE_DRAW_PRESENT);
        presentFrame();
        if(streamFirstDrawLogged) {
            static bool firstDrawCompleteLogged=false;
            if(!firstDrawCompleteLogged) {
                opennow::WriteStreamStartupStage("stream_ui_first_draw_complete");
                opennow::LogAppLifecycleEvent("STREAM_UI_FIRST_DRAW_COMPLETE");
                firstDrawCompleteLogged=true;
            }
        }

        // Fases 0 y 4 del camino de stream: limpieza de pantalla y presentacion. En la ruta de juego
        // no hay cabecera, contenido ni pie: todo el presupuesto esta en estas dos.
        s_phaseAccum[0] = 0;   // la limpieza va dentro de activeStream->draw(), no se puede separar
        s_phaseAccum[1] = 0;
        s_phaseAccum[2] = 0;
        s_phaseAccum[3] = 0;
        {
            const uint64_t nowUs = getProcessTimeUs();
            s_phaseAccum[4] += (nowUs > drawTPhase) ? (nowUs - drawTPhase) : 0;
            drawTPhase = nowUs;
        }
        ++s_phaseFrames;
        if(drawT0 - s_phaseLastLogUs >= 1000000ULL || s_phaseLastLogUs == 0) {
            s_phaseLastLogUs = drawT0;
            char phases[320];
            const uint64_t n = s_phaseFrames ? s_phaseFrames : 1;
            // Se registran TAMBIEN los acumuladores CRUDOS (`raw_present_us`, `raw_total_us`) ademas de
            // las medias. Motivo (v3.86): con el video a pantalla completa, `present_us`/`frames` da
            // 2,49 ms por frame mientras el bucle mide 50 ms por vuelta, y esa contradiccion no se puede
            // resolver sin ver el valor sin dividir y el divisor exacto que se ha usado. Si
            // `raw_present_us/n` no cuadra con lo que se ve en pantalla, el fallo esta en la division o
            // en donde se acumula; con los crudos se distingue.
            std::snprintf(phases, sizeof(phases),
                          "page=5 stream=1 frames=%llu present_us=%llu total_us=%llu "
                          "raw_present_us=%llu raw_total_us=%llu cajas=%u etiquetas=%u",
                          static_cast<unsigned long long>(s_phaseFrames),
                          static_cast<unsigned long long>(s_phaseAccum[4] / n),
                          static_cast<unsigned long long>((s_phaseAccum[0]+s_phaseAccum[1]+
                                                           s_phaseAccum[2]+s_phaseAccum[3]+s_phaseAccum[4]) / n),
                          static_cast<unsigned long long>(s_phaseAccum[4]),
                          static_cast<unsigned long long>(s_phaseAccum[0]+s_phaseAccum[1]+
                                                          s_phaseAccum[2]+s_phaseAccum[3]+s_phaseAccum[4]),
                          // La UI que se pinta ENCIMA del video. Debe ser 0 (o casi) durante la partida:
                          // es la comprobacion de que el lienzo es 100 % video, y de que el HUD ya no
                          // tapa el juego (que era uno de los cuatro fallos reportados).
                          g_frameRects, g_frameLabels);
            opennow::LogAppLifecycleEvent("UI_DRAW_PHASES", phases);
            s_phaseAccum[0]=s_phaseAccum[1]=s_phaseAccum[2]=s_phaseAccum[3]=s_phaseAccum[4]=0;
            s_phaseFrames = 0;
        }
        return;
    }
    opennow::SetCurrentStage(STAGE_DRAW_MENU_BEGIN);
    // =============================================================================================
    // ORDEN DE LAS MARCAS DE FASE (v3.68). ANTES ESTABA MAL Y FALSEABA LA MEDIDA.
    // =============================================================================================
    // `UI_DRAW_PHASES` daba `content_us=0` EN TODAS LAS PAGINAS desde que se instrumento. La causa era
    // el ORDEN de las marcas, no el dibujado:
    //
    //   ANTES:  HEADER -> limpieza -> [cabecera + TODA la pagina] -> CONTENT -> FOOTER
    //   AHORA:  HEADER -> limpieza -> cabecera -> CONTENT -> [pagina] -> FOOTER
    //
    // Con el orden antiguo, **el contenido se acumulaba en `s_phaseAccum[1]` (cabecera)** y
    // `s_phaseAccum[2]` (contenido) no se tocaba NUNCA. Por eso `header_us` salia en 8.300-9.800 us en
    // todas las pantallas (una cabecera real son unas bandas opacas y dos textos) mientras
    // `content_us` era siempre 0, y por eso las pantallas lentas no se podian diagnosticar: todo el
    // coste de la pagina aparecia atribuido a la cabecera.
    opennow::SetCurrentStage(STAGE_DRAW_MENU_HEADER);
    SDL_SetRenderDrawColor(renderer,10,14,21,255); SDL_RenderClear(renderer);
    {
        const uint64_t nowUs = getProcessTimeUs();
        s_phaseAccum[0] += (nowUs > drawTPhase) ? (nowUs - drawTPhase) : 0;
        drawTPhase = nowUs;
    }
    updatePageTransition(page);

    // --- CABECERA ---
    // =============================================================================================
    // LA CABECERA COSTABA 384.000 PIXELES MEZCLADOS POR FRAME. AHORA CUESTA 90 LINEAS OPACAS.
    // =============================================================================================
    // Version anterior:
    //
    //     fillAlpha(0,0,W,200,color(16,22,33),90);   -> 1920 x 200 = 384.000 px CON MEZCLA, cada frame
    //     fillAlpha(0,10,W,5,color(118,255,66),70);  -> con mezcla
    //     fillAlpha(0,15,W,4,color(118,255,66),32);  -> con mezcla
    //
    // Con el renderizador SOFTWARE de PS4 eso se paga pixel a pixel en la CPU, y es una de las dos
    // causas de la caida a 19 fps.
    //
    // AHORA:
    //   - El velo de la franja de cabecera se hace con TRAMADO (90 lineas opacas de las 200) en vez de
    //     mezclar 384.000 pixeles. Mismo efecto visual, coste proporcional a las LINEAS.
    //   - Las dos lineas del resplandor verde se sustituyen por UNA sola banda opaca mas estrecha: a
    //     1080p un resplandor de 9 px con degradado no se distingue de una banda de 5 px.
    fill(0,0,W,10,color(118,255,66));                  // barra de acento
    fill(0,10,W,5,color(66,140,38));                    // resplandor: color intermedio, OPACO
    fillAlpha(0,0,W,200,color(16,22,33),90);            // velo tramado: 90 lineas, no 384.000 px
    label(120,72,"AJ / GEFORCE NOW PS4",5);
    label(120,130,"TU BIBLIOTECA, TU SESION, TU PERFIL DE STREAMING",2);
    if(page==0 || page==3) drawAccountBadge();
    // Linea divisoria con una sombra corta debajo.
    fill(120,190,1680,2,color(70,90,115));
    fill(120,192,1680,2,color(12,17,26));
    // =============================================================================================
    // FRONTERA CABECERA -> CONTENIDO (v3.68). ESTA MARCA FALTABA Y FALSEABA LA MEDIDA.
    // =============================================================================================
    // `UI_DRAW_PHASES` daba `content_us=0` EN TODAS LAS PAGINAS desde que se instrumento. La causa era
    // el ORDEN de las marcas, no el dibujado:
    //
    //   ANTES:  HEADER -> limpieza -> [cabecera + TODA la pagina] -> CONTENT -> FOOTER
    //   AHORA:  HEADER -> limpieza -> cabecera -> CONTENT -> [pagina] -> FOOTER
    //
    // Con el orden antiguo **el contenido se acumulaba en `s_phaseAccum[1]` (cabecera)** y
    // `s_phaseAccum[2]` (contenido) no se tocaba NUNCA. Por eso `header_us` salia en 8.300-9.800 us en
    // TODAS las pantallas —cuando una cabecera real son unas bandas opacas y dos textos— y por eso las
    // pantallas lentas no se podian diagnosticar: todo el coste de la pagina aparecia como cabecera.
    //
    // A partir de aqui todo lo que se dibuje (tarjetas, QR, filas de ajustes, lista del catalogo y el
    // velo de transicion animado de `updatePageTransition`) se cuenta como CONTENIDO.
    opennow::SetCurrentStage(STAGE_DRAW_MENU_CONTENT);
    {
        const uint64_t nowUs = getProcessTimeUs();
        s_phaseAccum[1] += (nowUs > drawTPhase) ? (nowUs - drawTPhase) : 0;
        drawTPhase = nowUs;
    }
    if(page==6) {
        logLaunchDrawStage(1,"header");
        label(120,235,"PREPARANDO TU SESION DE JUEGO",4);
        panel(120,305,1680,170,false);
        label(165,330,"GEFORCE NOW",2);
        label(165,370,launchTitle.c_str(),4);
        const int launchResolution=resolution>=0 && resolution<=2 ? resolution : 0;
        const int launchBitrate=networkMode==0?std::min(bitrate,15):bitrate;
        char profile[128]; snprintf(profile,sizeof(profile),"PERFIL: %s / %d MBPS / %d FPS",resolutions[launchResolution],launchBitrate,fps);
        label(165,430,profile,2);
        logLaunchDrawStage(2,"game_card");
        panel(120,520,1680,285,false);
        const int state=launchState.load();
        const int cloudStatus=launchCloudStatus.load();
        const int queue=launchQueuePosition.load();
        const int elapsed=static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now()-launchStartedAt).count());
        if(state==1) {
            if(launchUiDrawStage<3) opennow::LogAppLifecycleEvent("UI_LAUNCH_DRAW_CONTENT_BEGIN","state=preparing");
            label(165,560,cloudStatus==1?"NVIDIA ESTA PREPARANDO TU EQUIPO":"SOLICITANDO UN EQUIPO A NVIDIA",3);
            char progress[180];
            if(cloudStatus==0) snprintf(progress,sizeof(progress),"VALIDANDO CUENTA Y CONSULTANDO REGIONES...");
            else if(cloudStatus==1) snprintf(progress,sizeof(progress),"NVIDIA PREPARA EL EQUIPO; CONSULTA %d",launchPollCount.load());
            else snprintf(progress,sizeof(progress),"EQUIPO ASIGNADO; PREPARANDO LA CONEXION...");
            label(165,625,progress,2);
            const int minutes=elapsed/60, seconds=elapsed%60;
            char queueText[150];
            if(queue>0) snprintf(queueText,sizeof(queueText),"COLA %d  |  CONSULTAS %d  |  TIEMPO %02d:%02d",queue,launchPollCount.load(),minutes,seconds);
            else snprintf(queueText,sizeof(queueText),"ASIGNANDO EQUIPO  |  CONSULTAS %d  |  TIEMPO %02d:%02d",launchPollCount.load(),minutes,seconds);
            label(165,675,queueText,2);
            if(launchUiDrawStage<3) opennow::LogAppLifecycleEvent("UI_LAUNCH_DRAW_BEAM_BEGIN");
            drawLoadingBeam(165,765,1590,8);
            if(launchUiDrawStage<3) opennow::LogAppLifecycleEvent("UI_LAUNCH_DRAW_BEAM_COMPLETE");
            logLaunchDrawStage(3,"preparation_card");
            label(165,784,"CIRCULO / B: CANCELAR Y VOLVER AL CATALOGO",1);
        } else if(state==2) {
            const auto remainingMs=std::chrono::duration_cast<std::chrono::milliseconds>(
                launchReadyAt-std::chrono::steady_clock::now()).count();
            const int seconds=static_cast<int>(std::max<int64_t>(0,(remainingMs+999)/1000));
            label(165,570,"TU EQUIPO ESTA LISTO",4);
            char countdown[96]; snprintf(countdown,sizeof(countdown),"CONECTANDO AUTOMATICAMENTE EN %d S",seconds);
            label(165,632,countdown,2);
            label(165,682,"PULSA X PARA CONECTAR AHORA",3);
            label(165,735,"CIRCULO / B: CANCELAR LA SESION Y VOLVER",2);
        } else if(state==3) {
            std::string error;
            SDL_LockMutex(launchMutex); error=launchError; SDL_UnlockMutex(launchMutex);
            std::string message=error;
            if(error.find("SESSION_REQUEST_IN_QUEUE_ABANDONED")!=std::string::npos || error.find("4A8C300F")!=std::string::npos)
                message="NVIDIA ABANDONO LA SOLICITUD EN COLA. PUEDE SER FALTA TEMPORAL DE EQUIPOS EN ESA REGION.";
            else if(error.find("SESSION_LIMIT_EXCEEDED_STATUS")!=std::string::npos || error.find("SESSION_LIMIT")!=std::string::npos)
                message="GEFORCE NOW TODAVIA DETECTA OTRA SESION ACTIVA. CIERRALA EN TUS OTROS DISPOSITIVOS Y REINTENTA.";
            else if(error.find("HTTP 503")!=std::string::npos)
                message="NVIDIA NO PUDO ASIGNAR UN EQUIPO (HTTP 503). ESPERA UN MOMENTO Y REINTENTA.";
            else if(message.empty()) message="NO SE RECIBIO DETALLE DEL SERVICIO.";
            label(165,560,"NO SE PUDO INICIAR EL JUEGO",3);
            char line[150]; snprintf(line,sizeof(line),"%.112s",message.c_str()); label(165,625,line,2);
            if(message.size()>112) { snprintf(line,sizeof(line),"%.112s",message.c_str()+112); label(165,665,line,2); }
            label(165,735,"X: REINTENTAR  |  CIRCULO / B: VOLVER AL CATALOGO",2);
        } else {
            label(165,570,"CARGA CANCELADA",3);
            label(165,650,"PULSA CIRCULO / B PARA VOLVER AL CATALOGO",2);
        }
        logLaunchDrawStage(3,"preparation_card");
    } else if(page==0) {
        label(120,235,"CENTRO DE JUEGO",3);

        // =========================================================================================
        // CENTRO DE JUEGO — REESCRITO CON UNA SOLA TABLA
        // =========================================================================================
        // QUE ESTABA MAL (y se veia en pantalla):
        //   Habia DOS tablas paralelas, `titles` y `details`, con un `nullptr` en los indices 2 y 3
        //   porque los titulos de esas posiciones se calculaban aparte. Ademas `titles` tenia SIETE
        //   entradas y la ultima era "ACERCA DE" otra vez, asi que:
        //      - La fila 3 (indice 4) se pintaba con `titles[4]` = "PROBAR MANDO" pero con
        //        `details[4]` = "GAMEPAD TESTER": titulo y descripcion de sitios distintos.
        //      - El indice 5 solo salia bien por casualidad, porque `titles[5]` y `details[5]`
        //        coincidian en la rama "sin stream".
        //   Una entrada `nullptr` en `label()` es, como poco, fragil: basta cambiar el orden para
        //   tener una linea en blanco o un fallo.
        //
        // AHORA: una tabla de cinco elementos con titulo, descripcion y accion ya resueltos. El
        // orden es SIEMPRE el mismo, cambien o no los textos, asi que no puede desalinearse.
        const bool streamActive=hasPublishedStream();
        const bool signedIn=(authState.load()==3);

        // =========================================================================================
        // CENTRO DE JUEGO — USA LA TABLA UNICA DE ARRIBA
        // =========================================================================================
        // Aqui NO hay tabla propia ni indices escritos a mano: se llama a `buildHomeCards()`, la misma
        // funcion que alimenta el dispatch de la X. Esa es la garantia de que el indice que se ILUMINA
        // y el que se EJECUTA son siempre el mismo.
        HomeCard cards[kHomeCardCount];
        buildHomeCards(cards, signedIn, streamActive);
        const int count=kHomeCardCount;
        static_assert(kHomeCardCount==6, "El centro de juego debe tener seis tarjetas: 2x3");

        // COMPROBACION EN TIEMPO REAL DE LA SELECCION.
        //
        // Si `selection` saliera alguna vez del rango [0..kHomeCardCount-1], NINGUNA tarjeta se
        // iluminaria (el bucle nunca encontraria `i==selection`) y el usuario veria el mando "muerto".
        // Es exactamente el sintoma de un desfase, asi que en vez de confiar en que no pase, se
        // comprueba y SE REPARA: se acota al rango valido y se deja constancia en el log.
        //
        // El coste es una comparacion por frame. El log solo se escribe cuando hay un problema, asi
        // que en funcionamiento normal no genera ni una linea.
        if(selection<0 || selection>=kHomeCardCount) {
            char detail[160];
            snprintf(detail,sizeof(detail),
                     "selection=%d fuera de rango [0..%d] - se corrige a 0",
                     selection,kHomeCardCount-1);
            opennow::LogAppLifecycleEvent("MENU_SELECTION_OUT_OF_RANGE",detail);
            selection=0;
            homeSelectionAnimReset();
        }

        // Maquetacion: 2 columnas x 820 px, separadas 40 px (980-120-820 = 40 exactos).
        // Filas de 140 px de alto con 35 px de separacion: 265, 440, 615.
        //   Seis tarjetas = TRES filas de dos. Fin de la tercera: 615+140 = 755
        //   < 955 (la ayuda de abajo)  -> sin solape.
        const int cardWidth=820, cardHeight=140;
        const int cardXs[2]={120,980};
        const int cardYs[kHomeRows]={265,440,615};

        // =========================================================================================
        // DIBUJADO DE LAS TARJETAS — MAPEO 1D -> REJILLA
        // =========================================================================================
        // EL MAPEO ES EL QUE SE PIDIO, Y ES EL UNICO QUE HAY:
        //
        //     fila    = i / 2      (indice 0,1 -> fila 0;  2,3 -> fila 1;  4,5 -> fila 2)
        //     columna = i % 2      (pares -> columna 0;  impares -> columna 1)
        //     x = cardXs[columna]  y = cardYs[fila]
        //
        // Y la navegacion usa EXACTAMENTE el inverso:
        //     ABAJO/ARRIBA -> nextRow*2 + columna   (suma +2 y CONSERVA la columna)
        //     DERECHA      -> fila*2 + 1            (suma +1, solo desde columna par)
        //     IZQUIERDA    -> fila*2 + 0            (resta 1, solo desde columna impar)
        //
        // Lo confirma el log de la consola: desde index=1 (fila 0, col 1) bajar lleva a index=3
        // (fila 1, col 1), y desde index=5 subir baja a index=3. La columna se conserva siempre.
        //
        // ---------------------------------------------------------------------------------------
        // QUE SE HA QUITADO, Y POR QUE (esto es lo que causaba el "desfase" que se veia)
        // ---------------------------------------------------------------------------------------
        // Habia un efecto que iluminaba TAMBIEN las tarjetas VECINAS, con una intensidad proporcional
        // a su distancia al foco:
        //
        //     closeness = max(0, 1 - |selection_suavizado - i|)
        //     if(closeness > 0.01) fillAlpha(..., 70*closeness);
        //
        // El resultado eran DOS o TRES tarjetas iluminadas a la vez, y la mas brillante no siempre
        // era la seleccionada mientras la animacion iba de camino. En pantalla eso se lee como que el
        // foco esta en la tarjeta equivocada.
        //
        // AHORA SE ILUMINA UNA SOLA TARJETA, sin ambiguedad: la condicion es exactamente `selection==i`
        // y no depende de ningun valor suavizado. Es imposible que se vean dos resaltes.
        //
        // Se conserva UN toque de suavizado, pero solo en el PULSO de la barra de acento de la tarjeta
        // elegida (que siempre es la correcta): quita la sensacion de rigidez sin poder despistar.
        {
            const Uint32 nowTick=SDL_GetTicks();
            // La variable suavizada se sigue actualizando para que el pulso no de un salto al cambiar
            // de tarjeta, pero NO decide que se ilumina.
            const float menuDelta=g_homeSelectionLastTick
                                     ?(nowTick-g_homeSelectionLastTick)/1000.0f:0.016f;
            g_homeSelectionLastTick=nowTick;
            if(g_homeSelectionSmooth<0.0f) g_homeSelectionSmooth=static_cast<float>(selection);
            g_homeSelectionSmooth+=(static_cast<float>(selection)-g_homeSelectionSmooth)
                                     *std::min(1.0f,menuDelta*14.0f);

            for(int i=0;i<count;i++) {
                // MAPEO 1D -> REJILLA. Unica formula, la misma que usa la navegacion al reves.
                const int fila=i/2, columna=i%2;
                const int x=cardXs[columna],y=cardYs[fila];

                // UNICA CONDICION DE ILUMINACION. Sin distancia, sin suavizado, sin vecinos.
                const bool isSelected=(selection==i);

                fill(x,y,cardWidth,cardHeight,color(23,31,45));
                if(isSelected) {
                    // La tarjeta elegida: fondo algo mas claro + halo interior.
                    fillAlpha(x,y,cardWidth,cardHeight,color(35,58,44),70);
                } else {
                    // Las demas: un pelo mas oscuras por abajo, para que no parezcan planas.
                    fillAlpha(x,y+cardHeight-6,cardWidth,6,color(14,19,28),120);
                }
                outline(x,y,cardWidth,cardHeight,isSelected?color(118,255,66):color(49,62,79));
                if(isSelected) {
                    // Barra de acento a la IZQUIERDA con un pulso lento (un seno sobre el reloj: un
                    // valor por frame, sin llamadas extra al renderizador).
                    fill(x,y,7,cardHeight,color(118,255,66));
                    const float pulse=0.5f+0.5f*std::sin(nowTick*0.004f);
                    fillAlpha(x+7,y,3,cardHeight,color(190,255,161),
                              static_cast<int>(120.0f*pulse));
                }
                label(x+38,y+37,cards[i].title,3);
                label(x+38,y+91,cards[i].detail,2);
            }
        }

        label(120,955,"CRUCETA: ELEGIR     X: ABRIR",2);
        // No separate version label here: the build identity already lives in the
        // bottom status bar ("AJ | PS4 | ... | UI n FPS | VERSION x.yz"), which is
        // the existing place for it. A second copy just duplicated the information.
    } else if(page==7) {
        label(120,235,"PROBADOR DE MANDO",4);
        panel(120,300,1680,590,false);
        char line[180];
        snprintf(line,sizeof(line),"MANDOS: %d  |  ENTRADA: %s",std::max(controllerCount,ps4PadReady?1:0),ps4PadReady?"SCEPAD NATIVO":(isSdlPs4Joystick(genericJoysticks[0])?"SDL / SCEPAD DIRECTO":"SDL")); label(165,330,line,2);
        SDL_GameController* pad=gameControllers[0];
        SDL_Joystick* joy=genericJoysticks[0];
        sampleInputCalibration();
        OrbisPadData testerFresh{};
        const bool testerFreshReady=!ps4PadReady && readFreshSdlPs4State(joy,testerFresh);
        const OrbisPadData& testerPad=testerFreshReady?testerFresh:ps4PadState;
        if(ps4PadReady || testerFreshReady) {
            struct ButtonView { const char* name; uint32_t mask; };
            static const ButtonView buttons[]={{"L3",ORBIS_PAD_BUTTON_L3},{"R3",ORBIS_PAD_BUTTON_R3},{"OPTIONS",ORBIS_PAD_BUTTON_OPTIONS},{"UP",ORBIS_PAD_BUTTON_UP},{"RIGHT",ORBIS_PAD_BUTTON_RIGHT},{"DOWN",ORBIS_PAD_BUTTON_DOWN},{"LEFT",ORBIS_PAD_BUTTON_LEFT},{"L2",ORBIS_PAD_BUTTON_L2},{"R2",ORBIS_PAD_BUTTON_R2},{"L1",ORBIS_PAD_BUTTON_L1},{"R1",ORBIS_PAD_BUTTON_R1},{"TRIANGLE",ORBIS_PAD_BUTTON_TRIANGLE},{"CIRCLE",ORBIS_PAD_BUTTON_CIRCLE},{"CROSS",ORBIS_PAD_BUTTON_CROSS},{"SQUARE",ORBIS_PAD_BUTTON_SQUARE},{"TOUCHPAD",ORBIS_PAD_BUTTON_TOUCH_PAD}};
            for(int i=0;i<16;i++) { const int col=i%8,row=i/8,x=165+col*200,y=375+row*68; const bool active=(testerPad.buttons&buttons[i].mask)!=0; panel(x,y,180,54,active); label(x+12,y+17,buttons[i].name,2); label(x+112,y+17,active?"SI":"--",2); }
            const float axes[]={calibratedStickValue(testerPad.leftStick.x,0,true),calibratedStickValue(testerPad.leftStick.y,1,true),calibratedStickValue(testerPad.rightStick.x,2,true),calibratedStickValue(testerPad.rightStick.y,3,true)};
            const char* names[]={"STICK IZQ X","STICK IZQ Y","STICK DER X","STICK DER Y"};
            for(int i=0;i<4;i++) { const int y=535+i*57; label(165,y,names[i],2); fill(420,y+3,960,18,color(38,49,66)); fill(900,y,4,24,color(109,123,142)); const int marker=std::clamp(900+static_cast<int>(axes[i]*460),420,1368); fill(marker,y,12,24,color(118,255,66)); snprintf(line,sizeof(line),"%+.2f",axes[i]); label(1420,y,line,2); }
            for(int i=0;i<2;i++) { const int raw=i?testerPad.analogButtons.r2:testerPad.analogButtons.l2; const int value=calibratedTriggerValue(raw,i,true); const int y=770+i*44; label(165,y,i?"GATILLO R2":"GATILLO L2",2); fill(420,y+3,960,18,color(38,49,66)); fill(420,y+3,std::max(0,std::min(960,value*960/255)),18,color(118,255,66)); snprintf(line,sizeof(line),"%d%%",value*100/255); label(1420,y,line,2); }
        } else if(pad) {
            const char* names[]={"A","B","X","Y","L1","R1","L3","R3","UP","DOWN","LEFT","RIGHT","OPTIONS","SHARE"};
            const SDL_GameControllerButton buttons[]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_LEFTSTICK,SDL_CONTROLLER_BUTTON_RIGHTSTICK,SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_BACK};
            for(int i=0;i<14;i++) { const int col=i%7,row=i/7; const int x=165+col*220,y=410+row*74; const bool down=SDL_GameControllerGetButton(pad,buttons[i])!=0; panel(x,y,200,58,down); label(x+12,y+18,names[i],2); label(x+112,y+18,down?"SI":"--",2); }
            snprintf(line,sizeof(line),"EJES: L %d/%d  R %d/%d  |  GATILLOS: L2 %d R2 %d",SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX),SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY),SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTX),SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTY),SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT),SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)); label(165,615,line,2);
        } else if(joy) {
            snprintf(line,sizeof(line),"JOYSTICK GENERICO: %d EJES / %d BOTONES / %d HAT",SDL_JoystickNumAxes(joy),SDL_JoystickNumButtons(joy),SDL_JoystickNumHats(joy)); label(165,370,line,2);
            static const char* buttonNames[20]={"X","CIRCULO","CUADRADO","TRIANGULO","L1","R1","B6","B7","B8","START","B10","L3","R3","ARRIBA","ABAJO","IZQUIERDA","DERECHA","TOUCHPAD","L2","R2"};
            for(int i=0;i<std::min(20,SDL_JoystickNumButtons(joy));i++) { const int x=165+(i%8)*200,y=410+(i/8)*58; const bool down=SDL_JoystickGetButton(joy,i)!=0; panel(x,y,180,48,down); snprintf(line,sizeof(line),"B%d %s: %s",i,buttonNames[i],down?"SI":"--"); label(x+12,y+15,line,2); }
            const int axes=SDL_JoystickNumAxes(joy);
            const int rightX=genericStickAxis(2),rightY=genericStickAxis(3),leftT=genericTriggerAxis(0),rightT=genericTriggerAxis(1);
            const float lx=calibratedStickValue(SDL_JoystickGetAxis(joy,0),0,false),ly=axes>1?calibratedStickValue(SDL_JoystickGetAxis(joy,1),1,false):0.0f;
            const float rx=axes>rightX?calibratedStickValue(SDL_JoystickGetAxis(joy,rightX),2,false):0.0f;
            const float ry=axes>rightY?calibratedStickValue(SDL_JoystickGetAxis(joy,rightY),3,false):0.0f;
            snprintf(line,sizeof(line),"STICKS L %+.2f/%+.2f  R %+.2f/%+.2f",lx,ly,rx,ry); label(165,610,line,2);
            const int lt=axes>leftT?SDL_JoystickGetAxis(joy,leftT):-32768,rt=axes>rightT?SDL_JoystickGetAxis(joy,rightT):-32768;
            const bool l2Button=SDL_JoystickNumButtons(joy)>18 && SDL_JoystickGetButton(joy,18);
            const bool r2Button=SDL_JoystickNumButtons(joy)>19 && SDL_JoystickGetButton(joy,19);
            const int l2pct=(l2Button && std::abs(lt-genericTriggerRestValue(0))<128)?100:genericTriggerValue(lt,0)*100/255;
            const int r2pct=(r2Button && std::abs(rt-genericTriggerRestValue(1))<128)?100:genericTriggerValue(rt,1)*100/255;
            snprintf(line,sizeof(line),"L2 %d%%  |  R2 %d%% %s  |  MAPA %s",l2pct,r2pct,r2Button?"(B19)":"",genericSplitTriggerAxes[0]?"GATILLOS 2/3; STICK DER 4/5":"GATILLOS 4/5; STICK DER 2/3"); label(165,660,line,2);
            int raw[6]{}; for(int i=0;i<std::min(6,axes);i++) raw[i]=SDL_JoystickGetAxis(joy,i);
            traceGamepadAnalogSample(raw,l2Button,r2Button);
            snprintf(line,sizeof(line),"EJES CRUDOS 0-5: %d / %d / %d / %d / %d / %d",raw[0],raw[1],raw[2],raw[3],raw[4],raw[5]); label(165,690,line,2);
            if(SDL_JoystickNumHats(joy)>0) { snprintf(line,sizeof(line),"CRUCETA HAT: 0x%02X",SDL_JoystickGetHat(joy,0)); label(165,720,line,2); }
        } else label(165,420,"CONECTA UN MANDO USB O DUALSHOCK 4",3);
        label(165,844,inputCalibrationStatus,2);
        label(120,925,"PRUEBA BOTONES, CRUCETA, EJES Y GATILLOS. MANTEN CIRCULO 1 S PARA SALIR.",2);
    } else if(page==8) {
        // =========================================================================================
        // ACERCA DE — MAQUETACION EN DOS COLUMNAS
        // =========================================================================================
        // Panel: x=120 .. 1800 (1680 de ancho), y=300 .. 985 (685 de alto).
        //   Columna IZQUIERDA (texto): x=175, ancho util 900 px. A escala 2 son ~82 caracteres por
        //     linea, asi que el texto va partido a ~78 para que no se salga de la columna.
        //   Columna DERECHA (QR): los tres QR van APILADOS en vertical, que es la unica forma de que
        //     quepan con tamano legible. Numeros exactos, sin aproximar:
        //         x del QR          = 1520 - 156/2 = 1442      (centrado en la columna)
        //         alto de cada celda= 156 (QR) + 30 (titulo debajo) = 186
        //         separacion        = 40
        //         y de cada celda   = 330, 330+186+40=556, 556+186+40=782
        //         fin del ultimo    = 782 + 186 = 968  <  985 (fondo del panel)  -> NO se sale
        //   El texto termina como mucho en y=830, y empieza en x=175; el QR empieza en x=1442.
        //   Hay 1442-175 = 1267 px de separacion horizontal: no hay solapamiento posible.
        label(120,235,"ACERCA DE AJ / GEFORCE NOW PS4",4);
        panel(120,300,1680,712,false);

        // --- Columna izquierda: que es esta aplicacion ---
        int ty=352;
        const int lineStep=46;
        label(175,ty,"CLIENTE NATIVO EXPERIMENTAL PARA PLAYSTATION 4.",3); ty+=62;
        label(175,ty,"INTEGRACION CON WEBRTC PARA TRANSMISION EN LA NUBE, CON",2); ty+=lineStep;
        label(175,ty,"DECODIFICACION POR HARDWARE Y PRESENTACION DIRECTA EN",2); ty+=lineStep;
        label(175,ty,"PANTALLA (SCEVIDEOOUT 1080P60). AUDIO OPUS NATIVO POR",2); ty+=lineStep;
        label(175,ty,"SCEAUDIOOUT Y ENTRADA DE ULTRA BAJA LATENCIA (SCEPAD).",2); ty+=lineStep+22;
        label(175,ty,"ESTA ES UNA VERSION DE PRUEBA: NO ES UNA VERSION FINAL.",3); ty+=62;
        label(175,ty,"PUEDE FALLAR, CAMBIAR SIN AVISO Y NO ESTA AFILIADA NI",2); ty+=lineStep;
        label(175,ty,"RESPALDADA POR NVIDIA CORPORATION. TOMA OPENNOW-SWITCH",2); ty+=lineStep;
        label(175,ty,"(LICENCIA MIT) COMO REFERENCIA DEL PROTOCOLO.",2);

        // AYUDA DE NAVEGACION EN LA ESQUINA SUPERIOR DERECHA.
        //
        // No cabe abajo: el pie de pagina ("AJ | PS4 | ... | VERSION") ya ocupa y=1035, y el ultimo QR
        // termina en y=996. Ponerla en 1035 la solaparia con el pie.
        //
        // Aqui no molesta a nadie: el titulo "ACERCA DE AJ / GEFORCE NOW PS4" a escala 4 ocupa hasta
        // x~715, y la columna de los QR empieza en x=1434. La ayuda (x=1120, ~387 px de ancho) cabe
        // entera en ese hueco y queda a la vista sin competir con el texto informativo.
        label(1120,220,"CIRCULO: VOLVER AL CENTRO DE JUEGO",2);

        // --- Columna derecha: los tres QR ---
        // Se dibujan SIEMPRE los tres huecos, aunque una imagen no haya cargado: asi la maquetacion
        // no se mueve y el usuario ve que hay tres enlaces, no dos.
        //
        // EL TITULO VA ENCIMA DE CADA QR, como se pidio. Eso cambia la aritmetica de la columna:
        //   Cada celda = banda de titulo (30) + QR (156) = 186 px de alto.
        //   Separacion entre celdas = 54 px, elegida para que la banda de titulo de una celda no
        //   quede pegada al QR de la anterior. Separacion total = 186 + 54 = 240.
        //     celda 0: banda y=330, QR y=360
        //     celda 1: banda y=570, QR y=600
        //     celda 2: banda y=810, QR y=840
        //   Comprobaciones:
        //     fin del ultimo QR  = 840 + 156 = 996 ; con la tarjeta (8 px de borde) = 1004
        //     tarjeta blanca    = 360 - 8   = 352 ; la banda acaba en 330 + 16 = 346
        //                                          -> 6 px de margen, no pisa la tarjeta
        //     el panel mide 712 de alto (300..1012) para CONTENER la ultima tarjeta, que acaba en 1004
        //     el pie de pagina esta en y=1035 -> 23 px de separacion, sin solape
        const int qrSize=156;
        const int qrX=1442;
        const int captionBand=30;   // alto reservado al titulo, POR ENCIMA del QR
        for(int i=0;i<3;i++) {
            const int cellY=330+i*240;
            const int qrY=cellY+captionBand;
            qrCard(i,qrX,qrY,qrSize);

            const char* caption=translateUi(qrImages[i].caption);
            // El titulo va CENTRADO sobre su QR. Se mide el ancho real del texto para centrarlo de
            // verdad en vez de estimarlo: con una estimacion los titulos largos quedaban desviados.
            int captionWidth=0,captionHeight=0;
            if(SDL_Texture* captionTexture=makeTextTexture(caption,16)) {
                SDL_QueryTexture(captionTexture,nullptr,nullptr,&captionWidth,&captionHeight);
            }
            const int captionX=qrX+(qrSize-captionWidth)/2;
            text(captionX,cellY,caption,2,color(150,220,150));
        }

    } else if(page==4) {
        label(120,235,"INICIAR SESION EN GEFORCE NOW",4);
        panel(120,300,1680,570,false);
        int state=authState.load();
        if(state==1) label(180,345,"CONECTANDO CON NVIDIA...",3);
        else if(state==2) {
            opennow::QrLoginChallenge challenge;
            SDL_LockMutex(authMutex); challenge=authChallenge; SDL_UnlockMutex(authMutex);
            label(180,345,"ESCANEA EL CODIGO QR Y AUTORIZA EL INICIO",2);
            if(!challenge.verification_uri_complete.empty()) {
                uint8_t temp[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
                if(qrcodegen_encodeText(challenge.verification_uri_complete.c_str(),temp,qr,qrcodegen_Ecc_MEDIUM,1,qrcodegen_VERSION_MAX,qrcodegen_Mask_AUTO,true)) {
                    int size=qrcodegen_getSize(qr), scale=std::min(9,400/(size+8));
                    int originX=1290, originY=390, quiet=4*scale;
                    fill(originX-quiet,originY-quiet,(size+8)*scale,(size+8)*scale,color(255,255,255));
                    for(int qy=0;qy<size;qy++) for(int qx=0;qx<size;qx++)
                        if(qrcodegen_getModule(qr,qx,qy)) fill(originX+qx*scale,originY+qy*scale,scale,scale,color(10,14,21));
                }
            }
            char code[96]; snprintf(code,sizeof(code),"CODIGO: %s",challenge.user_code.c_str());
            label(180,425,code,4);
            label(180,495,"ABRE NVIDIA.COM/LOGIN EN TU TELEFONO",2);
            label(180,540,rememberLogin?"SESION GUARDADA CIFRADA EN ESTE PS4":"LA CUENTA SE MANTIENE SOLO EN MEMORIA",2);
        } else if(state==3) {
            label(180,360,"SESION AUTORIZADA EN ESTA EJECUCION",3);
            label(180,435,"SIGUIENTE: CATALOGO Y SESION DE JUEGO",2);
        } else if(state==4) {
            char error[180];
            SDL_LockMutex(authMutex); snprintf(error,sizeof(error),"ERROR: %.150s",authError.c_str()); SDL_UnlockMutex(authMutex);
            label(180,355,error,2);
        } else label(180,355,"PULSA X PARA SOLICITAR UN NUEVO CODIGO",2);
        panel(120,790,1680,62,selection==2); label(155,810,rememberLogin?"[X] MANTENER SESION INICIADA":"[ ] MANTENER SESION INICIADA",2); label(850,810,rememberLogin?"ACTIVADO":"DESACTIVADO",2);
        panel(120,895,820,75,selection==0); label(165,920,(state==1||state==2)?(language==0?"CANCELAR":"CANCEL"):(state==3?(language==0?"CERRAR SESION":"SIGN OUT"):(language==0?"INTENTAR DE NUEVO":"TRY AGAIN")),2);
        panel(980,895,820,75,selection==1); label(1025,920,"VOLVER AL INICIO",2);
        label(120,985,"CIRCULO: VOLVER",2);
    } else if(page==1) {
        // ---------------------------------------------------------------------------
        // PANTALLA DE CONFIGURACION
        //
        // Reorganizada en la 2.90d. Los problemas de la version anterior eran de
        // posicionamiento, no de datos:
        //   - El titulo estaba en y=235 y la primera fila empezaba en y=248: el titulo
        //     quedaba tapado por el panel.
        //   - Las etiquetas se colocaban a mano una por una, asi que varias quedaron
        //     desplazadas respecto a su panel (el valor del dispositivo de entrada aparecia
        //     900,329 mientras su panel estaba en otra fila).
        //
        // Ahora todo se calcula a partir de una sola constante: la fila i empieza en
        // kRowTop + i*kRowStep. Cambiar el numero de filas o el espaciado es tocar una linea.
        // ---------------------------------------------------------------------------
        constexpr int kSettingsRows = 15;
        constexpr int kRowTop  = 268;   // primera fila (deja libre el titulo de y=210)
        constexpr int kRowStep = 42;    // separacion vertical entre filas
        constexpr int kRowH    = 38;    // alto del panel
        constexpr int kLabelY  = 11;    // desplazamiento del texto dentro del panel
        constexpr int kLabelX  = 160;   // margen izquierdo del texto
        constexpr int kValueX  = 1120;  // donde empiezan los valores (alineados entre si)
        constexpr int kPanelW  = 1680;

        label(120,210,"CONFIGURACION AJ",4);

        for(int i=0;i<kSettingsRows;i++) {
            const int rowY=kRowTop+i*kRowStep;
            panel(120,rowY,kPanelW,kRowH,selection==i);
            // PILDORA DEL VALOR: un fondo suave detras de la columna de valores (x=1120 hasta el
            // borde derecho del panel). Sirve para que el ojo separe de un vistazo la ETIQUETA (que
            // es lo que se lee) del VALOR (que es lo que se cambia), y para que la columna de valores
            // se vea como una columna y no como texto suelto.
            //
            // Coste: UNA llamada a SDL_RenderFillRect por fila (15 en total en esta pantalla). Es el
            // mismo tipo de primitiva que ya usa `panel`, asi que el tiempo de dibujado no cambia de
            // forma apreciable.
            fillAlpha(kValueX-14,rowY+5,(120+kPanelW-8)-(kValueX-14),kRowH-10,color(12,17,26),110);
        }
        // Devuelve la Y del texto para la fila i. Centralizarlo evita que una etiqueta
        // quede fuera de su panel al anadir o mover filas.
        auto rowTextY=[&](int i){ return kRowTop+i*kRowStep+kLabelY; };

        // --- Fila 0: idioma de la interfaz ---
        label(kLabelX,rowTextY(0),language==0?"IDIOMA / LANGUAGE":"LANGUAGE",2);
        label(kValueX,rowTextY(0),language==0?"ESPANOL":"ENGLISH",2);

        // --- Fila 1: dispositivo de entrada ---
        label(kLabelX,rowTextY(1),language==0?"DISPOSITIVO DE ENTRADA":"INPUT DEVICE",2);
        {
            const char* deviceName = inputDevice==0 ? (language==0?"SOLO MANDO":"CONTROLLER ONLY")
                                   : inputDevice==1 ? (language==0?"TECLADO + RATON":"KEYBOARD + MOUSE")
                                                    : (language==0?"MANDO + TECLADO/RATON":"CONTROLLER + KEYBOARD/MOUSE");
            label(kValueX,rowTextY(1),deviceName,2);
        }

        // --- Fila 2: resolucion del stream ---
        label(kLabelX,rowTextY(2),language==0?"RESOLUCION DEL STREAM":"STREAM RESOLUTION",2);
        {
            // LA ETIQUETA TIENE QUE COINCIDIR CON LO QUE SE PIDE (v3.95).
            //
            // Antes, al elegir el modo 2 decia `"1080P (1920x1080)"` — pero `resolution_to_wh()` fuerza
            // **720p** en los tres modos, por el cierre medido con framebuffer de 1080p (1 flip frente a
            // 119 en 720p). La interfaz prometia una resolucion que nunca se pedia.
            //
            // Ahora el modo 2 se muestra como **540P (RECOMENDADO)**, que es lo que de verdad conviene:
            // el servidor entrega 960x540 en todas las sesiones, asi que aceptarlo es mas honesto que
            // llamarlo 1080p. **La peticion al servidor NO cambia** (sigue siendo 720p, que es lo
            // estable): lo unico que cambia es que la pantalla deja de mentir.
            const char* resName = resolution==0 ? (language==0?"AUTOMATICA":"AUTOMATIC")
                                : resolution==1 ? "720P (1280x720)"
                                                : (language==0?"720P (1080P NO DISPONIBLE)":"720P (1080P UNAVAILABLE)");
            label(kValueX,rowTextY(2),resName,2);
        }

        // --- Fila 3: limite de bitrate ---
        // Antes no existia en la interfaz: el stream siempre pedia "auto" y el servidor
        // elegia 12 Mbps para 720p60, que es la causa de la imagen borrosa.
        // --- Fila 3: FPS de transmision ---
        //
        // 60 FPS: presupuesto de 16,6 ms por cuadro. Maxima fluidez si la CPU da abasto.
        // 30 FPS: presupuesto de 33,3 ms. El escalado 540p->720p (~13,8 ms) deja de ser critico y
        //         la sesion se mantiene estable aunque el servidor entregue 540p.
        label(kLabelX,rowTextY(3),language==0?"FPS DE TRANSMISION":"STREAM FPS",2);
        {
            static const char* const kF[]={"60 FPS","30 FPS"};
            label(kValueX,rowTextY(3),(fps==30)?kF[1]:kF[0],2);
        }
        label(kLabelX,rowTextY(4),language==0?"LIMITE DE BITRATE":"BITRATE LIMIT",2);
        {
            char brLabel[72];
            if(networkMode==0) std::snprintf(brLabel,sizeof(brLabel),"%d MBPS  (MODO RED LENTA)",bitrate);
            else               std::snprintf(brLabel,sizeof(brLabel),"%d MBPS",bitrate);
            label(kValueX,rowTextY(4),brLabel,2);
        }

        // --- Fila 4: modo de calidad ---
        label(kLabelX,rowTextY(5),language==0?"MODO DE CALIDAD":"QUALITY MODE",2);
        {
            static const char* const kQ[]={"ORIGINAL (MAX. NITIDEZ)","CLARITY (SUAVIZADO)","ADAPTIVE (EQUILIBRIO)"};
            label(kValueX,rowTextY(5),kQ[(imageQualityMode>=0&&imageQualityMode<3)?imageQualityMode:0],2);
        }

        // --- Fila 5: buffer de audio ---
        label(kLabelX,rowTextY(6),language==0?"BUFFER DE AUDIO":"AUDIO BUFFER",2);
        {
            char ab[48]; std::snprintf(ab,sizeof(ab),"%d MS",audioBufferMs);
            label(kValueX,rowTextY(6),ab,2);
        }

        // --- Fila 6: idioma del juego ---
        label(kLabelX,rowTextY(7),language==0?"IDIOMA DEL JUEGO":"GAME LANGUAGE",2);
        {
            static const char* const kL[]={"ENGLISH","ESPANOL (ES)","ESPANOL (MX)","FRANCAIS","DEUTSCH","ITALIANO","PORTUGUES","JAPONES"};
            label(kValueX,rowTextY(7),kL[(gameLanguage>=0&&gameLanguage<8)?gameLanguage:0],2);
        }

        // --- Fila 7: modo de decodificador ---
        //
        // AUTO intenta el decodificador por hardware de PS4 (libSceVideodec2) y cae a software si
        // falla, registrando el motivo exacto. HARDWARE lo fuerza para poder probarlo aislado.
        // SOFTWARE fuerza FFmpeg, que es el camino estable actual.
        //
        // POR QUE ES NECESARIO: el decodificador por hardware llevaria el coste de decodificacion a
        // casi cero, dejando la CPU para el escalado. Medido: a 720p el escalado consume 14.419 us de
        // los 16.666 de presupuesto, que es lo que limita los FPS a 39-48.
        label(kLabelX,rowTextY(8),language==0?"DECODIFICADOR":"VIDEO DECODER",2);
        {
            // PUNTO D DEL PLAN v3.30: el decodificador por hardware NO es funcional en esta
            // consola y el ajuste se retira de la interaccion. La evidencia medida es tajante:
            //   - libSceVideodec2 no carga: BOOT_CODEC_MODULE module=VIDEODEC2 rc=0x805A1000
            //   - hardware_decode=no en TODOS los logs de todas las versiones
            //   - la arbitracion no tiene firmas verificadas y cerro la aplicacion en la 3.11
            // Se muestra el estado REAL (de solo lectura) en vez de ofrecer una eleccion que no
            // cambia nada. El probe v1 se conserva como diagnostico interno, fuera de la UI.
            label(kValueX,rowTextY(8),language==0?"NO DISPONIBLE (SW)":"NOT AVAILABLE (SW)",2);
        }
        // --- Fila 8: realce de nitidez del escalador ---
        // Compensa el suavizado que introduce el escalado cuando el servidor entrega 540p en vez de
        // 720p. Es ajustable porque el nivel ideal depende del juego: demasiado genera halos en los
        // bordes, y muy poco deja la imagen blanda.
        // --- Fila 8: modo de escalado de video ---
        //
        // NATIVO: se pide 720p y, si el servidor baja a 540p, se escala en CPU (17-26 ms).
        // 540P FIJO: se pide 540p directamente, NO se escala en CPU (0 ms), y `sceVideoOut` lleva
        //            el framebuffer a la pantalla por hardware. Se pierde algo de nitidez teorica
        //            pero se ganan 60 FPS estables.
        label(kLabelX,rowTextY(9),language==0?"RESOLUCION DE VIDEO":"VIDEO RESOLUTION",2);
        {
            // PUNTO B DEL PLAN v3.30: esta fila era REDUNDANTE. Su valor se aplicaba dentro de
            // `resolution_to_wh()`, la MISMA funcion que usa la fila 2 (RESOLUCION DEL STREAM), asi
            // que las dos escribian la misma variable y se apilaban. Eso es lo que llevaba a poner
            // "720 y 720" y acabar con 1080p en el framebuffer, que fue el unico caso que cerro la
            // aplicacion. Ahora la resolucion la controla SOLO la fila 2 y aqui se muestra el
            // resultado real que se aplica, en modo informativo (sin ajuste).
            label(kValueX,rowTextY(9),language==0?"720P (FIJO, 1080P DESCARTADO)"
                                                 :"720P (FIXED, 1080P DISABLED)",2);
        }
        label(kLabelX,rowTextY(10),language==0?"REALCE DE NITIDEZ":"SHARPNESS",2);
        {
            static const char* const kS[]={"DESACTIVADO","SUAVE","MEDIO","ALTO"};
            label(kValueX,rowTextY(10),kS[(sharpnessLevel>=0&&sharpnessLevel<4)?sharpnessLevel:2],2);
        }
        // --- Fila 9: region de GeForce NOW ---
        label(kLabelX,rowTextY(11),language==0?"REGION DE GEFORCE NOW":"GEFORCE NOW REGION",2);
        {
            std::string regionLabel=settingsRegionLabel();
            if(settingsRegionState.load()==1) regionLabel=language==0?"CARGANDO LISTA...":"LOADING REGIONS...";
            else if(settingsRegionState.load()==3 && streamRegion=="Auto") regionLabel=language==0?"AUTO | X: REINTENTAR":"AUTO | X: RETRY";
            else if(settingsRegionState.load()!=2 && streamRegion=="Auto") regionLabel=language==0?"AUTO | X: CARGAR LISTA":"AUTO | X: LOAD LIST";
            if(regionLabel.size()>34) regionLabel.resize(34);
            // El valor va en la MISMA fila que su etiqueta. Estaba en rowTextY(7), que es la fila
            // del realce de nitidez, asi que los dos textos se dibujaban uno encima del otro: es
            // exactamente el solape que se veia en consola.
            label(kValueX,rowTextY(11),regionLabel.c_str(),2);
        }

        // --- Fila 8: medir velocidad ---
        label(kLabelX,rowTextY(12),language==0?"MEDIR VELOCIDAD DE INTERNET":"RUN NETWORK SPEED TEST",2);
        {
            const int state=probeState.load();
            if(state==1 && testKind==1) {
                label(kValueX,rowTextY(12),language==0?"MIDIENDO...":"MEASURING...",2);
            } else if(state==2 && testKind==1) {
                char speed[48];
                std::snprintf(speed,sizeof(speed),"%d.%d MBPS",probeSpeedMbps10.load()/10,probeSpeedMbps10.load()%10);
                label(kValueX,rowTextY(12),speed,2);
            } else if(state==3 && testKind==1) {
                char failed[48];
                if(probeHttpStatus.load()>0) std::snprintf(failed,sizeof(failed),"ERROR HTTP %d",probeHttpStatus.load());
                else std::snprintf(failed,sizeof(failed),"FALLO LA PETICION");
                label(kValueX,rowTextY(12),failed,2);
            } else {
                label(kValueX,rowTextY(12),language==0?"PULSA X":"PRESS X",2);
            }
        }

        // --- Fila 9: volver a leer el aviso beta ---
        label(kLabelX,rowTextY(13),language==0?"IMPORTANTE LEER":"IMPORTANT TO READ",2);
        label(kValueX,rowTextY(13),language==0?"VER AVISO DE VERSION BETA":"VIEW BETA NOTICE",2);

        // --- Fila 10: guardar y volver ---
        label(kLabelX,rowTextY(14),language==0?"GUARDAR Y VOLVER":"SAVE AND RETURN",2);
        if(saveResult<0)      label(kValueX,rowTextY(14),language==0?"ERROR AL GUARDAR":"SAVE FAILED",2);
        else if(saveResult>0) label(kValueX,rowTextY(14),language==0?"GUARDADO":"SAVED",2);
        else                  label(kValueX,rowTextY(14),language==0?"PULSA X":"PRESS X",2);

        // Nota informativa, debajo de todas las filas y sin solaparse con ellas.
        {
            const int noteY = kRowTop + kSettingsRows*kRowStep + 16;
            if(probeState.load()==2 && testKind==1) {
                label(120,noteY,   language==0?"TRANSMISION A 60 FPS OPTIMIZADA PARA PS4.":"60 FPS STREAM OPTIMIZED FOR PS4.",2);
                label(120,noteY+34,language==0?"MEDICION CLOUDFLARE; RUTA A GFN PUEDE VARIAR.":"CLOUDFLARE SPEED TEST; GFN ROUTING MAY VARY.",2);
            } else {
                label(120,noteY,language==0?"TRANSMISION A 60 FPS CON SALIDA DIRECTA POR HARDWARE (VIDEOOUT).":"60 FPS STREAM WITH DIRECT HARDWARE OUTPUT (VIDEOOUT).",2);
            }
            label(120,975,language==0?"CRUCETA: ELEGIR  |  IZQ/DER: CAMBIAR  |  REGION: X ABRIR SELECTOR":"D-PAD: SELECT  |  LEFT/RIGHT: CHANGE  |  REGION: X OPEN PICKER",2);
        }
        if(settingsRegionPickerVisible) {
            if(!settingsRegionPickerDrawLogged) {
                opennow::LogAppLifecycleEvent("REGION_PICKER_DRAW_BEGIN");
                settingsRegionPickerDrawLogged=true;
            }
            SDL_SetRenderDrawColor(renderer,3,7,14,232);
            SDL_Rect shade={0,0,W,H}; SDL_RenderFillRect(renderer,&shade);
            panel(350,190,1220,700,false);
            label(405,230,language==0?"ELEGIR REGION":"SELECT REGION",4);
            label(405,278,language==0?"RTT DE CONEXION TCP APROXIMADO; NO ES PING ICMP":"APPROX. TCP CONNECT RTT; THIS IS NOT ICMP PING",2);
            const int state=settingsRegionState.load();
            if(state==1) label(405,335,language==0?"CONSULTANDO REGIONES Y MIDIENDO RTT...":"FETCHING REGIONS AND MEASURING RTT...",2);
            else if(state==3) label(405,335,language==0?"NO SE PUDO MEDIR. X PARA REINTENTAR":"UNABLE TO MEASURE. PRESS X TO RETRY",2);
            std::vector<opennow::StreamRegion> regionSnapshot;
            { std::lock_guard<std::mutex> lock(settingsRegionsMutex); regionSnapshot=settingsRegions; }
            const int count=1+static_cast<int>(regionSnapshot.size());
            const int start=std::max(0,std::min(settingsRegionSelection-4,count-8));
            for(int line=0;line<8 && start+line<count;++line) {
                const int idx=start+line, y=370+line*52;
                panel(395,y,1130,44,idx==settingsRegionSelection);
                if(idx==0) label(420,y+12,language==0?"AUTOMATICA | NVIDIA ELIGE":"AUTOMATIC | NVIDIA SELECTS",2);
                else {
                    const auto& item=regionSnapshot[static_cast<size_t>(idx-1)];
                    label(420,y+12,item.name.c_str(),2);
                    char latency[48]; snprintf(latency,sizeof(latency),"%s",item.ping_ms>=0?(std::to_string(item.ping_ms)+" ms").c_str():"-- ms");
                    label(1350,y+12,latency,2);
                }
            }
            if(state==1) {
                constexpr int trackX=405,trackY=812,trackWidth=1110,pulseWidth=150;
                fill(trackX,trackY,trackWidth,4,color(49,62,79));
                const int travel=trackWidth-pulseWidth;
                const int phase=static_cast<int>((SDL_GetTicks()/5)%(2*travel));
                const int pulse=phase<=travel?phase:2*travel-phase;
                fill(trackX+pulse,trackY-2,pulseWidth,8,color(118,255,66));
            }
            label(405,835,language==0?"ARRIBA/ABAJO: ELEGIR  |  X: APLICAR  |  CIRCULO: CANCELAR":"UP/DOWN: SELECT  |  X: APPLY  |  CIRCLE: CANCEL",2);
            if(settingsRegionPickerDrawLogged) {
                opennow::LogAppLifecycleEvent("REGION_PICKER_DRAW_COMPLETE");
                settingsRegionPickerDrawLogged=false;
            }
        } else if(settingsRegionPickerDrawLogged) {
            opennow::LogAppLifecycleEvent("REGION_PICKER_CLOSED_BEFORE_DRAW_COMPLETE");
            settingsRegionPickerDrawLogged=false;
        }
    } else if(page==2) {
        label(120,235,"ESTADO DEL SERVICIO",4);
        panel(120,320,1680,300,false);
        int state=probeState.load();
        fill(165,375,20,20,state==2?color(118,255,66):(state==3?color(255,90,90):color(255,190,60)));
        if(state==1) label(210,370,"COMPROBANDO CONEXION HTTPS CON GFN...",2);
        else if(state==2) {
            char line[64]; snprintf(line,sizeof(line),"EL SERVICIO RESPONDIO - ESTADO HTTP %d",probeHttpStatus.load());
            label(210,370,line,2);
        } else if(state==3) {
            char line[64]; snprintf(line,sizeof(line),"FALLO LA PETICION HTTPS - ERROR %d",probeError.load());
            label(210,370,line,2);
        } else label(210,370,"PULSA X PARA PROBAR LA CONEXION.",2);
        label(165,445,testKind==1?"HTTPS GET: SPEED.CLOUDFLARE.COM":"HTTPS GET: PLAY.GEFORCENOW.COM/MALL/",2);
        label(165,495,"ESTA PRUEBA SOLO CONFIRMA RESPUESTA HTTPS.",2);
        label(165,565,"INICIA SESION Y ABRE EL CATALOGO PARA JUGAR.",2);
        panel(120,680,1680,86,selection==0);
        label(165,707,"VOLVER AL INICIO",3);
        label(120,850,"X: REPETIR PRUEBA     CIRCULO: VOLVER",2);
    } else if(page==3) {
        label(120,235,authState.load()==3?"MI CATALOGO GEFORCE NOW":"CATALOGO PUBLICO DE GEFORCE NOW",4);
        // Keep the catalog heading clear; its count/search controls occupy this row.
        const int state=catalogState.load();
        if(state==1) label(120,350,"DESCARGANDO CATALOGO DE NVIDIA...",3);
        else if(state==3) {
            char err[80];
            if(catalogHttpStatus.load()>0) snprintf(err,sizeof(err),"NO SE PUDO CARGAR: HTTP %d",catalogHttpStatus.load());
            else snprintf(err,sizeof(err),"NO SE PUDO CARGAR: ERROR %d",catalogError.load());
            label(120,350,err,3);
            label(120,420,"X: REINTENTAR     CIRCULO: VOLVER",2);
        } else if(state==2) {
            char countLine[160]; snprintf(countLine,sizeof(countLine),"%d/%d JUEGOS | L1/R1 CATEGORIA | L3/R3 FAV | TRIANGULO BUSCAR",catalogVisibleCount,catalogCount.load());
            label(120,270,countLine,2);
            panel(120,300,1040,62,catalogSearchActive);
            char searchLine[100]; snprintf(searchLine,sizeof(searchLine),"BUSCAR: %s%s",catalogSearch.empty()?"ESCRIBE CON TECLADO":catalogSearch.c_str(),catalogSearchActive?"_":"");
            label(150,320,searchLine,2);
            panel(1190,300,610,62,false);
            char filterLine[100]; snprintf(filterLine,sizeof(filterLine),"CATEGORIA: %s  (L1 / R1)",catalogFilters[catalogStoreFilter]);
            label(1220,320,filterLine,2);
            for(int row=0;row<9;row++) {
                const int visibleIndex=catalogOffset+row;
                if(visibleIndex>=catalogVisibleCount) break;
                const int index=catalogVisible[visibleIndex];
                const int y=390+row*58;
                panel(120,y,1190,50,visibleIndex==catalogSelection);
                label(160,y+15,catalogGames[index].title,2);
                label(920,y+15,catalogGameIsFavorite(index)?"FAV":"",2);
                label(1040,y+15,catalogGames[index].store,2);
            }
            panel(1340,390,460,522,false);
            label(1370,408,"VISTA PREVIA",2);
            if(catalogCoverTexture) {
                int imageWidth=0,imageHeight=0; SDL_QueryTexture(catalogCoverTexture,nullptr,nullptr,&imageWidth,&imageHeight);
                if(imageWidth>0 && imageHeight>0) {
                    const float scale=std::min(410.0f/imageWidth,450.0f/imageHeight);
                    const int drawWidth=static_cast<int>(imageWidth*scale),drawHeight=static_cast<int>(imageHeight*scale);
                    SDL_Rect destination={1365+(410-drawWidth)/2,440+(450-drawHeight)/2,drawWidth,drawHeight};
                    SDL_RenderCopy(renderer,catalogCoverTexture,nullptr,&destination);
                }
            } else if(catalogCoverState.load()==1) label(1390,650,"CARGANDO CARATULA...",2);
            else if(catalogCoverState.load()==3) label(1380,650,"CARATULA NO DISPONIBLE",2);
            else label(1420,650,"SIN CARATULA",2);
            if(catalogVisibleCount==0) label(120,865,catalogStoreFilter==6?"NO HAY FAVORITOS. SELECCIONA UN JUEGO Y PULSA R3, L3 O F3.":(catalogStoreFilter==0?"NO HAY JUEGOS VINCULADOS A TU CUENTA.":"NO HAY JUEGOS QUE COINCIDAN. BORRA LA BUSQUEDA O CAMBIA EL FILTRO."),2);
            if(launchState.load()==1) {
                const int elapsed=static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now()-launchStartedAt).count());
                char progress[180];
                const int cloudStatus=launchCloudStatus.load();
                const int queuePosition=launchQueuePosition.load();
                const int seatEtaMs=launchSeatEtaMs.load();
                const int pollCount=launchPollCount.load();
                panel(120,908,1680,96,false);
                if(cloudStatus==1) label(155,920,"NVIDIA ESTA PREPARANDO TU EQUIPO",2);
                else if(cloudStatus==0) label(155,920,"ESPERANDO UN EQUIPO DE JUEGO",2);
                else {
                    snprintf(progress,sizeof(progress),"EQUIPO ASIGNADO; PREPARANDO LA CONEXION...");
                    label(155,920,progress,2);
                }
                if(queuePosition>0)
                    snprintf(progress,sizeof(progress),"COLA %d  |  %d S TRANSCURRIDOS  |  X: CANCELAR",queuePosition,elapsed);
                else if(seatEtaMs>0)
                    snprintf(progress,sizeof(progress),"PREPARANDO  |  ETA %d S  |  %d S TRANSCURRIDOS  |  X: CANCELAR",seatEtaMs/1000,elapsed);
                else
                    snprintf(progress,sizeof(progress),"%d S TRANSCURRIDOS  |  REVISION %d  |  X: CANCELAR",elapsed,pollCount);
                label(155,958,progress,2);
                drawLoadingBeam(155,987,1608,4);
            }
            else if(launchState.load()==2) {
                panel(120,908,1680,96,false);
                label(155,922,"TU EQUIPO ESTA LISTO",3);
                label(155,963,"PULSA X PARA CONECTAR EL STREAM  |  CIRCULO: CANCELAR SESION",2);
            }
            else if(launchState.load()==3) {
                char error[180]; SDL_LockMutex(launchMutex); snprintf(error,sizeof(error),"NO SE PUDO INICIAR: %.150s",launchError); SDL_UnlockMutex(launchMutex);
                panel(120,908,1680,96,false); label(155,922,error,2);
                label(155,963,"X: REINTENTAR  |  CIRCULO: VOLVER AL INICIO",2);
            }
            else if(g_activeRecoverableSession && !activeSessionId.empty()) {
                panel(120,908,1680,96,true);
                char activeBanner[200];
                snprintf(activeBanner,sizeof(activeBanner),"SESION ACTIVA EN CURSO: %.80s",activeGameTitle.empty()?"JUEGO ACTIVO":activeGameTitle.c_str());
                label(155,922,activeBanner,3);
                label(155,963,"X: RECONECTAR  |  TRIANGULO: CERRAR SESION  |  ARRIBA/ABAJO: ELEGIR OTRO JUEGO",2);
            } else label(120,935,"ARRIBA / ABAJO: ELEGIR     X: INICIAR JUEGO     CIRCULO: INICIO",2);
        } else {
            label(120,350,"PULSA X PARA CARGAR EL CATALOGO",3);
        }
    }
    // Frontera contenido -> pie de pagina. Acumula el tiempo del CONTENIDO (que antes se contaba como
    // cabecera). Con esto `header_us` mide la cabecera y `content_us` la pagina.
    opennow::SetCurrentStage(STAGE_DRAW_MENU_FOOTER);
    {
        const uint64_t nowUs = getProcessTimeUs();
        s_phaseAccum[2] += (nowUs > drawTPhase) ? (nowUs - drawTPhase) : 0;
        drawTPhase = nowUs;
    }
    // =============================================================================================
    // PIE DE PAGINA — SE SUPRIME DURANTE LA PARTIDA
    // =============================================================================================
    // Durante un stream, la pantalla es DEL VIDEO. Cualquier cosa que dibuje la UI encima lo tapa,
    // y aqui habia dos elementos que se pintaban siempre:
    //
    //   1. Esta barra de estado con la version y los FPS, en y=1015..1055.
    //   2. El velo de transicion a pantalla completa (ver mas abajo).
    //
    // La barra se sigue dibujando en las pantallas de menu (donde es la unica forma de ver la version
    // que se esta ejecutando), pero NO cuando hay una partida publicada en pantalla. Si el usuario
    // abre las estadisticas o el menu del stream, `streamStatsVisible` lo devuelve: entonces la UI
    // manda y la barra reaparece.
    const bool streamOwnsScreen = (page==5 && hasPublishedStream());
    const bool uiOverStreamVisible = streamStatsVisible || streamMenuVisible;
    if(!streamOwnsScreen || uiOverStreamVisible) {
        fill(120,1015,1680,2,color(49,62,79));
        char footer[200];
        // The version belongs to this existing bar, not to a separate label. Keep the
        // string in sync with $appVersion in scripts/build-ps4.ps1; the decoder tag now
        // reports the software path explicitly so a future hardware backend is visible
        // here too.
        snprintf(footer,sizeof(footer),"AJ  |  PS4  |  %s  |  UI %d FPS  |  VERSION " GFN_APP_VERSION,
                 pigletVideoActive?"GLES GPU / H264 SW":(rendererHardwareAccelerated?"SDL GPU / H264 SW":"SDL VIDEOOUT / H264 SW"),
                 appLoopFps);
        label(120,1035,footer,2);
    }

    // =============================================================================================
    // VELO DE TRANSICION — YA NO CUBRE LA PANTALLA
    // =============================================================================================
    // Antes era `fillAlpha(0,0,W,H,...)`: 1920x1080 = 2.073.600 pixeles MEZCLADOS por frame durante
    // 180 ms. Con el renderizador software de PS4 eso hunde los FPS justo cuando el usuario acaba de
    // cambiar de pantalla, que es cuando mas se nota.
    //
    // AHORA son DOS BANDAS de 14 px arriba y abajo, con bandas opacas degradadas. El efecto se lee
    // igual (la pantalla "parpadea" un instante al cambiar) pero el coste pasa de 2 millones de
    // pixeles mezclados a 24 llamadas de 14 px. **Coste fijo y despreciable.**
    //
    // Y SE SALTA POR COMPLETO EN LA PANTALLA DE JUEGO: durante el stream no hay transiciones de menu
    // que disimular, y cualquier cosa que se dibuje encima del video es un riesgo innecesario.
    if(page!=5) {
        const int veil=pageTransitionAlpha();
        if(veil>0) {
            fillAlpha(0,0,W,14,color(8,11,17),veil);
            fillAlpha(0,H-14,W,14,color(8,11,17),veil);
        }
    }
    opennow::SetCurrentStage(STAGE_DRAW_PRESENT);
    if(page==6) logLaunchDrawStage(4,"present_begin");
    {
        const uint64_t nowUs = getProcessTimeUs();
        s_phaseAccum[3] += (nowUs > drawTPhase) ? (nowUs - drawTPhase) : 0;
        drawTPhase = nowUs;
    }
    presentFrame();
    if(page==6) logLaunchDrawStage(5,"present_complete");

    // =============================================================================================
    // INFORME DE FASES (una linea por segundo)
    // =============================================================================================
    // `present_us` es el dato que decide: si se lleva la mayor parte del frame, el cuello de botella
    // esta en copiar el framebuffer a VideoOut (que es lo que hace SDL_RenderPresent con el
    // renderizador software) y NO en dibujar. Es la distincion que faltaba para saber donde atacar.
    {
        const uint64_t nowUs = getProcessTimeUs();
        s_phaseAccum[4] += (nowUs > drawTPhase) ? (nowUs - drawTPhase) : 0;
        drawTPhase = nowUs;
    }
    ++s_phaseFrames;
    // Acumuladores de primitivas del frame (v3.69). Se suman aqui, al final del dibujado, cuando ya se
    // han contado todas las de este frame.
    static uint64_t s_rectAccum = 0, s_labelAccum = 0;
    s_rectAccum  += g_frameRects;
    s_labelAccum += g_frameLabels;
    if(s_phaseLastLogUs == 0 || (drawT0 - s_phaseLastLogUs) >= 1000000ULL) {
        s_phaseLastLogUs = drawT0;
        const uint64_t n = s_phaseFrames ? s_phaseFrames : 1;
        char phases[320];
        std::snprintf(phases, sizeof(phases),
                      "page=%d clear_us=%llu header_us=%llu content_us=%llu footer_us=%llu "
                      "present_us=%llu total_us=%llu frames=%llu cajas=%llu etiquetas=%llu",
                      page,
                      static_cast<unsigned long long>(s_phaseAccum[0]/n),
                      static_cast<unsigned long long>(s_phaseAccum[1]/n),
                      static_cast<unsigned long long>(s_phaseAccum[2]/n),
                      static_cast<unsigned long long>(s_phaseAccum[3]/n),
                      static_cast<unsigned long long>(s_phaseAccum[4]/n),
                      static_cast<unsigned long long>((s_phaseAccum[0]+s_phaseAccum[1]+
                                                       s_phaseAccum[2]+s_phaseAccum[3]+
                                                       s_phaseAccum[4])/n),
                      static_cast<unsigned long long>(n),
                      // Primitivas POR FRAME: son las copias de memoria que hace el renderizador
                      // software. Sin estos dos numeros, un `content_us` alto no tiene causa conocida.
                      static_cast<unsigned long long>(s_rectAccum/n),
                      static_cast<unsigned long long>(s_labelAccum/n));
        opennow::LogAppLifecycleEvent("UI_DRAW_PHASES", phases);
        s_phaseAccum[0]=s_phaseAccum[1]=s_phaseAccum[2]=s_phaseAccum[3]=s_phaseAccum[4]=0;
        s_phaseFrames = 0;
        s_rectAccum = 0;
        s_labelAccum = 0;
    }
}

// Defined in src/opennow/stream/videodec_abi_check.cpp. Declared here because the
// client does not otherwise include the Videodec ABI header.
extern "C" int32_t opennow_videodec_abi_probe();
extern "C" int opennow_videodec_codec_type_probe(int32_t* out_rc, const char** out_labels);

static void logBuildInventory() {
    // Boot inventory. Answers "what is this build running on?" from the log alone,
    // so a report can be diagnosed without asking which PKG or console was used.
    opennow::LogAppLifecycleEvent("BOOT_BUILD",
        "app_version=" GFN_APP_VERSION " videoout_buffers=4 scaler=threaded decoder=FFmpeg-SW "
        "flip_sync=equeue input_throttle=125Hz");
    opennow::LogAppLifecycleEvent("BOOT_COMPILER", __VERSION__);

    // Direct memory: tells us how much headroom the framebuffers and any hardware
    // decoder surfaces have to share.
    const size_t total = sceKernelGetDirectMemorySize();
    char memDetail[128];
    std::snprintf(memDetail, sizeof(memDetail),
                  "direct_total_mb=%zu framebuffer_mb=7.91x3 at_1080p",
                  total / (1024 * 1024));
    opennow::LogAppLifecycleEvent("BOOT_MEMORY", memDetail);

    // Display mode actually negotiated, rather than the 1080p/60 the client assumes.
    const int32_t handle = sceVideoOutOpen(
        ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (handle > 0) {
        OrbisVideoOutResolutionStatus res{};
        if (sceVideoOutGetResolutionStatus(handle, &res) == 0) {
            char resDetail[160];
            std::snprintf(resDetail, sizeof(resDetail),
                          "width=%u height=%u pane=%ux%u refresh_enum=%llu flags=0x%04X",
                          res.width, res.height, res.paneWidth, res.paneHeight,
                          static_cast<unsigned long long>(res.refreshRate), res.flags);
            opennow::LogAppLifecycleEvent("BOOT_DISPLAY", resDetail);
            // Adaptar la ventana al display real. Sin esto, una ventana de 1920x1080 en un
            // panel de 1280x720 hace que SDL_UpdateWindowSurface falle y la pantalla quede
            // negra mientras la app sigue dibujando (los frames van a una superficie que no
            // se presenta). Causa de la pantalla negra de las 2.91/2.92.
            if (res.width >= 640 && res.height >= 480) {
                W = static_cast<int>(res.width);
                H = static_cast<int>(res.height);
                char adaptDetail[96];
                std::snprintf(adaptDetail, sizeof(adaptDetail),
                              "window_adapted width=%d height=%d", W, H);
                opennow::LogAppLifecycleEvent("BOOT_WINDOW_SIZE", adaptDetail);
            }
        } else {
            opennow::LogAppLifecycleEvent("BOOT_DISPLAY", "stage=get_resolution_failed");
        }
        sceVideoOutClose(handle);
    } else {
        opennow::LogAppLifecycleEvent("BOOT_DISPLAY", "stage=open_bus_main_failed");
    }

    // ARBITRACION DEL DECODIFICADOR DE VIDEO.
    //
    // Este es el paso que faltaba y que YouTube SI hace: en PS4 el decodificador es un recurso
    // compartido con el sistema (grabacion, transmision), asi que hay que solicitarlo por
    // libSceVideoDecoderArbitration antes de intentar cargar VIDEODEC2. Sin este paso, VIDEODEC2
    // ARBITRACION DEL DECODIFICADOR: NO SE LLAMA EN EL ARRANQUE.
    //
    // HISTORIA (importante, para no repetirlo): en la 3.11 se llamo a
    // sceVideoDecoderArbitrationInitialize() en este punto y **la aplicacion dejo de arrancar**.
    // El log se cortaba justo despues de BOOT_WINDOW_SIZE y el sistema mostraba
    // "Se produjo un error en la aplicacion". Las firmas de arbitracion eran DEDUCIDAS (el SDK no
    // trae cabecera para ese modulo) y una firma equivocada en una llamada real al sistema hace que
    // la funcion lea argumentos que nunca se pasaron y termine el proceso.
    //
    // DECISION: la libreria se sigue ENLAZANDO (libSceVideoDecoderArbitration), pero la llamada
    // queda detras del ajuste Configuracion > DECODIFICADOR > HARDWARE y NUNCA en la ruta de
    // arranque. Asi el arranque es siempre seguro y la prueba de hardware es una accion explicita
    // del usuario, con el intento registrado ANTES de llamar.
    opennow::LogAppLifecycleEvent(
        "VDEC_ARB_SKIPPED",
        "reason=deduced_signatures_aborted_boot_in_3.11; enable_via_settings_decoder_hardware");

    // Which of the hardware codec modules are even present. libSceVideodec2 is the
    // one that would lift the ~35 FPS software ceiling; a previous standalone probe
    // found it failing with 0x805A1000 while VIDEODEC/VDECWRAP/AV_PLAYER load.
    struct ModuleProbe { const char* name; int32_t id; };
    const ModuleProbe modules[] = {
        {"VIDEODEC",   ORBIS_SYSMODULE_VIDEODEC},
        {"VIDEODEC2",  ORBIS_SYSMODULE_VIDEODEC2},
        {"VDECWRAP",   ORBIS_SYSMODULE_VDECWRAP},
        {"AV_PLAYER",  ORBIS_SYSMODULE_AV_PLAYER},
    };
    for (const auto& m : modules) {
        const int32_t rc = sceSysmoduleLoadModule(static_cast<OrbisSysModule>(m.id));
        char detail[96];
        std::snprintf(detail, sizeof(detail), "module=%s rc=0x%08X",
                      m.name, static_cast<unsigned>(rc));
        opennow::LogAppLifecycleEvent("BOOT_CODEC_MODULE", detail);
    }

    opennow::LogAppLifecycleEvent("BOOT_INVENTORY_DONE", "stages=build,memory,display,codecs");

    // Runtime check of the libSceVideodec (v1) ABI.
    //
    // The hardware decoder is the only route past the ~35 FPS software ceiling, and
    // v1 is the variant whose sysmodule actually loads on firmware 9.00 (VIDEODEC2
    // fails with 0x805A1000). Its struct sizes were measured with a compiler probe
    // rather than confirmed against firmware, and the library rejects a wrong
    // thisSize with ORBIS_VIDEODEC_ERROR_STRUCT_SIZE (0x80C10002). One call here
    // states definitively whether the ABI is right, before any decoder work is built
    // on top of it.
    const int32_t abi_rc = opennow_videodec_abi_probe();
    char abiDetail[192];
    if (abi_rc == 0) {
        std::snprintf(abiDetail, sizeof(abiDetail),
                      "rc=0x%08X ABI_ACCEPTED struct_sizes=confirmed",
                      static_cast<unsigned>(abi_rc));
    } else if (static_cast<uint32_t>(abi_rc) == 0x80C10002u) {
        std::snprintf(abiDetail, sizeof(abiDetail),
                      "rc=0x%08X STRUCT_SIZE_MISMATCH: our sizes are wrong, do not build on this",
                      static_cast<unsigned>(abi_rc));
    } else if (static_cast<uint32_t>(abi_rc) == 0x80C10001u) {
        // Measured: only the codec type was rejected, so the layouts are correct.
        std::snprintf(abiDetail, sizeof(abiDetail),
                      "rc=0x%08X CODEC_TYPE rejected; struct_sizes=ACCEPTED layouts are correct",
                      static_cast<unsigned>(abi_rc));
    } else {
        std::snprintf(abiDetail, sizeof(abiDetail),
                      "rc=0x%08X (see ORBIS_VIDEODEC_ERROR_* table)",
                      static_cast<unsigned>(abi_rc));
    }
    opennow::LogAppLifecycleEvent("BOOT_VIDEODEC_ABI_PROBE", abiDetail);

    // The v1 hardware API rejects codecType=1 even though vdecsw and videodec2 use
    // it for AVC, and no available source documents the hardware constant. Try every
    // plausible value once and log each result, so one console run identifies it
    // instead of guessing build after build.
    int32_t ct_rc[8] = {0};
    const char* ct_labels[8] = {nullptr};
    const int ct_count = opennow_videodec_codec_type_probe(ct_rc, ct_labels);
    for (int i = 0; i < ct_count && i < 8; ++i) {
        char ctDetail[160];
        std::snprintf(ctDetail, sizeof(ctDetail), "codecType=%s rc=0x%08X %s",
                      ct_labels[i] ? ct_labels[i] : "?",
                      static_cast<unsigned>(ct_rc[i]),
                      ct_rc[i] == 0 ? "<-- ACCEPTED" : "");
        opennow::LogAppLifecycleEvent("BOOT_VIDEODEC_CODEC_TYPE", ctDetail);
    }
}

int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    // =================================================================================================
    // LA TRAZA ARRANCA LO PRIMERO (v4.25)
    // =================================================================================================
    // Tiene que ser la PRIMERA llamada despues de configurar stdout, para que el primer hito de
    // `trace_boot.txt` sea el primero de verdad y el tiempo de los demas pasos sea real. Si se llamara
    // mas tarde, los pasos anteriores quedarian invisibles y el tiempo total no cuadraria.
    opennow::trace::StartTrace();
    opennow::trace::BootStepSync("MAIN_ENTRADA", (std::string("argv0=") + (argc > 0 && argv[0] ? argv[0] : "?")).c_str());
    opennow::trace::BootStep("BeginAppLifecycleLog", "abre diagnostic.log");
    opennow::BeginAppLifecycleLog();
    opennow::trace::BootStep("InstallAppCrashDiagnostics", "manejadores de senal");
    opennow::InstallAppCrashDiagnostics();
    opennow::SetAppCrashContext(page,launchState.load(),streamStartState.load());
    // Keep in sync with $appVersion in scripts/build-ps4.ps1, which is what lands
    // in the PKG's APP_VER/VERSION. The console previously kept reporting 2.70 for
    // every build, so there was no way to tell which revision was installed.
    opennow::trace::BootStep("APP_START", "version=" GFN_APP_VERSION);
    opennow::LogAppLifecycleEvent("APP_START","version=" GFN_APP_VERSION);
    opennow::trace::BootStep("logBuildInventory", "inventario de subsistemas");
    logBuildInventory();
    struct AppExitLog { ~AppExitLog() {
        opennow::trace::BootStepSync("APP_EXIT", "salida normal");
        opennow::trace::StopTrace();
        opennow::LogAppLifecycleEvent("APP_EXIT","normal");
    } } appExitLog;
    opennow::SetStreamDiagnosticsEnabled(true);
    opennow::trace::BootStep("loadSettings", "lee /data/gfnps4/settings.cfg");
    loadSettings();
    opennow::trace::BootStep("loadCatalogFavorites", nullptr);
    loadCatalogFavorites();
    // Show the beta notice unless the user permanently dismissed it.
    betaDisclaimerVisible=!betaDisclaimerDontShowAgain;
    opennow::LogAppLifecycleEvent("BETA_DISCLAIMER_STATE",
        ("visible=" + std::to_string(betaDisclaimerVisible?1:0) +
         " persisted=" + std::to_string(betaDisclaimerDontShowAgain?1:0)).c_str());
    opennow::LogAppLifecycleEvent("APP_SETTINGS_LOADED");
    // EL REALCE SE APLICA AQUI, AL ARRANCAR.
    //
    // Antes vivia SOLO dentro de saveSettings(), que se ejecuta unicamente al pulsar X en "GUARDAR
    // Y VOLVER". En una sesion normal eso no ocurre nunca, asi que el ajuste se guardaba en el
    // fichero y NO llegaba al escalador. Verificado en consola: el log no tenia ni SETTINGS_SAVED ni
    // STREAM_SHARPNESS_APPLIED.
    //
    // Aplicarlo al arrancar garantiza que el valor guardado este activo desde el primer frame, sin
    // depender de que el usuario entre a la pantalla de ajustes. Es el mismo error de patron que
    // tuvo el bitrate en la 2.88.
    applySharpness();

    // REGISTRO DE SESION ACTIVADO POR DEFECTO.
    //
    // Se activa siempre para no depender de que el usuario lo encienda: los dos sintomas que hay
    // que diagnosticar (la degradacion al bajar a 540p y la actualizacion periodica de imagen) solo
    // aparecen en sesion real, y perder esa sesion por no haber activado el registro seria un
    // desperdicio.
    //
    // El coste es bajo: una escritura en un anillo EN MEMORIA por frame, sin tocar el disco. El
    // volcado se hace al terminar y cada 30 s un resumen compacto.
    // MODO DE DECODIFICADOR: se registra al arrancar para que el log diga cual se va a usar.
    // El valor se pasa al decodificador en el arranque del stream (ver session.cpp).
    {
        static const char* const kDMode[]={"AUTO","HARDWARE","SOFTWARE"};
        char dmDetail[96];
        std::snprintf(dmDetail,sizeof(dmDetail),"decoder_mode=%d name=%s",
                      decoderMode, kDMode[(decoderMode>=0&&decoderMode<3)?decoderMode:0]);
        opennow::LogAppLifecycleEvent("STREAM_DECODER_MODE",dmDetail);

        // Modo de escalado: se registra para poder correlacionarlo con el coste medido.
        {
            char sDetail[128];
            std::snprintf(sDetail,sizeof(sDetail),"scale_mode=%d name=%s stream_target=%dx%d",
                          scaleMode, scaleMode==0?"720p_fixed":"1080p_fixed",
                          scaleMode==0?1280:1920, scaleMode==0?720:1080);
            opennow::LogAppLifecycleEvent("STREAM_SCALE_MODE",sDetail);
    opennow::trace::StreamEvent("MODO_ESCALADO", sDetail);
        }

        // PROBE DEL DECODIFICADOR POR HARDWARE AL ARRANCAR EL STREAM.
        //
        // POR QUE AQUI (error de diseno corregido): en la 3.16 el probe solo se disparaba al
        // CAMBIAR el ajuste con X. Si el usuario ya tenia HARDWARE guardado de una sesion anterior,
        // el probe NO se ejecutaba nunca: el log mostraba "decoder_mode=1 name=HARDWARE" y CERO
        // lineas HWVDEC. Eso es exactamente lo que paso en la prueba de la 3.16.
        //
        // Ahora se ejecuta SIEMPRE que el stream arranca con un modo que puede usar hardware (AUTO
        // o HARDWARE). El arranque de la aplicacion sigue sano: esto ocurre al conectar la sesion.
        //
        // Sobre el modulo: `sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC2)` devuelve 0x805A1000,
        // que es SCE_SYSMODULE_ERROR_UNKNOWN (el firmware no tiene ese ID en su tabla). Eso NO
        // significa que la libreria no exista: el cliente la ENLAZA (`-lSceVideodec2`) y YouTube la
        // usa en esta misma consola. Puede que no necesite carga explicita y baste con llamar a sus
        // funciones, o que requiera `sceSysmoduleLoadModuleInternal`. Este probe lo averigua:
        // llama DIRECTAMENTE a la ABI v2 y registra el codigo real de cada paso.
        if (decoderMode==0 || decoderMode==1) {
            char at[160];
            std::snprintf(at, sizeof(at),
                          "trigger=stream_start decoder_mode=%d api=v2 sysmodule_load_unknown_but_linked",
                          decoderMode);
            opennow::LogAppLifecycleEvent("HWVDEC_ATTEMPT", at);
            // PRIMERO la v1: su sysmodule SI carga en este firmware (rc=0x00000000 medido en todas
            // las versiones) y no necesita cola de computo, asi que es la via con posibilidades
            // reales. La v2 se prueba despues y su guardia la abortara sola si el modulo sigue sin
            // cargar, sin riesgo para la aplicacion.
            const opennow::videodec1_probe::VideoDecodeV1ProbeResult pr1 =
                opennow::videodec1_probe::RunVideoDecodeV1Probe();
            (void)pr1;
            const opennow::videodec2_probe::HardwareDecodeProbeResult pr =
                opennow::videodec2_probe::RunHardwareDecodeProbe();
            (void)pr;
        }
    }

    opennow::diag::SetSessionRecorderEnabled(true);
    opennow::diag::ResetSessionRecorder();
    if(SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS)!=0) { opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=sdl"); return 2; }
    int randomRc=sceSysmoduleLoadModule(ORBIS_SYSMODULE_RANDOM);
    if(randomRc<0) {
        opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=random_module");
        char message[128];
        snprintf(message,sizeof(message),"No se pudo cargar libSceRandom. Codigo Orbis: 0x%08X",static_cast<unsigned>(randomRc));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"AJ GeForce NOW PS4",message,NULL);
        SDL_Quit(); return 7;
    }
    unsigned char randomProbe[16];
    randomRc=sceRandomGetRandomNumber(randomProbe,sizeof(randomProbe));
    if(randomRc!=0) {
        opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=random_probe");
        char message[128];
        snprintf(message,sizeof(message),"libSceRandom no genero bytes. Codigo Orbis: 0x%08X",static_cast<unsigned>(randomRc));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"AJ GeForce NOW PS4",message,NULL);
        SDL_Quit(); return 7;
    }
    int networkRc=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    if(networkRc<0) { opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=net_module"); SDL_Quit(); return 6; }
    const int imeModuleRc=sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    const int dialogRc=sceCommonDialogInitialize();
    opennow::LogAppLifecycleEvent("PS4_IME_INIT",("module_rc="+std::to_string(imeModuleRc)+" dialog_rc="+std::to_string(dialogRc)).c_str());
    networkRc=sceNetInit();
    if(networkRc<0) { opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=net_init"); SDL_Quit(); return 6; }
    networkMutex=SDL_CreateMutex();
    authMutex=SDL_CreateMutex();
    launchMutex=SDL_CreateMutex();
    if(!networkMutex || !authMutex || !launchMutex) { opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=mutex"); if(networkMutex) SDL_DestroyMutex(networkMutex); if(authMutex) SDL_DestroyMutex(authMutex); if(launchMutex) SDL_DestroyMutex(launchMutex); sceNetTerm(); SDL_Quit(); return 5; }
    if(rememberLogin) {
        try {
            opennow::GfnClient client; opennow::AuthSession saved;
            if(client.LoadSavedSession(saved)) { saved.persistence_enabled=true; authSession=std::move(saved); authState.store(3); opennow::LogAppLifecycleEvent("AUTH_RESTORED","source=encrypted_local_vault"); }
            else {
                struct stat vaultInfo{},deviceInfo{};
                const bool vaultExists=stat("/data/gfnps4/auth_accounts.json",&vaultInfo)==0;
                const bool deviceExists=stat("/data/gfnps4/device_id.txt",&deviceInfo)==0;
                opennow::LogAppLifecycleEvent("AUTH_RESTORE_MISSING",
                    (std::string("vault=")+(vaultExists?"present":"missing")+
                     " bytes="+(vaultExists?std::to_string(static_cast<long long>(vaultInfo.st_size)):"0")+
                     " device_id="+(deviceExists?"present":"missing")).c_str());
            }
        } catch(...) { opennow::LogAppLifecycleEvent("AUTH_RESTORE_FAILED","reauthentication_required=1"); }
    }
    // =================================================================================================
    // LA RUTA DE VideoOut DIRECTO PASA A SER LA OPCION POR DEFECTO (v3.93). ESTE ES EL CAMBIO DEFINITIVO.
    // =================================================================================================
    // POR QUE, con la evidencia del propio proyecto delante:
    //
    // **1. Es la unica ruta que puede prometer 60 fps.**
    //    Sus medidas de consola, en los documentos del proyecto:
    //
    //        v2.84/2.85/2.86:  VIDEOOUT_HANDOFF_COMPLETE mode=direct_hardware_60fps
    //        v2.79:            escalador avg 8,0-8,7 ms
    //        v3.02:            VIDEOOUT_SCALE_TIMING dispatch_avg_us=8523
    //        v3.25 (6 hilos):  dispatch_avg_us=13766
    //
    //    El presupuesto de un frame a 60 fps es **16.666 us. 8.523 us es la mitad.** Cabe con margen.
    //    En cambio la ruta SDL, medida en la v3.90: `copy_us=24.962` **mas** `updatewindowsurface_us=14.719`
    //    = **39,7 ms de 33 ms**: no cabe ni a 30 fps. **No es cuestion de optimizar mas: no cabe.**
    //
    // **2. Es la unica ruta que puede dar PANTALLA COMPLETA.**
    //    El driver de SDL fija sus buffers de VideoOut al tamano del DISPLAY (1920x1080) y copia dentro
    //    de ellos `window->w x window->h`, **sin escalar** (`SDL_ps4video.c:509-519`). Con un lienzo de
    //    720p en un panel de 1080p eso deja **el video en una esquina con marco negro** — que es
    //    exactamente el sintoma reportado. La ruta directa **registra sus propios buffers** con
    //    `sceVideoOutSetBufferAttribute`, y **es `sceVideoOut` el que escala a la pantalla**, en hardware.
    //
    // **3. Evita el cierre de la v3.90.**
    //    El log de esa version termina en `stage=6 name=PRESENT beat=264`, es decir **dentro del present**,
    //    y el propio proyecto ya documenta el patron (`PS4-V2.81-CRASH-HEARTBEAT.md`): el log "termina en
    //    seco" porque el proceso muere por algo que **no pasa por los manejadores de senal** (el kernel lo
    //    mata: presion de memoria o watchdog). Y la ruta SDL **escribe 8,29 MB a memoria visible por GPU en
    //    cada frame** (`memset` + `memcpy` del driver), frente a los **3,69 MB** de los buffers propios a
    //    720p. Ademas el proyecto ya midio el caso extremo: framebuffer de 1080p -> **1 flip y se cerro**;
    //    720p -> **119 flips estables**.
    //
    // **4. El respaldo ya esta implementado y es automatico.**
    //    El traspaso tiene `initOk`, los buffers se registran con comprobacion de error, y si algo falla:
    //
    //        VIDEOOUT_HANDOFF_FAIL             fallback=sdl_software
    //        restoreSdlFromVideoOut()          SDL vuelve a tomar el control
    //
    //    Es decir: **si la ruta directa no arranca, la app NO se queda sin imagen; vuelve a SDL.**
    //
    // COMO SE DESACTIVA (por si hiciera falta): creando `/data/gfnps4/no_videoout.flag`, que devuelve
    // la app a la ruta SDL. La via es de **exclusion**, no de activacion: antes habia que crear un
    // fichero a mano para TENER los 60 fps, y como nadie lo sabe, la app caia siempre en SDL.
    // =====================================================================================================
    // LA RUTA DIRECTA PASA A SER **OPT-IN** (v4.06). ESTE ES EL CAMBIO QUE DEVUELVE LA ESTABILIDAD.
    // =====================================================================================================
    // QUE HA CAMBIADO Y POR QUE, con los datos de `logs/gfnps4` de la sesion de la v4.05:
    //
    //   `APP_START version=4.05`, **beat=460** (el latido es de 1 s, asi que la app vivio **460 s**).
    //   El stream arranco a los **354,3 s**, y `last_stage.txt` quedo en **`stage=6 name=PRESENT`**.
    //   El log termina a los **356,4 s** -> **la sesion de juego duro ~2 segundos**.
    //
    //   Y el `stage=6` es el dato que decide: **`STAGE_PRESENT` es la etapa del camino SDL**
    //   (`presentFrame()`, `main.cpp`), no la del camino directo (que usa las etapas 7, 8, 10 y 11 de
    //   `PS4VideoOutRenderer`). El cierre ocurre **justo despues de `VIDEOOUT_HANDOFF_COMPLETE`**.
    //
    //   Ademas, en esa misma sesion los menus iban a **`loop_fps=58..59` con `slow_frames=0`**, y al
    //   entrar al stream el coste por frame sube a **`present_us=17037`** sobre un presupuesto de
    //   **16.666 us**: el colapso de FPS y el cierre **coinciden con el traspaso**.
    //
    // DECISION DEL USUARIO, que es la autoridad aqui y ademas es la correcta:
    //
    //   *"El objetivo principal es una experiencia JUGABLE, COMODA y ESTABLE. Prefiero una ruta de
    //    renderizado estable a 30/60fps (como la v3.62) que un experimento de VideoOut que cause
    //    cierres a los pocos segundos."*
    //
    // Por eso la ruta directa **deja de activarse sola**:
    //
    //   - **POR DEFECTO: SDL.** Es la ruta con la que el proyecto alcanzo los 59 fps en la v3.62 y
    //     **la que no se ha visto cerrarse** en 460 s de sesion.
    //   - **Para probar la ruta directa:** crear `/data/gfnps4/direct_videoout.flag`.
    //   - `no_videoout.flag` se sigue respetando (fuerza SDL), pero ya no hace falta.
    //
    // NO se borra nada de la ruta directa: sigue entera, con su traspaso, su respaldo automatico, su
    // cuarentena y su guardian de degradacion. **Simplemente deja de ser el camino por defecto** hasta
    // que haya una sesion en la que se haya visto aguantar.
    {
        struct stat noFlagStat{};
        struct stat directFlagStat{};
        const bool noVideoOutFlag    = (stat("/data/gfnps4/no_videoout.flag",    &noFlagStat)    == 0);
        const bool directVideoOutFlag = (stat("/data/gfnps4/direct_videoout.flag", &directFlagStat) == 0);

        // Por defecto SDL. La ruta directa SOLO con su flag explicito y sin el de exclusion.
        // =========================================================================================
        // v4.16: LA RUTA DIRECTA VUELVE A SER LA DE POR DEFECTO
        // =========================================================================================
        // POR QUE SE REVIVE, con las medidas de los logs de la v4.13 delante:
        //
        //     STREAM_VIDEO_PHASES  lock_us=2  conv_us=31.000  unlock_us=1  copy_us=45.000
        //     UI_LOOP_BUDGET page=5 stream=1  iter_ms=50  frames=20     -> 19-20 fps
        //
        // **Todo el coste es la conversion/escalado a 1920x1080.** El desglose nuevo lo dejo claro:
        // `lock` y `unlock` son 2 us; **los 31 ms son la conversion SSE2 escalando 960x540 -> 1920x1080**,
        // y el blit anade 45 ms mas. **Sobre un presupuesto de 16.666 us, eso no cabe ni de lejos.**
        //
        // Y la ruta SDL **tiene que** escalar a 1920x1080 en CPU, porque el driver de SDL-PS4 fija sus
        // buffers al display y confirma el usuario que la imagen llena la pantalla: **no hay escalado de
        // hardware en ese camino**.
        //
        // La ruta directa, en cambio, **escribe el frame a resolucion nativa** (960x540) en su framebuffer
        // y **no escala nada en CPU**: es la unica que cabe en 16.666 us. El propio analisis del proyecto
        // ya lo concluyo (`PS4-V3.44`, `PS4-V3.62`: 59 fps solo con el video a resolucion nativa).
        //
        // SE VUELVE A ACTIVAR, y para poder diagnosticar el cierre que sufrio, **se anaden marcas de
        // traza alrededor de todo el traspaso**. Si vuelve a cerrarse, el log dira exactamente en que
        // punto. **Con `no_videoout.flag` se puede volver a SDL en un segundo** sin reinstalar nada.
        g_videoOutDebugFlagDetected = !noVideoOutFlag;
        videoOutDirectActive = false;         // se activa durante el stream, al llegar el primer frame

        if(!g_videoOutDebugFlagDetected) {
            opennow::LogAppLifecycleEvent("VIDEOOUT_DISABLED_OPT_IN",
                noVideoOutFlag
                    ? "path=/data/gfnps4/no_videoout.flag usando_ruta=SDL"
                    : "ruta=SDL por defecto motivo=estabilidad_la_directa_cerro_en_2s_en_la_405 "
                      "activar_con=/data/gfnps4/direct_videoout.flag");
        } else {
            opennow::LogAppLifecycleEvent("VIDEOOUT_ENABLED_BY_FLAG",
                "path=/data/gfnps4/direct_videoout.flag ruta=directa respaldo=SDL automatico "
                "aviso=ruta_en_pruebas_puede_cerrarse");
            // =========================================================================================
            // NO SE HACE UN AUTO-TEST DE VideoOut EN EL ARRANQUE. DECISION DELIBERADA.
            // =========================================================================================
            // El codigo anterior hacia `Initialize()` + `PresentTestPattern()` + `Shutdown()` aqui,
            // pero **solo cuando existia `/data/gfnps4/debug_videoout.flag`**, que hay que crear a mano.
            // Es decir: **ese auto-test NUNCA se ha ejecutado en consola.** Y aqui hay un riesgo real de
            // orden que si se ejecutaria:
            //
            //     linea 6987: (aqui)  auto-test -> abre BUS_MAIN, registra buffers, los libera
            //     linea 7008:         Piglet intenta inicializarse
            //     linea 7031:         `SDL_CreateWindow` -> SDL abre BUS_MAIN y registra SUS buffers
            //
            // Si el cierre del auto-test no deja BUS_MAIN perfectamente libre, **SDL no conseguiria la
            // ventana** y la app se quedaria sin imagen en los menus. Eso es exactamente la clase de
            // fallo nuevo que no se debe introducir en una entrega cuyo objetivo incluye la estabilidad.
            //
            // LA VALIDACION YA EXISTE, y en el momento correcto: el traspaso SDL -> VideoOut se hace al
            // llegar el primer frame decodificado, comprueba `initOk`, y si algo falla registra
            // `VIDEOOUT_HANDOFF_FAIL fallback=sdl_software` y llama a `restoreSdlFromVideoOut()`.
            // **Los menus siguen siempre en SDL, y el video directo se valida cuando toca.**
        }

        // =============================================================================================
        // MODO DE PRUEBA DEL ESCALADOR DE VideoOut (v3.99). RESULTADO VISUAL, SIN NECESIDAD DE LOGS.
        // =============================================================================================
        // QUE RESUELVE: la pregunta que el proyecto declaro indeducible del codigo
        // (`PS4-V3.56` §2): **si se registra un framebuffer MAS PEQUENO que el panel (960x540 en un
        // panel de 1080p), lo estira la GPU a pantalla completa o se presenta a su tamanio?**
        //
        // POR QUE NO SE USA LA SONDA AISLADA: `src/probes/videoout_scaler_probe.c` esta escrita y
        // empaquetada (`IV0000-VOSC00001_00-AJVIDEOOUTSCALER.pkg`) pero **se cierra sin escribir ni una
        // linea de log**, asi que no ha dado ninguna respuesta. Y **el cliente SI arranca** (es el
        // binario que el usuario ejecuta a diario), asi que la prueba se hace aqui y no en un binario
        // aparte que nunca ha llegado a ejecutarse.
        //
        // COMO SE USA: crear el fichero `/data/gfnps4/test_scaler.flag` y arrancar la app. Se vera el
        // patron de prueba durante unos 8 segundos y despues la app sigue normalmente.
        //
        // COMO SE LEE EL RESULTADO, y esto NO necesita log:
        //
        //   - El **borde blanco pegado a los cuatro limites de la pantalla**
        //     -> **EL ESCALADOR EXISTE.** Se puede registrar el framebuffer al tamanio del stream
        //        (960x540) y **el escalado en CPU pasa de 8.523-13.766 us a CERO**.
        //
        //   - El **borde blanco enmarca un rectangulo mas pequeno, con marco negro alrededor**
        //     -> **NO hay escalado automatico**, y el framebuffer tiene que ser del tamanio del panel
        //        (que es exactamente lo que hace el cliente).
        //
        // El patron tiene ademas una esquina BLANCA y otra NEGRA arriba, y una diagonal amarilla, asi
        // que tambien se distingue si el estirado es uniforme (la diagonal tiene que seguir recta).
        {
            // =============================================================================================
            // ESTE MODO DE PRUEBA USA UN FLAG **PROPIO** Y NO EL GENERAL (v4.03)
            // =============================================================================================
            // POR QUE SE SEPARAN LOS FLAGS, porque es un riesgo que yo mismo introduje y luego entendi:
            //
            // Este bloque ejecuta `Initialize(960, 540)` y `Shutdown()` **ANTES** de que SDL cree su
            // ventana:
            //
            //     aqui:  Initialize -> abre BUS_MAIN, registra buffers
            //     aqui:  Shutdown   -> los libera
            //     luego: `SDL_CreateWindow` -> **SDL necesita BUS_MAIN**
            //
            // **Esa secuencia NUNCA se ha ejecutado en consola.** Si el cierre no deja BUS_MAIN
            // impecablemente libre, `SDL_CreateWindow` falla, la app registra `APP_INIT_FAILED stage=window`
            // y **sale sin mostrar nada** — una pantalla negra sin explicacion, que es justo el sintoma
            // que hay que evitar.
            //
            // Y el problema era que usaba **el mismo flag** (`/data/gfnps4/test_scaler.flag`) que el
            // usuario podria crear para el metodo seguro. Es decir: **crear el flag del metodo seguro
            // disparaba el metodo arriesgado.**
            //
            // AHORA:
            //   - `/data/gfnps4/test_scaler.flag`  -> reservado para el metodo **SEGURO** (en el momento
            //     del stream, donde el traspaso SDL -> VideoOut ya se hace de todas formas y el respaldo
            //     automatico existe). **Todavia no implementado**: si el fichero existe, se registra un
            //     aviso y NO se hace nada arriesgado.
            //   - `/data/gfnps4/test_scaler_boot.flag` -> este metodo, el del arranque. **Nombre explicito
            //     y distinto a proposito**, para que solo lo dispare quien lea esta documentacion.
            //   - **La via RECOMENDADA sigue siendo la sonda arreglada** (`IV0000-VOSC00001`), que es un
            //     binario aparte y **no toca el cliente**.
            struct stat scalerFlagStat{};
            struct stat scalerBootFlagStat{};
            if(stat("/data/gfnps4/test_scaler.flag", &scalerFlagStat) == 0) {
                opennow::LogAppLifecycleEvent("VIDEOOUT_SCALER_TEST_FLAG_SAFE_PATH",
                    "existe=/data/gfnps4/test_scaler.flag metodo=seguro_en_stream "
                    "estado=no_implementado_todavia accion=ninguna "
                    "nota=use_la_sonda_o_test_scaler_boot.flag");
            }
            if(stat("/data/gfnps4/test_scaler_boot.flag", &scalerBootFlagStat) == 0) {
                opennow::LogAppLifecycleEvent("VIDEOOUT_SCALER_TEST_BEGIN",
                    "framebuffer=960x540 panel=1080p duracion_s=8 lee_la_pantalla=1 "
                    "aviso=secuencia_initialize_shutdown_antes_de_sdl_NUNCA_PROBADA");
                if(opennow::PS4VideoOutRenderer::Initialize(960, 540)) {
                    // Se presenta el patron varias veces durante 8 segundos. El escalador de hardware o
                    // estira el buffer o no; en los dos casos el patron es el mismo y la lectura visual
                    // es la que decide.
                    opennow::LogAppLifecycleEvent("VIDEOOUT_SCALER_TEST_PRESENTING",
                        "presentando 16 veces a 0.5 s; borde_blanco=referencia");
                    for(int i = 0; i < 16; ++i) {
                        opennow::PS4VideoOutRenderer::PresentTestPattern();
                        sceKernelUsleep(500000);   // 0.5 s entre presentaciones
                    }
                    opennow::PS4VideoOutRenderer::Shutdown();
                    opennow::LogAppLifecycleEvent("VIDEOOUT_SCALER_TEST_END",
                        "resultado=leer_la_pantalla borde_blanco_en_los_limites=ESCALADOR_EXISTE "
                        "borde_blanco_pequeno_con_marco_negro=NO_HAY_ESCALADO");
                } else {
                    opennow::LogAppLifecycleEvent("VIDEOOUT_SCALER_TEST_FAIL",
                        "reason=initialize_960x540_fallo la_via_esta_bloqueada");
                }
            }
        }
    }

    // Subfase 1.1: Piglet/EGL es el ÃƒÆ’Ã‚Âºnico dueÃƒÆ’Ã‚Â±o visible de la pantalla y VideoOut.
    // SDL dibuja a una superficie RGBA off-screen sin crear ventana visible.
    pigletVideoActive=PS4PigletVideoRenderer::Initialize();
    if(pigletVideoActive) {
        pigletOverlaySurface=SDL_CreateRGBSurfaceWithFormat(0,W,H,32,SDL_PIXELFORMAT_RGBA32);
        renderer=pigletOverlaySurface?SDL_CreateSoftwareRenderer(pigletOverlaySurface):nullptr;
        if(!renderer) {
            opennow::LogAppLifecycleEvent("PIGLET_FALLBACK_SDL","reason=offscreen_renderer_failed");
            if(pigletOverlaySurface) { SDL_FreeSurface(pigletOverlaySurface); pigletOverlaySurface=nullptr; }
            PS4PigletVideoRenderer::Shutdown();
            pigletVideoActive=false;
        } else {
            rendererHardwareAccelerated=true;
            rendererUsesWindowSurface=false;
            opennow::LogAppLifecycleEvent("VIDEO_RENDERER_READY","name=Piglet GLES2 compositor mode=gpu_yuv");
        }
    } else {
        opennow::LogAppLifecycleEvent("PIGLET_FALLBACK_SDL","reason=piglet_init_failed");
    }
    if(!pigletVideoActive) {
        if(!SDL_WasInit(SDL_INIT_VIDEO)) {
            if(SDL_InitSubSystem(SDL_INIT_VIDEO)!=0) {
                opennow::LogAppLifecycleEvent("PIGLET_FALLBACK_SDL_VIDEO_FAIL",SDL_GetError());
            }
        }
        window=SDL_CreateWindow("GFN PS4",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,W,H,0);
        if(!window) { opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=window"); sceNetTerm(); SDL_Quit(); return 3; }
        // =============================================================================================
        // AQUI FALTABA EL LIENZO PROPIO (v3.91). ESTE ERA EL FALLO DE FORMATO.
        // =============================================================================================
        // En la v3.90 el log decia:
        //
        //     VIDEO_RENDERER_READY name=software mode=software flags=9
        //
        // y **NO** decia `SDL_OWN_CANVAS_ON`. Eso significa que se tomo ESTE camino, que creaba el
        // renderizador sobre la **superficie de la VENTANA** (que el driver declara `BGR888`, 3 bytes) y
        // **nunca creaba el lienzo propio**. Y entonces el bloque de mas abajo, que es donde estaba la
        // creacion del lienzo, **no se ejecutaba** (`if(renderer && !pigletVideoActive)`).
        //
        // CONSECUENCIA MEDIDA EN CONSOLA (v3.90): con la textura del video en `ARGB8888` (4 bytes) y el
        // lienzo en `BGR888` (3 bytes), **los formatos no coinciden**, asi que `SDL_LowerBlitScaled`
        // (`SDL_surface.c:897-903`) cae a `SDL_LowerBlit` **aunque los tamanos sean iguales**. Resultado:
        // blit 1:1 de 1280x720 -> `copy_us` = **24.962 us** (25 ms de un frame de 33 ms) = 27 ns/pixel.
        //
        // **Aqui se crea el lienzo propio, en los DOS caminos**, para que el formato sea `ARGB8888`
        // (igual que la textura del video) y SDL pueda usar su copia rapida de filas.
        {
            const int cw = W>0?W:1920, ch = H>0?H:1080;
            // v4.06: el formato del lienzo tiene que COMPENSAR la cadena del driver. Ver el bloque largo de
        // `GFN_CANVAS_PIXELFORMAT`: el driver envia [R][G][B] y VideoOut lo lee como [B][G][R], asi que el
        // lienzo tiene que estar en el formato cuyo byte bajo es R. Con respaldo: si el driver no
        // soportara ese formato, se cae al de antes en vez de quedarse sin lienzo.
        g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, GFN_CANVAS_PIXELFORMAT);
        if(!g_ownCanvas) {
            opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_FORMAT_FALLBACK",
                "formato_pedido=SDL_PIXELFORMAT_BGR888 motivo=no_soportado usando=ARGB8888");
            g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, SDL_PIXELFORMAT_ARGB8888); // RESPALDO_DECLARADO
        }
            SDL_Surface* surface = g_ownCanvas;
            if(!surface) {
                opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_FAILED", SDL_GetError());
                surface = SDL_GetWindowSurface(window);
            } else {
                char cb[176];
                std::snprintf(cb,sizeof(cb),
                              "canvas=%dx%d fmt=%s pitch=%d (propio, en RAM) "
                              "motivo=formatos_iguales_para_blit_rapido",
                              cw,ch,SDL_GetPixelFormatName(surface->format->format),surface->pitch);
                opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_ON", cb);
            }
            renderer=surface?SDL_CreateSoftwareRenderer(surface):nullptr;
        }
        applyLogicalSize();
        rendererUsesWindowSurface=renderer!=nullptr;
        rendererHardwareAccelerated=false;
    }
    if(renderer && !pigletVideoActive) {
        SDL_RendererInfo info{};
        if(SDL_GetRendererInfo(renderer,&info)==0) {
            rendererHardwareAccelerated=(info.flags&SDL_RENDERER_ACCELERATED)!=0;
            opennow::LogAppLifecycleEvent("VIDEO_RENDERER_READY",
                ("name="+std::string(info.name?info.name:"unknown")+
                 " mode="+(rendererHardwareAccelerated?"accelerated":"software")+
                 " flags="+std::to_string(info.flags)).c_str());
        } else {
            rendererHardwareAccelerated=false;
            opennow::LogAppLifecycleEvent("VIDEO_RENDERER_INFO_FAILED",SDL_GetError());
        }
        // =============================================================================================
        // EL RENDERIZADOR EN USO DIBUJA EN LA SUPERFICIE DE LA VENTANA. HAY QUE SABER SU FORMATO. (v3.91)
        // =============================================================================================
        // ESTE ES EL DATO QUE EXPLICA POR QUE EL BLIT ES LENTO AUNQUE SEA 1:1.
        //
        // `SDL_LowerBlitScaled` (SDL_surface.c:897-903) solo usa su camino rapido cuando se cumplen DOS
        // condiciones: **los tamanos coinciden Y LOS FORMATOS COINCIDEN**. Si el formato no coincide, cae
        // a `SDL_LowerBlit`, que **convierte pixel a pixel**.
        //
        // El driver de SDL-PS4 **declara** la superficie de la ventana como `SDL_PIXELFORMAT_BGR888`
        // (3 bytes, `SDL_ps4video.c:456`), y la textura del video es `ARGB8888` (4 bytes). **No
        // coinciden.** Medido en consola con la v3.90: blit 1:1 de 1280x720 -> `copy_us` = **24.962 us**
        // (25 ms de un frame de 33 ms), que es 27 ns/pixel: el coste de la conversion por pixel, no el de
        // una copia.
        //
        // Aqui se registra el formato REAL para no tener que suponerlo. Y se registra tambien si el
        // lienzo propio esta activo (que es `ARGB8888` y por tanto SI coincide con la textura).
        {
            SDL_Surface* ws = SDL_GetWindowSurface(window);
            char fd[256];
            std::snprintf(fd,sizeof(fd),
                          "superficie_ventana=%s bpp=%d pitch=%d lienzo_propio=%s "
                          "lienzo_fmt=%s COINCIDEN_LIENZO_Y_VENTANA=%d coincide_con_textura=%d",
                          ws && ws->format ? SDL_GetPixelFormatName(ws->format->format) : "nula",
                          ws && ws->format ? static_cast<int>(ws->format->BitsPerPixel) : 0,
                          ws ? ws->pitch : 0,
                          g_ownCanvas ? "si" : "NO",
                          (g_ownCanvas && g_ownCanvas->format)
                              ? SDL_GetPixelFormatName(g_ownCanvas->format->format) : "nula",
                          (ws && ws->format && g_ownCanvas && g_ownCanvas->format &&
                           ws->format->format == g_ownCanvas->format->format) ? 1 : 0,
                          (g_ownCanvas && g_ownCanvas->format &&
                           g_ownCanvas->format->format == SDL_PIXELFORMAT_ARGB8888) ? 1 : 0);
            opennow::LogAppLifecycleEvent("SDL_WINDOW_SURFACE_FORMAT", fd);
        }
    } else {
        const std::string acceleratedError=SDL_GetError();
        SDL_ClearError();
        if(window) {
            // =========================================================================================
            // LIENZO PROPIO EN 4 BYTES (v3.84). ARREGLA EL PARPADEO Y EL RENDIMIENTO A LA VEZ.
            // =========================================================================================
            // DOS PROBLEMAS CON UNA MISMA CAUSA:
            //
            //   1. **Parpadeo con cambios de color** (v3.83). Se escribia el video DIRECTAMENTE en la
            //      superficie de la VENTANA, que es **la misma memoria que `SDL_UpdateWindowSurface`
            //      copia a VideoOut**. Con 4 buffers rotando, la pantalla acababa mostrando buffers que
            //      mezclan la escritura del video con la copia del driver.
            //   2. **Rendimiento** (v3.82, 19 fps). El renderizador dibujaba en la superficie de la
            //      ventana, **declarada `BGR888` (3 bytes)** por el driver, mientras la textura del
            //      video es `ARGB8888` (4 bytes). **Al no coincidir los formatos**,
            //      `SDL_LowerBlitScaled` (`SDL_surface.c:897-903`) NO usa `SDL_SoftStretch` y cae a
            //      `SDL_LowerBlit`, que **convierte pixel a pixel**: medido en consola, **27,4 ns por
            //      pixel de destino**, frente a **1,25 ns/px** de una simple limpieza del mismo lienzo.
            //
            // LA MISMA SOLUCION ARREGLA LAS DOS: **que el renderizador dibuje en un LIENZO PROPIO en
            // memoria normal, de formato `ARGB8888` (4 bytes).**
            //
            //   - El driver y el renderizador **nunca tocan la misma memoria** -> se acaba el parpadeo.
            //   - El lienzo y la textura del video son **los dos `ARGB8888`** -> SDL entra en su camino
            //     rapido (`SDL_SoftStretch`, replicacion de filas) y el blit deja de costar 27 ns/px.
            //
            // Y es lo COHERENTE con el driver: el mismo que **declara** el lienzo de 3 bytes lo
            // **recorre con paso de 32 bits** (`uint32_t* pDst`, `SDL_ps4video.c:499`), asi que el
            // lienzo real es de 4 bytes. Ponerlo a 3 bytes fue lo que dio el blanco y negro de la v3.81.
            //
            // En `presentFrame()` se copia este lienzo a la superficie de la ventana (una copia lineal
            // por fila) y se llama a `SDL_UpdateWindowSurface`, que es quien hace el flip de VideoOut.
            const int cw = W>0?W:1920, ch = H>0?H:1080;
            // v4.06: el formato del lienzo tiene que COMPENSAR la cadena del driver. Ver el bloque largo de
        // `GFN_CANVAS_PIXELFORMAT`: el driver envia [R][G][B] y VideoOut lo lee como [B][G][R], asi que el
        // lienzo tiene que estar en el formato cuyo byte bajo es R. Con respaldo: si el driver no
        // soportara ese formato, se cae al de antes en vez de quedarse sin lienzo.
        g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, GFN_CANVAS_PIXELFORMAT);
        if(!g_ownCanvas) {
            opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_FORMAT_FALLBACK",
                "formato_pedido=SDL_PIXELFORMAT_BGR888 motivo=no_soportado usando=ARGB8888");
            g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, SDL_PIXELFORMAT_ARGB8888); // RESPALDO_DECLARADO
        }
            SDL_Surface* surface = g_ownCanvas;
            if(!surface) {
                // Respaldo: si no se puede crear el lienzo propio, se usa el de la ventana (como antes).
                opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_FAILED", SDL_GetError());
                surface = SDL_GetWindowSurface(window);
            } else {
                char cb[176];
                std::snprintf(cb,sizeof(cb),
                              "canvas=%dx%d fmt=%s pitch=%d (propio, en RAM) motivo=color_correcto_y_blit_rapido",
                              cw,ch,SDL_GetPixelFormatName(surface->format->format),surface->pitch);
                opennow::LogAppLifecycleEvent("SDL_OWN_CANVAS_ON", cb);
            }
            renderer=surface?SDL_CreateSoftwareRenderer(surface):nullptr;
            applyLogicalSize();
            rendererUsesWindowSurface=renderer!=nullptr;
            if(renderer) opennow::LogAppLifecycleEvent("VIDEO_RENDERER_READY",
                g_ownCanvas ? "name=SDL own-canvas fmt=BGR888 mode=software present=explicit_videoout_flip"
                            : "name=SDL window-surface mode=software present=explicit_videoout_flip");
        }
    }
    if(!renderer) { opennow::LogAppLifecycleEvent("APP_INIT_FAILED","stage=renderer"); if(window) SDL_DestroyWindow(window); if(pigletVideoActive) PS4PigletVideoRenderer::Shutdown(); sceNetTerm(); SDL_Quit(); return 4; }
    const bool fontLoaded=loadUiFont();
    opennow::LogAppLifecycleEvent(fontLoaded?"UI_FONT_READY":"UI_FONT_FALLBACK","font=Gontserrat");
    // Los QR de "ACERCA DE" se decodifican EN HILOS DE FONDO aqui, al arrancar. Si se hiciera la
    // primera vez que se entra en la pantalla, el menu se quedaria clavado mientras se decodifican
    // los tres PNG. Al arrancar, ese coste no lo nota nadie.
    startQrLoading();
    SDLVideoRenderer::SetRenderTarget(renderer);
SDLVideoRenderer::SetRenderWindow(window);
SDLVideoRenderer::SetRenderCanvas(g_ownCanvas);   // el escalador escribe en el LIENZO PROPIO
    controllerCount=SDL_NumJoysticks();
    opennow::trace::BootStepSync("APP_READY", "bucle principal a punto de arrancar");
    opennow::LogAppLifecycleEvent("APP_READY",("controllers="+std::to_string(controllerCount)).c_str());
    const bool keyboardReady=initPs4Keyboard();
    opennow::LogAppLifecycleEvent(keyboardReady?"USB_KEYBOARD_READY":"USB_KEYBOARD_SDL_FALLBACK",
        ("init_rc="+std::to_string(ps4KeyboardInitRc)+" open_rc="+std::to_string(ps4KeyboardOpenRc)).c_str());
    const bool nativePadReady=initPs4Pad();
    opennow::LogAppLifecycleEvent(nativePadReady?"NATIVE_PS4_PAD_READY":"NATIVE_PS4_PAD_INITIAL_PROBE",
        ("handle="+std::to_string(ps4PadOpenRc)).c_str());
    const bool mouseReady=initPs4Mouse();
    opennow::LogAppLifecycleEvent(mouseReady?"USB_MOUSE_READY":"USB_MOUSE_UNAVAILABLE",
        ("handle="+std::to_string(ps4MouseHandle)+" input_mode="+std::to_string(inputDevice)).c_str());
    opennow::LogAppLifecycleEvent("UI_PAGE","home");
    for(int i=0;i<controllerCount;i++) {
        if(SDL_IsGameController(i)) {
            SDL_GameController* controller=SDL_GameControllerOpen(i);
            if(i<8) gameControllers[i]=controller;
            if(controller) {
                SDL_Joystick* joystick=SDL_GameControllerGetJoystick(controller);
                char guid[64]{};
                if(joystick) SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick),guid,sizeof(guid));
                char* mapping=SDL_GameControllerMapping(controller);
                std::string detail="index="+std::to_string(i)+" name="+(SDL_GameControllerName(controller)?SDL_GameControllerName(controller):"unknown")+
                    " guid="+guid+" mapping="+(mapping?mapping:"unavailable");
                if(detail.size()>700) detail.resize(700);
                opennow::LogAppLifecycleEvent("GAMEPAD_MAPPING",detail.c_str());
                if(mapping) SDL_free(mapping);
            }
        }
        else { SDL_Joystick* joystick=SDL_JoystickOpen(i); if(i<8) {
            genericJoysticks[i]=joystick;
            if(joystick && SDL_JoystickNumAxes(joystick)>=6) {
            genericSplitTriggerAxes[i]=SDL_JoystickGetAxis(joystick,2)<-20000;
            genericTriggerRest[i][0]=SDL_JoystickGetAxis(joystick,genericTriggerAxis(0));
            genericTriggerRest[i][1]=SDL_JoystickGetAxis(joystick,genericTriggerAxis(1));
                opennow::LogAppLifecycleEvent("GENERIC_PAD_AXIS_MAP",("device="+std::to_string(i)+" axes="+std::to_string(SDL_JoystickNumAxes(joystick))+" split_triggers="+(genericSplitTriggerAxes[i]?"1":"0")).c_str());
            }
        } }
    }
    bool running=true; SDL_Event ev;
    Uint32 perfSampleStart=SDL_GetTicks(),perfMaxRenderMs=0;
    uint32_t perfSampleFrames=0,perfSlowFrames=0;
    Uint32 launchUiHeartbeatAt=0;
    Uint32 lastActionTraceFlushAt=SDL_GetTicks();
    // Arranca el latido persistente. Marca cada etapa del bucle en
    // /data/gfnps4/last_stage.txt para que un fallo duro (que no ejecuta los manejadores de
    // senal) siga siendo diagnosticable. Se anaden etapas nuevas al final de esta lista.
    opennow::trace::BootStep("StartStageHeartbeat", "latido de etapa a disco");
    enum : int {
        STAGE_IDLE=0, STAGE_CATALOG, STAGE_EVENTS, STAGE_INPUT, STAGE_STREAM_UI, STAGE_DRAW, STAGE_PRESENT,
        STAGE_VIDEO_PICK, STAGE_VIDEO_CONVERT, STAGE_VIDEO_SCALE, STAGE_VIDEO_FLIP, STAGE_VIDEO_WAIT
    };
    while(running) {
      try {
        opennow::SetCurrentStage(STAGE_IDLE);
        const Uint32 frameStartMs=SDL_GetTicks();
        publishPendingCatalog();
        const Uint32 uiNow=SDL_GetTicks();

        // Watchdog de Piglet: monitorea que la presentaciÃƒÆ’Ã‚Â³n no se detenga tras el primer swap exitoso
        if(pigletVideoActive) {
            const uint32_t successfulSwaps=PS4PigletVideoRenderer::GetSuccessfulSwaps();
            if(successfulSwaps>0) {
                const uint64_t nowMs=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                const uint64_t firstSwapMs=PS4PigletVideoRenderer::GetFirstSwapTimestampMs();
                const uint64_t lastSwapMs=PS4PigletVideoRenderer::GetLastSwapTimestampMs();
                if(nowMs>=firstSwapMs+3000 && nowMs>=lastSwapMs+3000) {
                    opennow::LogAppLifecycleEvent("PIGLET_WATCHDOG_TRIGGER","reason=stall_after_first_swap");
                    (void)fallbackFromPiglet("reason=watchdog_stall_after_first_swap");
                } else {
                    static uint64_t lastWatchdogClearLogMs=0;
                    if(nowMs-lastWatchdogClearLogMs>=5000) {
                        lastWatchdogClearLogMs=nowMs;
                        char detail[96];
                        snprintf(detail,sizeof(detail),"swaps=%u elapsed_ms=%llu",
                                 successfulSwaps,static_cast<unsigned long long>(nowMs-firstSwapMs));
                        opennow::LogAppLifecycleEvent("PIGLET_WATCHDOG_CLEAR",detail);
                    }
                }
            }
        }
        opennow::SetAppCrashContext(page,launchState.load(std::memory_order_relaxed),
                                    streamStartState.load(std::memory_order_relaxed));
        if(traceLastPage!=page) {
            traceLastPage=page;
            opennow::TraceAppAction("UI_PAGE",("page="+std::to_string(page)+" selection="+std::to_string(selection)).c_str());
        }
        if(page==6 && !launchUiLoopEnteredLogged) {
            opennow::LogAppLifecycleEvent("UI_LAUNCH_LOOP_ENTERED");
            launchUiLoopEnteredLogged=true;
        }
        if(page==6 && static_cast<Uint32>(uiNow-launchUiHeartbeatAt)>=1000u) {
            launchUiHeartbeatAt=uiNow;
            const std::string detail="page=6 state="+std::to_string(launchState.load())+
                " queue="+std::to_string(launchQueuePosition.load())+
                " polls="+std::to_string(launchPollCount.load())+
                " cloud_status="+std::to_string(launchCloudStatus.load())+
                " elapsed_s="+std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now()-launchStartedAt).count());
            opennow::LogAppLifecycleEvent("UI_LAUNCH_HEARTBEAT_1S",detail.c_str());
        }
        if(streamStartState.load(std::memory_order_acquire)==2 && !streamFirstLoopLogged) {
            opennow::WriteStreamStartupStage("stream_ui_loop_entered");
            opennow::LogAppLifecycleEvent("STREAM_UI_LOOP_ENTERED");
            streamFirstLoopLogged=true;
        }
        // Adopt the completed worker before dispatching input or polling the pad.
        // Previously a loop could observe state=2 while activeStream was still
        // unpublished, leaving the first post-connect event pass in a split state.
        if(streamStartThread && streamStartState.load(std::memory_order_acquire)!=1) {
            if(streamStartState.load(std::memory_order_acquire)==2 && !streamJoinBeginLogged) {
                opennow::WriteStreamStartupStage("stream_ui_join_begin");
                opennow::LogAppLifecycleEvent("STREAM_UI_JOIN_BEGIN");
                streamJoinBeginLogged=true;
            }
            joinAndAdoptStreamWorker();
            if(streamStartState.load(std::memory_order_acquire)==2 && !streamJoinDoneLogged) {
                opennow::WriteStreamStartupStage("stream_ui_join_complete");
                opennow::LogAppLifecycleEvent("STREAM_UI_JOIN_COMPLETE");
                streamJoinDoneLogged=true;
            }
        }
        opennow::SetCurrentStage(STAGE_EVENTS);
        while(SDL_PollEvent(&ev)) {
            // Strict modal input trap: while the beta disclaimer is visible its
            // events are consumed here so nothing navigates the page underneath.
            if(consumeDisclaimerEvent(ev)) continue;
            traceSdlAction(ev,page);
            if(ev.type==SDL_QUIT) running=false;
            else if(ev.type==SDL_KEYDOWN) {
                if(page==5 && hasPublishedStream() && ps4KeyboardHandle<0 && inputDevice!=0) {
                    const Uint16 exitMods=static_cast<Uint16>(KMOD_CTRL|KMOD_SHIFT);
                    if(ev.key.keysym.sym==SDLK_F1) { streamMenuVisible=!streamMenuVisible; streamMenuSelection=0; }
                    else if(streamMenuVisible) {
                        if(ev.key.keysym.sym==SDLK_UP || ev.key.keysym.sym==SDLK_w) nav(-1);
                        else if(ev.key.keysym.sym==SDLK_DOWN || ev.key.keysym.sym==SDLK_s) nav(1);
                        else if(ev.key.keysym.sym==SDLK_RETURN || ev.key.keysym.sym==SDLK_SPACE) activateStreamMenu();
                        else if(ev.key.keysym.sym==SDLK_ESCAPE || ev.key.keysym.sym==SDLK_BACKSPACE) streamMenuVisible=false;
                    }
                    else if(ev.key.keysym.sym==SDLK_F10 && (ev.key.keysym.mod&exitMods)==exitMods) leaveStream();
                    else {
                        opennow::input::KeyboardStroke stroke{};
                        if(mapSdlKey(ev.key.keysym.sym,ev.key.keysym.mod,stroke))
                            activeStream->send_keyboard_key(stroke.keycode,stroke.scancode,stroke.modifiers,true);
                    }
                } else if(page!=5) handleKey(ev.key.keysym.sym);
            }
            else if(ev.type==SDL_TEXTINPUT && page==3 && ps4KeyboardHandle<0) editCatalogSearch(ev.text.text);
            else if(ev.type==SDL_KEYUP && page==5 && hasPublishedStream() && ps4KeyboardHandle<0 && inputDevice!=0) {
                opennow::input::KeyboardStroke stroke{};
                if(ev.key.keysym.sym!=SDLK_F1 && mapSdlKey(ev.key.keysym.sym,ev.key.keysym.mod,stroke))
                    activeStream->send_keyboard_key(stroke.keycode,stroke.scancode,stroke.modifiers,false);
            }
            else if(ev.type==SDL_JOYHATMOTION && page!=5 && !ps4PadReady && !SDL_GameControllerFromInstanceID(ev.jhat.which)) {
                if(ev.jhat.value&SDL_HAT_UP) nav(-1); else if(ev.jhat.value&SDL_HAT_DOWN) nav(1);
                if(ev.jhat.value&SDL_HAT_LEFT) moveHorizontal(-1); else if(ev.jhat.value&SDL_HAT_RIGHT) moveHorizontal(1);
            } else if(ev.type==SDL_JOYAXISMOTION && page!=5 && !ps4PadReady && !SDL_GameControllerFromInstanceID(ev.jaxis.which) && !isSdlPs4Instance(ev.jaxis.which)) {
                const int value=ev.jaxis.value;
                if(ev.jaxis.axis==0) {
                    if(value>16000 && lastAxisX<=16000) moveHorizontal(1);
                    else if(value<-16000 && lastAxisX>=-16000) moveHorizontal(-1);
                    lastAxisX=value;
                } else if(ev.jaxis.axis==1) {
                    if(value>16000 && lastAxisY<=16000) nav(1);
                    else if(value<-16000 && lastAxisY>=-16000) nav(-1);
                    lastAxisY=value;
                }
            } else if(ev.type==SDL_JOYBUTTONDOWN && !ps4PadReady && !SDL_GameControllerFromInstanceID(ev.jbutton.which)) {
                if(page==5) {
                    if(streamStartState.load()==3 && (ev.jbutton.button==0 || ev.jbutton.button==1)) leaveStream();
                    else if(hasPublishedStream() && activeStream->is_terminal() && (ev.jbutton.button==0 || ev.jbutton.button==1)) leaveStream();
                    else if(streamMenuVisible && ev.jbutton.button==0) activateStreamMenu();
                    else if(streamMenuVisible && ev.jbutton.button==1) { streamMenuVisible=false; suppressStreamInputBriefly(); statsLastSampleMs=0; }
                    else if(streamMenuVisible && ev.jbutton.button==13) nav(-1);
                    else if(streamMenuVisible && ev.jbutton.button==14) nav(1);
                }
                else if(page==7 && ev.jbutton.button==2) {
                    if(beginInputCalibration()) opennow::LogAppLifecycleEvent("GAMEPAD_CALIBRATION_ACTION","source=generic_joystick_square");
                }
                else if(page==6 && ev.jbutton.button==1) cancelPreparedLaunch();
                else if(page==3 && ev.jbutton.button==5) cycleCatalogFilter(1);
                else if(page==3 && ev.jbutton.button==4) cycleCatalogFilter(-1);
                else if(page==3 && ev.jbutton.button==3) openCatalogSearchKeyboard();
                else if(page==3 && ev.jbutton.button==2 && catalogSearchActive) eraseCatalogSearchChar();
                else if(page==3 && (ev.jbutton.button==11 || ev.jbutton.button==12)) {
                    opennow::LogAppLifecycleEvent("FAVORITE_BUTTON_INPUT",("source=joystick button="+std::to_string(ev.jbutton.button)).c_str());
                    toggleSelectedCatalogFavorite();
                }
                else if(page==7 && (ev.jbutton.button==0 || ev.jbutton.button==1)) { /* Circle requires a hold; Cross is shown in the tester. */ }
                else if(ev.jbutton.button==0) activate();
                else if(ev.jbutton.button==1) { if(page==1) { discardSettings(); page=0; selection=0; } else if(page==3) backFromCatalog(); else { if(page==4) cancelGfnLogin(); page=0; selection=0; } }
                else if(ev.jbutton.button==13) nav(-1);
                else if(ev.jbutton.button==14) nav(1);
                else if(ev.jbutton.button==15) moveHorizontal(-1);
                else if(ev.jbutton.button==16) moveHorizontal(1);
            } else if(ev.type==SDL_CONTROLLERBUTTONDOWN && !ps4PadReady) {
                if(page==7 && ev.cbutton.button==SDL_CONTROLLER_BUTTON_X) {
                    if(beginInputCalibration()) opennow::LogAppLifecycleEvent("GAMEPAD_CALIBRATION_ACTION","source=SDL_square");
                }
                else if(page==5 && streamStartState.load()==3 && (ev.cbutton.button==SDL_CONTROLLER_BUTTON_B || ev.cbutton.button==SDL_CONTROLLER_BUTTON_A)) leaveStream();
                else if(page==5 && hasPublishedStream() && activeStream->is_terminal() && (ev.cbutton.button==SDL_CONTROLLER_BUTTON_B || ev.cbutton.button==SDL_CONTROLLER_BUTTON_A)) leaveStream();
                else if(page==5 && streamMenuVisible && ev.cbutton.button==SDL_CONTROLLER_BUTTON_A) activateStreamMenu();
                else if(page==5 && streamMenuVisible && ev.cbutton.button==SDL_CONTROLLER_BUTTON_B) { streamMenuVisible=false; suppressStreamInputBriefly(); statsLastSampleMs=0; }
                else if(page==5 && streamMenuVisible && ev.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) nav(-1);
                else if(page==5 && streamMenuVisible && ev.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) nav(1);
                else if(page==5) { /* game buttons are forwarded from the polled pad state */ }
                else if(page==6 && ev.cbutton.button==SDL_CONTROLLER_BUTTON_B) cancelPreparedLaunch();
                else if(page==3 && ev.cbutton.button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) cycleCatalogFilter(1);
                else if(page==3 && ev.cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER) cycleCatalogFilter(-1);
                else if(page==3 && ev.cbutton.button==SDL_CONTROLLER_BUTTON_Y) openCatalogSearchKeyboard();
                else if(page==3 && ev.cbutton.button==SDL_CONTROLLER_BUTTON_X && catalogSearchActive) eraseCatalogSearchChar();
                else if(page==3 && (ev.cbutton.button==SDL_CONTROLLER_BUTTON_RIGHTSTICK || ev.cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSTICK)) {
                    opennow::LogAppLifecycleEvent("FAVORITE_BUTTON_INPUT",("source=controller button="+std::to_string(ev.cbutton.button)).c_str());
                    toggleSelectedCatalogFavorite();
                }
                else if(ev.cbutton.button==SDL_CONTROLLER_BUTTON_A) activate();
                else if(ev.cbutton.button==SDL_CONTROLLER_BUTTON_B) { if(page==7) { /* Hold Circle to leave the tester. */ } else if(page==1) { discardSettings(); page=0; selection=0; } else if(page==3) backFromCatalog(); else { if(page==4) cancelGfnLogin(); page=0; selection=0; } }
                else if(ev.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) nav(-1);
                else if(ev.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) nav(1);
                else if(ev.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT) moveHorizontal(-1);
                else if(ev.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT) moveHorizontal(1);
            } else if(ev.type==SDL_CONTROLLERAXISMOTION && page!=5 && !ps4PadReady) {
                const int value=ev.caxis.value;
                if(ev.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX) {
                    if(value>16000 && lastAxisX<=16000) moveHorizontal(1);
                    else if(value<-16000 && lastAxisX>=-16000) moveHorizontal(-1);
                    lastAxisX=value;
                } else if(ev.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY) {
                    if(value>16000 && lastAxisY<=16000) nav(1);
                    else if(value<-16000 && lastAxisY>=-16000) nav(-1);
                    lastAxisY=value;
                }
            } else if(ev.type==SDL_MOUSEMOTION) {
                if(page==5 && streamMenuVisible) { lastMouseX=ev.motion.x; lastMouseY=ev.motion.y; }
                else if(page==5 && hasPublishedStream() && inputDevice!=0) {
                    lastMouseX=ev.motion.x; lastMouseY=ev.motion.y;
                    if(!streamMenuVisible && !streamSuppressInputUntilNeutral && (ev.motion.xrel||ev.motion.yrel))
                        activeStream->send_mouse_move(static_cast<int16_t>(std::clamp(ev.motion.xrel,-320,320)),static_cast<int16_t>(std::clamp(ev.motion.yrel,-240,240)));
                }
                else if(page==5) { lastMouseX=ev.motion.x; lastMouseY=ev.motion.y; }
                else handleMousePosition(ev.motion.x,ev.motion.y);
            } else if(ev.type==SDL_MOUSEBUTTONDOWN) {
                if(page==5 && streamMenuVisible && ev.button.button==SDL_BUTTON_LEFT) {
                    lastMouseX=ev.button.x; lastMouseY=ev.button.y;
                    if(ev.button.x>=590 && ev.button.x<=1330 && ev.button.y>=410 && ev.button.y<714) {
                        streamMenuSelection=std::clamp((ev.button.y-410)/76,0,3); activateStreamMenu();
                    } else streamMenuVisible=false;
                }
                else if(page==5 && hasPublishedStream() && inputDevice!=0) {
                    lastMouseX=ev.button.x; lastMouseY=ev.button.y;
                    if(ev.button.button==SDL_BUTTON_LEFT && !streamMenuVisible && !streamSuppressInputUntilNeutral) {
                        activeStream->send_mouse_left_button(true);
                    }
                }
                else if(page==5) { lastMouseX=ev.button.x; lastMouseY=ev.button.y; }
                else {
                    handleMousePosition(ev.button.x,ev.button.y);
                    if(page==1 && (selection<5 || selection==7)) adjust(ev.button.x>=W/2?1:-1);
                    else if(page==3) { if(ev.button.y>=390 && ev.button.y<912 && ev.button.x<1320) activate(); }
                    else activate();
                }
            } else if(ev.type==SDL_MOUSEBUTTONUP && page==5) {
                lastMouseX=ev.button.x; lastMouseY=ev.button.y;
                if(ev.button.button==SDL_BUTTON_LEFT && hasPublishedStream() && inputDevice!=0) {
                    activeStream->send_mouse_left_button(false);
                }
            }
        }
        if(streamStartState.load(std::memory_order_acquire)==2 && !streamFirstEventPassLogged) {
            opennow::WriteStreamStartupStage("stream_ui_events_complete");
            opennow::LogAppLifecycleEvent("STREAM_UI_EVENTS_COMPLETE");
            streamFirstEventPassLogged=true;
        }
        if(streamStartState.load(std::memory_order_acquire)==2 && !streamFirstPadPollBeginLogged) {
            opennow::WriteStreamStartupStage("stream_ui_native_pad_poll_begin");
            opennow::LogAppLifecycleEvent("STREAM_UI_NATIVE_PAD_POLL_BEGIN");
            streamFirstPadPollBeginLogged=true;
        }
        if(page==6 && !launchUiPadPollLogged)
            opennow::LogAppLifecycleEvent("UI_LAUNCH_PAD_POLL_BEGIN");
        opennow::SetCurrentStage(STAGE_INPUT);
        pollNativePad();
        if(page==6 && !launchUiPadPollLogged) {
            opennow::LogAppLifecycleEvent("UI_LAUNCH_PAD_POLL_COMPLETE");
            launchUiPadPollLogged=true;
        }
        if(streamStartState.load(std::memory_order_acquire)==2 && !streamFirstPadPassLogged) {
            opennow::WriteStreamStartupStage("stream_ui_pad_poll_complete");
            opennow::LogAppLifecycleEvent("STREAM_UI_PAD_POLL_COMPLETE");
            streamFirstPadPassLogged=true;
        }
        if(page==7 && !ps4PadReady) {
            SDL_GameController* testPad=gameControllers[0]; SDL_Joystick* testJoy=genericJoysticks[0];
            const bool circle=testPad?SDL_GameControllerGetButton(testPad,SDL_CONTROLLER_BUTTON_B)!=0:(testJoy && SDL_JoystickNumButtons(testJoy)>1 && SDL_JoystickGetButton(testJoy,1));
            if(circle) {
                if(!testerCircleHeld) { testerCircleHeld=true; testerCircleSince=std::chrono::steady_clock::now(); }
                else if(std::chrono::steady_clock::now()-testerCircleSince>=std::chrono::milliseconds(900)) { page=0; selection=4; testerCircleHeld=false; }
            } else testerCircleHeld=false;
        }
        if(page==6 && launchState.load()==2 && std::chrono::steady_clock::now()>=launchReadyAt) {
            opennow::LogAppLifecycleEvent("STREAM_AUTO_CONNECT","countdown=10s");
            startPreparedStream();
        }
        if(probeThread && probeState.load()!=1) { SDL_WaitThread(probeThread,NULL); probeThread=NULL; }
        if(settingsRegionThread && settingsRegionState.load()!=1) { SDL_WaitThread(settingsRegionThread,NULL); settingsRegionThread=nullptr; }
        if(authThread && authState.load()!=1 && authState.load()!=2) { SDL_WaitThread(authThread,NULL); authThread=NULL; }
        if(launchThread && launchState.load()!=1) { SDL_WaitThread(launchThread,NULL); launchThread=NULL; }
        maintainCatalogCover();
        if(streamStartState.load(std::memory_order_acquire)==2 && !streamCoverPassLogged) {
            opennow::WriteStreamStartupStage("stream_ui_maintenance_complete");
            opennow::LogAppLifecycleEvent("STREAM_UI_MAINTENANCE_COMPLETE");
            streamCoverPassLogged=true;
        }
        // Signaling, heartbeat and recovery polling run on the WebRTC network
        // worker so the UI loop never blocks on peer_mutex_.
        pollPs4Keyboard();
        if(page==5 && hasPublishedStream()) sendGamepadState();
        if(page==6 && !launchUiDrawBeginLogged) {
            opennow::LogAppLifecycleEvent("UI_LAUNCH_DRAW_BEGIN");
            launchUiDrawBeginLogged=true;
        }
        opennow::SetCurrentStage(STAGE_DRAW);
        // TELEMETRIA DE MEMORIA cada 30 s.
        //
        // POR QUE: la sesion de la 2.95 se cerro a los 27 minutos sin dejar rastro en el manejador de
        // senales (no hubo CRASH_ENTER). Eso apunta a una terminacion EXTERNA por parte del sistema,
        // y la causa mas probable en una sesion larga es presion de memoria. Sin medir la memoria
        // libre no se puede ni confirmar ni descartar.
        //
        // Lo que se busca: si la memoria libre BAJA de forma sostenida minuto a minuto, hay fuga y es
        // la causa. Si se mantiene estable, la causa es otra y habria que mirar los limites de GoldHEN.
        {
            static uint64_t s_lastMemLogMs=0;
            const uint64_t memNowMs=SDL_GetTicks();
            if(memNowMs-s_lastMemLogMs>=30000) {
                s_lastMemLogMs=memNowMs;
                uint64_t freeBytes=0, totalBytes=0;
                bool haveReading=false;
#if defined(__ORBIS__)
                // sceKernelGetDirectMemorySize da el TOTAL de memoria directa disponible para el
                // proceso; la LIBRE se consulta aparte, mas abajo, con
                // `sceKernelAvailableDirectMemorySize`. Sin la libre esta telemetria no puede
                // detectar una fuga: el total no cambia nunca (siempre 4608 MB en este proyecto).
                totalBytes = sceKernelGetDirectMemorySize();
                haveReading = (totalBytes > 0);
                // =============================================================================
                // MEMORIA LIBRE REAL (v3.93). ESTA ERA LA MEDIDA QUE FALTABA.
                // =============================================================================
                // `direct_total_mb` siempre dio 4608 y **no cambia nunca**, asi que no puede
                // detectar una fuga. Y una fuga es la hipotesis principal del cierre sin traza:
                // `PS4-V2.81-CRASH-HEARTBEAT.md` lo documenta con el mecanismo completo --
                // *"el proceso murio de una forma que no pasa por los manejadores de senal... el
                // kernel lo mata (memoria agotada, watchdog del sistema)"* -- y anade que el sintoma
                // es **degradacion progresiva antes de morir** (`PRESENT_US` multiplicandose por 9
                // en los ultimos 2,5 s).
                //
                // La v3.90 murio en `stage=6 name=PRESENT` tras 300 s, que es el mismo patron.
                //
                // FIRMA (verificada en las referencias del proyecto, `libraries/kernel/memory.cpp`):
                //     s32 sceKernelAvailableDirectMemorySize(u64 searchStart, u64 searchEnd,
                //                                            u64 alignment,
                //                                            u64* physAddrOut, u64* sizeOut);
                // El stub de OpenOrbis la declara con tipos **por valor**
                // (`int32_t(off_t, off_t, size_t, off_t, size_t)`), asi que no se puede llamar
                // directamente con punteros. Se usa la firma real mediante un cast explicito del
                // puntero a funcion: el simbolo y la convencion de llamada son los mismos, solo
                // cambian los tipos declarados.
                {
                    using AvailDmemFn = int32_t (*)(uint64_t, uint64_t, uint64_t,
                                                    uint64_t*, uint64_t*);
                    const AvailDmemFn availFn = reinterpret_cast<AvailDmemFn>(
                        &sceKernelAvailableDirectMemorySize);
                    uint64_t availPhys = 0, availSize = 0;
                    const int32_t availRc = availFn(0, ORBIS_KERNEL_MAIN_DMEM_SIZE, 1024ull,
                                                    &availPhys, &availSize);
                    if(availRc == 0) {
                        freeBytes = availSize;
                        // Guardia: si la memoria directa libre cae por debajo de 64 MB durante el
                        // stream, se registra como aviso. NO se corta la sesion por esto (cortarla
                        // seria peor que el problema), pero queda en el log con el numero exacto.
                        if(page == 5 && hasPublishedStream() && availSize < (64ull * 1024ull * 1024ull)) {
                            static uint64_t s_lastLowMemWarnMs = 0;
                            if(memNowMs - s_lastLowMemWarnMs >= 30000) {
                                s_lastLowMemWarnMs = memNowMs;
                                char lowDetail[160];
                                std::snprintf(lowDetail, sizeof(lowDetail),
                                              "direct_free_mb=%llu umbral_mb=64 page=%d accion=ninguna",
                                              static_cast<unsigned long long>(availSize / (1024ull*1024ull)),
                                              page);
                                opennow::LogAppLifecycleEvent("MEMORY_LOW_WARNING", lowDetail);
                            }
                        }
                    }
                }
#endif
                char memDetail[200];
                std::snprintf(memDetail,sizeof(memDetail),
                              "uptime_ms=%llu direct_total_mb=%llu direct_free_mb=%llu have_reading=%d",
                              static_cast<unsigned long long>(memNowMs),
                              static_cast<unsigned long long>(totalBytes/(1024ull*1024ull)),
                              static_cast<unsigned long long>(freeBytes/(1024ull*1024ull)),
                              haveReading?1:0);
                opennow::LogAppLifecycleEvent("MEMORY_WATCH",memDetail);
            }
        }
        // VOLCADO DEL REGISTRO DE SESION cada 30 s.
        //
        // POR QUE PERIODICO Y NO SOLO AL SALIR: la app se ha cerrado de golpe en varias sesiones sin
        // pasar por el manejador de senales, asi que un volcado al final perderia justo las sesiones
        // que interesa analizar. Volcando cada 30 s siempre hay datos en disco.
        {
            static uint64_t s_lastRecDump=0;
            const uint64_t recNow=SDL_GetTicks();
            if(recNow-s_lastRecDump>=30000) {
                s_lastRecDump=recNow;
                opennow::diag::DumpSessionRecorder("/data/gfnps4/session_frames.csv");
                opennow::diag::DumpSessionSummary("/data/gfnps4/session_summary.txt");
            }
        }
        // Telemetria de arranque del bucle: registra el primer frame y despues uno cada 5 s.
        // Sirve para distinguir "la app no llega a dibujar" de "dibuja pero se ve negro".
        {
            static uint64_t s_drawCount=0;
            static uint64_t s_lastDrawLogMs=0;
            ++s_drawCount;
            const uint64_t drawNowMs=SDL_GetTicks();
            if(s_drawCount==1 || drawNowMs-s_lastDrawLogMs>=5000) {
                s_lastDrawLogMs=drawNowMs;
                char drawDetail[128];
                std::snprintf(drawDetail,sizeof(drawDetail),
                              "frames=%llu page=%d renderer=%d sdl=%d direct=%d",
                              static_cast<unsigned long long>(s_drawCount), page,
                              renderer?1:0, SDL_WasInit(SDL_INIT_VIDEO)?1:0,
                              videoOutDirectActive?1:0);
                opennow::LogAppLifecycleEvent("UI_DRAW_TICK",drawDetail);
            }
        }
        // ------------------------------------------------------------------
        // FIN DE SESION DETECTADO AUTOMATICAMENTE.
        //
        // BUG QUE ESTO CORRIGE (medido en una sesion de 2 h 7 min):
        // `activeStream->is_terminal()` SOLO se comprobaba dentro de los manejadores de botones. Si
        // la sesion de GeForce NOW terminaba sola (limite de ~2 h), la app se quedaba en la pagina
        // del stream CON EL PEER YA CERRADO: el log mostraba
        //     STREAM diag reason=peer_state_terminal state=Peer closed
        // seguido de 52.243 lineas "PAD blocked ... peer=closed" mientras la UI seguia a 62 FPS.
        // No era un cuelgue ni una fuga de memoria (direct_total_mb=4608 constante durante 12,9 h de
        // uptime): era que NADIE salia del stream.
        //
        // La comprobacion va en el bucle principal, una vez por frame, para que la salida ocurra
        // aunque el usuario no toque el mando.
        {
            static int s_terminalSeenFrames=0;
            if(page==5 && hasPublishedStream() && activeStream->is_terminal()) {
                ++s_terminalSeenFrames;
                if(s_terminalSeenFrames==1) {
                    char detail[192];
                    std::snprintf(detail,sizeof(detail),
                                  "source=main_loop peer_terminal=1 start_state=%d "
                                  "action=return_to_menu_and_close_session",
                                  streamStartState.load());
                    opennow::LogAppLifecycleEvent("STREAM_SESSION_ENDED",detail);
                    // close_cloud_session=true: libera la plaza en el servicio y evita dejar una
                    // sesion colgada en la cuenta.
                    leaveStream(true);
                }
            } else {
                s_terminalSeenFrames=0;
            }
        }

        // =============================================================================================
        // MEDICION DEL DIBUJADO COMPLETO (v3.86): CIERRA LA CONTRADICCION
        // =============================================================================================
        // En la v3.85 se midio, con el video a pantalla completa en `page=5`:
        //
        //     UI_LOOP_BUDGET  iter_ms=50  draw_ms=50  sleep_ms=0  frames=20
        //     UI_DRAW_PHASES  present_us=49869  frames=20        -> 2,49 ms por frame
        //     STREAM_VIDEO_DRAW_US  video_draw_us=32449 frames=20 -> 1,62 ms por frame
        //     STREAM_VIDEO_PHASES   copy_us=47993  frames=16      -> 3,00 ms por copia
        //
        // **`draw_ms` = 50 ms, pero las fases internas suman ~4 ms.** Uno de los dos no esta midiendo
        // lo que se cree. Estas dos marcas lo resuelven sin ambiguedad:
        //
        //     draw_total_us : `draw()` medido DESDE EL BUCLE (fuera de la funcion)
        //     draw_phase_us : la suma de las fases medidas DENTRO de `draw()`
        //
        // Si `draw_total_us` >> `draw_phase_us`, dentro de `draw()` hay trabajo **sin instrumentar**.
        // Si son parecidos, entonces el tiempo no esta en `draw()` y `renderCostMs` mide otra cosa.
        //
        // Se registra una linea por segundo, junto a `UI_LOOP_BUDGET`, para poder compararlas.
        const uint64_t t_draw_loop0 = getProcessTimeUs();
        draw();
        {
            const uint64_t t_draw_loop1 = getProcessTimeUs();
            static uint64_t s_drSum=0, s_drFrames=0, s_drLastLog=0;
            s_drSum += (t_draw_loop1 > t_draw_loop0) ? (t_draw_loop1 - t_draw_loop0) : 0;
            ++s_drFrames;
            if(s_drLastLog == 0 || (t_draw_loop1 - s_drLastLog) >= 1000000ULL) {
                s_drLastLog = t_draw_loop1;
                const uint64_t n = s_drFrames ? s_drFrames : 1;
                char dl[176];
                std::snprintf(dl,sizeof(dl),
                              "page=%d stream=%d draw_total_us=%llu frames=%llu",
                              page, hasPublishedStream()?1:0,
                              (unsigned long long)(s_drSum/n), (unsigned long long)n);
                opennow::LogAppLifecycleEvent("UI_DRAW_TOTAL", dl);
                s_drSum=0; s_drFrames=0;
            }
        }
        if(page==6 && !launchUiDrawDoneLogged) {
            opennow::LogAppLifecycleEvent("UI_LAUNCH_DRAW_COMPLETE");
            launchUiDrawDoneLogged=true;
        }
        const Uint32 renderCostMs=SDL_GetTicks()-frameStartMs;
        perfMaxRenderMs=std::max(perfMaxRenderMs,renderCostMs);
        if(renderCostMs>=50) ++perfSlowFrames;
        // =============================================================================================
        // MEDICION DEL BUCLE COMPLETO (v3.85): RESUELVE UNA CONTRADICCION REAL
        // =============================================================================================
        // En el log de la v3.83, con el video a pantalla completa, aparecen DOS medidas que no pueden
        // ser ciertas a la vez:
        //
        //     UI_LOOP_PERF    loop_fps=19  render_max_ms=51  slow_frames=95    -> 50 ms por vuelta
        //     UI_DRAW_PHASES  present_us=49873  frames=20                       -> 2,49 ms por frame
        //
        // **El bucle dice que el dibujado mide 51 ms; las fases internas suman 2,5 ms.** Si el dibujado
        // costara 2,5 ms, el bucle iria a 60 fps (esta limitado a 16 ms por el `SDL_Delay` de abajo).
        //
        // Las fases del camino de stream miden `drawT0 -> present`, asi que **la diferencia tiene que
        // estar FUERA de `draw()`**: en lo que el bucle hace antes o despues. Estas tres cifras lo
        // separan sin ambiguedad:
        //
        //     iter_ms  : la vuelta COMPLETA del bucle (lo que define los fps)
        //     draw_ms  : lo que se atribuye al dibujado (`frameStartMs -> aqui`)
        //     sleep_ms : lo que el `SDL_Delay` de abajo ha esperado de verdad
        //
        // Con `iter_ms`, `draw_ms` y `sleep_ms` se sabe si el tiempo se va dibujando, esperando o en
        // otra parte del bucle. Se registra una linea por segundo.
        static uint32_t s_lastIterStartMs = 0;
        static uint64_t s_iterSumMs = 0, s_drawSumMs = 0, s_sleepSumMs = 0, s_iterFrames = 0;
        static uint32_t s_iterLastLogMs = 0;
        {
            const uint32_t nowMs = SDL_GetTicks();
            if(s_lastIterStartMs != 0) s_iterSumMs += (nowMs - s_lastIterStartMs);
            s_lastIterStartMs = nowMs;
            s_drawSumMs += renderCostMs;
            ++s_iterFrames;
        }
        // Pace against a 60 Hz budget instead of adding a fixed 16 ms after
        // rendering. A fixed sleep capped the software-rendered UI near 30 FPS
        // whenever drawing itself took another ~16 ms.
        const Uint32 frameElapsedMs=SDL_GetTicks()-frameStartMs;
        // Single VSYNC governor. When Piglet/EGL (eglSwapInterval) or the direct
        // VideoOut path (sceVideoOutSubmitFlip with FLIP_VSYNC) already paces the
        // display, applying this 16 ms software budget as well creates a second,
        // unaligned governor. The two drift against the 60 Hz DCE vblank, so the
        // flip is refused or the scanned-out buffer goes stale: black screen.
        // The software SDL fallback keeps exactly its previous pacing, which is
        // still needed because drawing itself can cost ~16 ms.
        const bool hardwarePaced=pigletVideoActive||(g_videoOutHandoffDone&&videoOutDirectActive);
        if(!hardwarePaced && frameElapsedMs<16) {
            const Uint32 sleepMs = 16-frameElapsedMs;
            s_sleepSumMs += sleepMs;
            SDL_Delay(sleepMs);
        }
        {
            const uint32_t nowMs2 = SDL_GetTicks();
            if(s_iterLastLogMs == 0 || (nowMs2 - s_iterLastLogMs) >= 1000u) {
                s_iterLastLogMs = nowMs2;
                const uint64_t n = s_iterFrames ? s_iterFrames : 1;
                char lp[200];
                std::snprintf(lp,sizeof(lp),
                              "page=%d stream=%d iter_ms=%llu draw_ms=%llu sleep_ms=%llu "
                              "frames=%llu render_max_ms=%u",
                              page, hasPublishedStream()?1:0,
                              (unsigned long long)(s_iterSumMs/n),
                              (unsigned long long)(s_drawSumMs/n),
                              (unsigned long long)(s_sleepSumMs/n),
                              (unsigned long long)n, (unsigned)perfMaxRenderMs);
                opennow::LogAppLifecycleEvent("UI_LOOP_BUDGET", lp);
                // Resumen por segundo a la TRAZA DETALLADA (v4.25). Es lo que permite cruzar "el bucle
                // va a X fps" con "Present() registro Z frames" en la misma linea, sin deducirlo.
                {
                    const uint64_t iterMedio = (s_iterSumMs / n) ? (s_iterSumMs / n) : 1;
                    const int loopFpsSeg = static_cast<int>(1000ull / iterMedio);
                    opennow::trace::StreamSecond(
                        loopFpsSeg,
                        static_cast<int>(s_drawSumMs / n),
                        static_cast<int>(iterMedio),
                        n, g_traceDecodeQueue, g_traceDecodeFps, static_cast<int>(opennow::trace::FrameRowsRecorded()));
                }
                // =========================================================================================
                // RED DE SEGURIDAD: SI LA RUTA DIRECTA DEJA DE PRESENTAR, SE DEGRADA A SDL SOLA (v4.26)
                // =========================================================================================
                // POR QUE EXISTE, y por que aqui y no dentro del bloque del stream:
                //
                //   En la v4.22 la ruta directa se quedo **con la imagen congelada** y la aplicacion
                //   siguio viva (audio, mando, decodificador). El usuario veia una foto fija y no habia
                //   ninguna proteccion: **el sintoma duraba hasta que cerraba el juego**.
                //
                //   Y esta comprobacion va EN ESTE BLOQUE, que se ejecuta una vez por segundo, **no dentro
                //   del bloque del stream**. Eso es deliberado: mis DOS intentos anteriores de reorganizar
                //   el bloque del stream son los que causaron el cierre de la v4.20 (dibujar con
                //   `renderer=NULL`) y la congelacion de la v4.22 (el callback encerrado en una guarda).
                //   **No vuelvo a tocar su estructura.** Desde aqui se observa y se pide la degradacion,
                //   que es exactamente lo que ya sabe hacer su propia cuarentena.
                //
                // CRITERIO, y los tres terminos importan:
                //   - la ruta directa esta activa y el traspaso se hizo
                //   - **se han presentado CERO frames en 3 segundos** (no es un frame lento: es ninguno)
                //   - **el decodificador SI esta produciendo** (si no produce, el problema es otro y
                //     degradar no arreglaria nada)
                // 3 segundos y no 1 porque un tiron puntual no debe tumbar la ruta que da 60 fps.
                if (videoOutDirectActive && g_videoOutHandoffDone && g_traceDecodeFps > 0) {
                    static uint32_t s_lastPresentedFramesSeen = 0;
                    static Uint32 s_lastPresentAdvanceMs = 0;
                    const uint32_t presentadosAhora = opennow::PS4VideoOutRenderer::GetPresentedFrames();
                    const Uint32 ahoraMs = SDL_GetTicks();
                    if (presentadosAhora != s_lastPresentedFramesSeen) {
                        // Avanza: la presentacion funciona. Se reinicia el reloj de vigilancia.
                        s_lastPresentedFramesSeen = presentadosAhora;
                        s_lastPresentAdvanceMs = ahoraMs;
                    } else if (s_lastPresentAdvanceMs == 0) {
                        s_lastPresentAdvanceMs = ahoraMs;
                    } else if ((ahoraMs - s_lastPresentAdvanceMs) > 3000u) {
                        char wd[224];
                        std::snprintf(wd, sizeof(wd),
                                      "presentados=%u sin_avanzar_ms=%u dec_fps=%d q=%d "
                                      "accion=siguiente_stream_en_SDL",
                                      presentadosAhora,
                                      static_cast<unsigned>(ahoraMs - s_lastPresentAdvanceMs),
                                      g_traceDecodeFps, g_traceDecodeQueue);
                        opennow::LogAppLifecycleEvent("VIDEOOUT_PRESENT_WATCHDOG", wd);
                        opennow::trace::StreamEvent("VIGILANTE_PRESENTACION_SIN_AVANCE", wd);
                        // Se reutiliza la cuarentena que ya existe: el siguiente stream arranca en SDL.
                        // **No se toca la estructura del bloque del stream.**
                        // LLAMA A LA RECUPERACION COMPLETA, no solo a la marca (v4.31).
                        //
                        // Antes solo ponia `g_videoOutQuarantined = true`, y esa marca **solo se evalua al
                        // arrancar el SIGUIENTE stream** (`videoOutDirectActive = g_videoOutDebugFlagDetected
                        // && !g_videoOutQuarantined`). Es decir: **la sesion en curso se quedaba con la
                        // imagen congelada hasta salir del juego**, que es justo lo que el vigilante venia a
                        // evitar. Ahora se recupera en el acto: se apaga VideoOut y se restaura SDL.
                        recoverFromDirectPathFailure("reason=presentacion_sin_avance_3s", /*desdeElCallback=*/false);
                        s_lastPresentAdvanceMs = ahoraMs;
                    }
                }
                s_iterSumMs=0; s_drawSumMs=0; s_sleepSumMs=0; s_iterFrames=0;
            }
        }
        ++perfSampleFrames;
        const Uint32 sampleElapsed=SDL_GetTicks()-perfSampleStart;
        if(sampleElapsed>=5000) {
            const int loopFps=static_cast<int>((static_cast<uint64_t>(perfSampleFrames)*1000u)/sampleElapsed);
            appLoopFps=loopFps;
            const std::string detail="loop_fps="+std::to_string(loopFps)+
                " render_max_ms="+std::to_string(perfMaxRenderMs)+
                " slow_frames="+std::to_string(perfSlowFrames)+
                " page="+std::to_string(page)+
                " piglet="+(pigletVideoActive?"1":"0")+
                " stream="+(hasPublishedStream()?"1":"0");
            opennow::LogAppLifecycleEvent("UI_LOOP_PERF",detail.c_str());
            perfSampleStart=SDL_GetTicks(); perfSampleFrames=0; perfMaxRenderMs=0; perfSlowFrames=0;
        }
        if(static_cast<Uint32>(SDL_GetTicks()-lastActionTraceFlushAt)>=250u) {
            lastActionTraceFlushAt=SDL_GetTicks();
            opennow::FlushAppActionTrace();
        }
      } catch(const std::exception& error) {
        char message[512];
        snprintf(message,sizeof(message),"Error de interfaz durante la sesion: %.450s",error.what());
        try { opennow::LogAppLifecycleEvent("UI_FRAME_EXCEPTION",error.what()); } catch(...) {}
        try { opennow::TraceAppAction("UI_EXCEPTION","frame_error; detail omitted from action trace"); } catch(...) {}
        if(streamStartThread) {
            SDL_WaitThread(streamStartThread,NULL);
            streamStartThread=NULL;
        }
        try { if(activeStream) activeStream->stop(); } catch(...) {}
        activeStream.reset();
        pendingStream.reset();
        SDL_LockMutex(launchMutex);
        snprintf(launchError,sizeof(launchError),"%.500s",message);
        SDL_UnlockMutex(launchMutex);
        streamStartState.store(3,std::memory_order_release);
        streamMenuVisible=false;
        streamStatsVisible=false;
        page=5;
        selection=0;
      } catch(...) {
        try { opennow::LogAppLifecycleEvent("UI_FRAME_EXCEPTION","unknown C++ exception"); } catch(...) {}
        if(streamStartThread) {
            SDL_WaitThread(streamStartThread,NULL);
            streamStartThread=NULL;
        }
        try { if(activeStream) activeStream->stop(); } catch(...) {}
        activeStream.reset();
        pendingStream.reset();
        SDL_LockMutex(launchMutex);
        snprintf(launchError,sizeof(launchError),"Ocurrio un error inesperado en la interfaz durante el inicio del stream.");
        SDL_UnlockMutex(launchMutex);
        streamStartState.store(3,std::memory_order_release);
        streamMenuVisible=false;
        streamStatsVisible=false;
        page=5;
        selection=0;
      }
    }
    if(probeThread) { SDL_WaitThread(probeThread,NULL); probeThread=NULL; }
    if(settingsRegionThread) { SDL_WaitThread(settingsRegionThread,NULL); settingsRegionThread=nullptr; }
    if(catalogThread) { SDL_WaitThread(catalogThread,NULL); catalogThread=NULL; }
    cancelGfnLogin();
    if(authThread) { SDL_WaitThread(authThread,NULL); authThread=NULL; }
    launchCancelled.store(true);
    if(launchThread) { SDL_WaitThread(launchThread,NULL); launchThread=NULL; }
    joinAndAdoptStreamWorker(true);
    for(int flush=0;flush<8;++flush) opennow::FlushAppActionTrace();
    if(catalogCoverThread) { SDL_WaitThread(catalogCoverThread,NULL); catalogCoverThread=NULL; }
    if(hasPublishedStream()) leaveStream(true);
    else if(g_activeRecoverableSession || !activeSessionId.empty()) {
        leaveStream(true);
    }
    if(stopThread) { SDL_WaitThread(stopThread,NULL); stopThread=NULL; }
    // Los QR de "ACERCA DE": una textura de SDL no se libera sola. Se sueltan antes de destruir el
    // renderizador, porque destruir una textura despues del renderizador al que pertenece es
    // comportamiento indefinido.
    freeQrTextures();
    for(auto& item:textCache) if(item.second) SDL_DestroyTexture(item.second);
    textCache.clear();
    if(ps4KeyboardHandle>=0) { sceKeyboardClose(ps4KeyboardHandle); ps4KeyboardHandle=-1; }
    if(ps4PadHandle>=0) { scePadClose(ps4PadHandle); ps4PadHandle=-1; }
    if(ps4MouseHandle>=0) { sceMouseClose(ps4MouseHandle); ps4MouseHandle=-1; ps4MouseReady=false; }
    SDL_DestroyMutex(authMutex); authMutex=NULL;
    SDL_DestroyMutex(launchMutex); launchMutex=NULL;
    SDL_DestroyMutex(networkMutex); networkMutex=NULL;
    // Release the persistent Net/SSL/HTTP contexts before bringing the network
    // stack down. Previously they were leaked on every exit, which left sockets
    // and TLS state open when the system killed the process.
    opennow::ShutdownHttpClient();
    // Tear down the graphics owner before sceNetTerm(): VideoOut/Piglet cleanup
    // must not race a half-closed network stack.
    if(pigletVideoActive) PS4PigletVideoRenderer::Shutdown();
    opennow::PS4VideoOutRenderer::Shutdown();
    if(renderer) { SDL_DestroyRenderer(renderer); renderer=nullptr; }
    if(pigletOverlaySurface) { SDL_FreeSurface(pigletOverlaySurface); pigletOverlaySurface=nullptr; }
    if(window) { SDL_DestroyWindow(window); window=nullptr; }
    sceNetTerm();
    SDL_Quit(); return 0;
}
