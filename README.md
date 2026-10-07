# AJ GeForce NOW para PS4

Cliente **GeForce NOW** nativo para **PlayStation 4** (homebrew, firmware 9.00, GoldHEN).
No es un navegador ni un envoltorio web: implementa el protocolo completo del servicio.

**Versión: 4.32** · PKG incluido en [`build/`](build/)

---

## Qué es

Un cliente que corre **en la propia consola** y hace todo el recorrido del servicio:

```
Login (OAuth por QR)  →  Catálogo  →  Cola de sesión (CloudMatch)
    →  Señalización WebSocket + SDP  →  WebRTC (libpeer)
    →  Stream de vídeo H.264  →  Decodificación FFmpeg  →  Presentación por libSceVideoOut
```

**El vídeo no se reescala en la CPU.** El frame llega a **720p nativo** y se presenta **1:1** en un
framebuffer de **1280x720**; el estirón al panel de 1080p lo hace **el hardware de `libSceVideoOut`**.
Ese detalle es el que separa los **20 fps** de los **60 fps** (ver *Rendimiento*, más abajo).

---

## Estado real

Medido en consola, no estimado.

### Funciona

| Función | Evidencia medida |
|---|---|
| Instalación, login por QR, catálogo, biblioteca, cola | `pkg_validate` **28/28** |
| **Stream de vídeo a 720p nativo** | **3543 frames presentados en 57,0 s = 62,2 fps** |
| **Presentación directa 1:1** | `1280x720 → 1280x720` en las 3543 filas; **`scaled=0`**, **`reused=0`** |
| **Presupuesto de frame** | **`pres_us` mediana 1221 µs sobre 16666** (7 %); solo **12 filas** con `over=1` |
| Arranque | **`APP_READY` a los 7 ms** |
| Audio Opus, mando a 125 Hz | `drops=0` |
| Red | `gaps=0`, `rtp_drops=0` |
| Memoria | 4608 MB constantes durante 12,9 h (sin fuga) |
| Colores | Lienzo `BGR888` + rotación R↔B en la copia (verificado con 6 pruebas) |

### Lo que falla, y conviene saberlo antes de instalarlo

**1. El decodificador por hardware de PS4 no está disponible.**
`libSceVideodec2` no carga en este firmware (`rc=0x805A1000`) y la librería de arbitración no tiene
firmas verificadas. **La decodificación es por software**, y va sobrada: **108-118 µs por lote**.
No es un cuello de botella.

**2. El servidor puede bajar la resolución a 960x540 aunque se pidan 720p.**
Se ha medido (`tex=1280x720 x15 → tex=960x540 x205`) incluso **con el viewport declarado a 1080p**, que
es la palanca que en la v3.15 lo evitaba. **No se puede corregir del todo desde el cliente.**
Y hay un círculo vicioso: al bajar la resolución el coste del cliente **no baja** si se está escalando
a 1080p, así que el servidor no ve mejoría y no la recupera. **La ruta directa rompe ese círculo**
porque su framebuffer se adapta al stream.

**3. La ruta SDL (respaldo) no puede llegar a 60 fps, y no es optimizable.**
El driver de SDL-PS4 fija el lienzo **al tamaño del display (1920x1080)** y copia a VideoOut con un
`memcpy` crudo sin escalar. Por tanto **un stream de 720p se escala a 1080p en la CPU**: ~31.000 µs
sobre un presupuesto de 16.666. **Techo estructural de ~20 fps.**

**4. Cierres esporádicos sin causa identificada.**
Las sesiones han terminado a los 31, 42, 178, 214, 233, 269, 325, 497, 515, 603, 1037 y 3570 segundos:
**no hay un umbral fijo**, así que no es un watchdog del sistema ni un límite de tiempo. Están
descartados con medidas la memoria (plana durante 240 s), la red, el decodificador y la cola.
Dos causas **sí** se identificaron y corrigieron: dibujar con `renderer = NULL` (v4.20) y el callback
de presentación quedando fuera de alcance (v4.24).

### Lo que no se pudo implementar

| Función | Por qué |
|---|---|
| **Decodificación por hardware** (`libSceVideodec2`) | el módulo no carga en firmware 9.00 (`0x805A1000`); la ABI v1 responde `0x80C10001` en `query_resource_info` |
| **1080p** | el único intento cerró la app: **1 flip con 1080p frente a 119 con 720p**. Además, sobre un panel 1080p un framebuffer de 720p **se estira y llena la pantalla**, así que 1080p solo añadiría 4× más píxeles que escribir |
| **Garantizar que el servidor no baje a 540p** | se probaron viewport 1080p en el SDP, REMB a 50 Mbps, calidad `Original`, `scalingFeature1` y prefiltro del servidor: **ninguno lo elimina del todo** |
| **Escalador de hardware de VideoOut** | `sceVideoOutSubmitFlip` no acepta un rectángulo de origen: **no se le puede pedir que escale a un tamaño concreto**, solo se aprovecha el estirado del framebuffer al panel |
| **Vídeo a 60 fps garantizados** | depende de que la ruta directa no se caiga; hay un vigilante que **degrada a SDL sola** si deja de presentar, y eso baja a ~20 fps en vez de congelar la imagen |

---

## Rendimiento: por qué 720p y por qué directo

| | **Ruta SDL** | **Ruta directa** |
|---|---|---|
| Destino | 1920x1080 (el display) | **1280x720** (el stream) |
| Escalado de un stream 720p | **en la CPU**, ~31.000 µs | **ninguno** (conversión 1:1) |
| Quién estira al panel | la CPU | **libSceVideoOut**, en hardware |
| **Fps medidos** | ~20 | **62,2** |

**La misma imagen se puede producir escalando en la CPU (20 fps) o en el hardware de VideoOut (60 fps).**

**Medición real de una sesión (de `trace_stream.csv`):**

```
3543 frames en 57,0 s          = 62,2 fps
over=1  (pasa de 16.666 µs)    = 12 de 3543   (0,3 %)
scaled=1 (escalado en CPU)     = 0
reused=1 (cola vacía)          = 0
pres_us: min 1159 · mediana 1221 · max 16987
```

El decodificador produce **~64 fps** y el servidor envía **~65 unidades/s**: se presenta el **96 %** de
lo que llega. **No hay cuello de botella en ningún sitio.**

---

## Requisitos

| Elemento | Requisito |
|---|---|
| Consola | PS4 Pro validada; Fat y Slim sin probar |
| Firmware | **9.00** |
| Jailbreak | **GoldHEN** |
| Cuenta | GeForce NOW (Free, Priority o Ultimate) |
| Red | Cable recomendado |
| Mando | DualShock 4; teclado y ratón USB opcionales |

No requiere PSN.

---

## Instalación

1. Copia `build/IV0000-GFNP00001_00-GFNPS4CLIENT0001.pkg` a un USB (exFAT o FAT32).
2. En la PS4: **GoldHEN → Package Installer**.
3. Instala. Aparecerá **AJ GeForce NOW** en el menú.

> **Title ID: `GFNP00001`.** Es el único que la consola acepta tras muchas pruebas.

---

## Uso

1. Abre la app y acepta el aviso beta.
2. **Iniciar sesión** → aparece un **código QR**. Escanéalo y autoriza en tu cuenta de NVIDIA.
   Solo hay que hacerlo una vez.
3. Navega el catálogo, elige un juego y espera la cola.
4. En la partida: mando de PS4 a 125 Hz. El menú de la app se abre con PS/Options.

| Botón | Acción |
|---|---|
| X | Confirmar |
| Círculo | Volver |
| Arriba/Abajo | Navegar |
| Izquierda/Derecha | Cambiar el valor del ajuste |

---

## Configuración (13 filas)

| # | Ajuste | Notas |
|---|---|---|
| 0 | Idioma | |
| 1 | Dispositivo de entrada | Auto / Mando / Teclado |
| 2 | **Resolución (solo 720P)** | La única que controla framebuffer y petición |
| 3 | FPS de transmisión | 30 / 60 |
| 4 | Límite de bitrate | 25 / 30 / 35 Mbps |
| 5 | Modo de calidad | Original / Clarity / Adaptive |
| 6 | Buffer de audio | 20-80 ms |
| 7 | Idioma del juego | 8 idiomas |
| 8 | Decodificador | **Informativo**: `NO DISPONIBLE (SW)` |
| 9 | Resolución de vídeo | **Informativo**: `720P (FIJO, 1080P DESCARTADO)` |
| 10 | Realce de nitidez | Cuesta ×3,9 en CPU; desactivado por defecto |
| 11 | Región | |
| 12 | Guardar y volver | |

**¿Por qué 1080p está bloqueado?** El único intento de sesión que cerró la aplicación tenía framebuffer
1920x1080: **1 flip presentado con 1080p frente a 119 con 720p**. Desde la v4.22 hay un **techo explícito**
en `Initialize()` que hace imposible registrar un framebuffer de 1080p.

---

## Diagnóstico

Todo se escribe en `/data/gfnps4/`.

### Traza detallada (v4.25)

**Una fila por frame y un hito por paso**, para responder "¿dónde se va el presupuesto de 16.666 µs?":

| Fichero | Contenido |
|---|---|
| `trace_boot.txt` | **Un hito por cada paso del arranque**, con su tiempo. `APP_READY` es el tiempo real hasta tener bucle |
| `trace_stream.txt` | Hitos de la sesión + resumen por segundo (`loop_fps`, `draw_ms`, `q`, `dec_fps`, `presentados`) |
| `trace_stream.csv` | **UNA FILA POR FRAME PRESENTADO** |

**Columnas del CSV y qué responde cada una:**

| Columna | Qué dice |
|---|---|
| `t_ms`, `frame`, `gen` | cuándo, qué frame, y su generación |
| **`reused`** | **1 = la cola estaba vacía** (no es problema de presentación) |
| `src_w/h`, `dst_w/h` | lo que entregó el servidor y el tamaño real del framebuffer |
| **`scaled`** | **1 = escalado en CPU** (el enemigo de los 60 fps) |
| `q`, `dec_fps` | profundidad de cola del decodificador y su caudal |
| **`pres_us`** | **lo que costó presentar** |
| `budget_us`, **`over`** | 16666, y **1 si se pasó** |

**Con eso, "por qué no van a 60" se lee directamente:** se cuentan las filas con `over=1`, y se mira si
`scaled=1` o `pres_us` lo explican.

### Log unificado

| Fichero | Contenido |
|---|---|
| `diagnostic.log` | Log completo de la sesión |
| `last_stage.txt` | Etapa en la que estaba la app en el último latido (1/s) |
| `session_frames.csv` | Anillo de los últimos 4096 frames presentados |
| `session_summary.txt` | Resumen: frames, cambios de resolución, tamaños de paquete |
| `settings.cfg` | Configuración (14 campos) |

**Eventos clave:**

```
APP_START version=4.32
VIDEOOUT_HANDOFF_COMPLETE mode=direct_hardware_60fps
VIDEOOUT_PRESENT_PATH path=direct_1to1 resolution=1280x720
VIDEOOUT_SIZE_CAPPED_720P ...                    <- el techo actuando
VIDEOOUT_PRESENT_GATE presentar=N cola_vacia=N   <- la puerta de presentacion
VIDEOOUT_PRESENT_WATCHDOG ...                    <- el vigilante: degrado a SDL
VIDEOOUT_RECOVERY_FROM_MAIN_LOOP ...             <- recuperacion desde el bucle
```

---

## Compilar

**Requisitos del entorno de compilación** (no incluidos en el repositorio, se descargan aparte):

| Herramienta | Uso |
|---|---|
| **OpenOrbis PS4 Toolchain** | compilador y enlazador para PS4 (`clang`, `ld`, `link.x`) |
| **LLVM-MinGW** | `make`, `ar` y utilidades POSIX para Windows |
| **CMake 3.31** | configuración del cliente GFN |
| **PortableGit** | shell POSIX que exige el `configure` de FFmpeg |
| **.NET** | `PkgTool.Core.exe` para construir y validar el PKG |

Se esperan en `tools/` (esa carpeta está en `.gitignore` por tamaño). El proyecto tiene scripts que
construyen las dependencias nativas desde `src/third_party/`.

```powershell
. .\scripts\ps4-env.ps1        # prepara el entorno
.\scripts\build-ps4.ps1        # compila el PKG completo + auditorias
```

Salida: `build/ps4/IV0000-GFNP00001_00-GFNPS4CLIENT0001.pkg`

**El build ejecuta las auditorías y falla si alguna no pasa:**

| Script | Comprueba |
|---|---|
| `scripts/audit-build.ps1` | **50 comprobaciones** sobre el binario y el fuente |
| `scripts/audit-navigation.ps1` | navegación del menú |
| `scripts/audit-translations.ps1` | coherencia de idiomas |
| `scripts/audit-settings-layout.ps1` | geometría de las filas y solapes |
| `scripts/run-host-tests.ps1` | **60 comprobaciones en 5 programas** de host |
| `PkgTool.Core.exe pkg_validate` | **28/28** sobre el PKG |

**Nota:** las auditorías no son decorativas. Varias nacieron de fallos reales, y **tres veces en este
proyecto una comprobación no era capaz de fallar** — se detectó probando cada una con una mutación.
Si añades una, **pruébala rompiendo a propósito lo que vigila.**

---

## Estructura del proyecto

```
src/
  ps4/main.cpp            La aplicacion completa (UI, bucle, estado)
  opennow/
    gfn/                  Cliente del servicio (auth, catalogo, sesion, persistencia)
    webrtc/               Sesion WebRTC, SDP, senalizacion, entrada
    stream/               Video: decodificador, conversores SSE2, presentacion
      PS4VideoOutRenderer.cpp   <- la ruta directa (la que da 60 fps)
      SDLVideoRenderer.cpp      <- la ruta SDL (respaldo)
      color_simd.cpp            <- conversion YUV->RGB y escalado en SSE2
    trace_detail.cpp      Traza detallada de arranque y sesion
    session_recorder.cpp  Anillo de frames presentados
  third_party/            Dependencias (ver abajo)
scripts/                  Build y auditorias
tests/                    Pruebas de host (se compilan en el PC, no en la consola)
cmake/                    Toolchain de CMake para PS4
assets/                   Fuentes e imagenes de la interfaz
patch/                    Parches y notas de investigacion puntual
build/                    El PKG compilado
```

### Dependencias incluidas (`src/third_party/`)

| Dependencia | Uso | Licencia |
|---|---|---|
| **FFmpeg** | decodificación H.264 software (`libavcodec` + `libavutil`) | LGPL/GPL según build |
| **libpeer** | WebRTC: ICE, DTLS, SRTP, SCTP | MIT |
| **Opus** | audio del stream | BSD |
| **cJSON / jansson** | JSON del protocolo | MIT |
| **opengnm** | reimplementación de referencia | ver su LICENSE |

**Compilar FFmpeg** usa `--disable-everything` con solo `h264`, `mjpeg` y `png` como decodificadores.

---

## Proyectos de referencia estudiados (no incluidos)

| Proyecto | Aporte |
|---|---|
| **OpenNOW** | implementación de referencia del protocolo NVST; FSR1 completo (MIT) |
| **prosper** | ABI verificada de `libSceVideodec2`; implementación de `videoout_present` |
| **shadPS4** | reimplementación de las librerías de PS4 — de aquí salió la traducción de `A8B8G8R8` a `RGBA8` que resolvió el orden de canales |
| **SDL-PS4** | driver de vídeo de PS4 para SDL2 — de aquí salió la cadena de color del driver |

No se incluyen en el repositorio: son cientos de MB y traen **sus propias licencias** (GPL), que
arrastrarían obligaciones a este proyecto.

---

## Notas de ingeniería, para quien continúe

Estas son las lecciones que costaron más caro, y están documentadas en el código:

1. **La cadena de color tiene tres eslabones y los tres tienen que coincidir.** El driver declara la
   ventana como `BGR888`, pero en SDL eso es `PACKEDORDER_XBGR`: **memoria `[R][G][B]`**. Y VideoOut
   está registrado como `A8B8G8R8`, que el emulador traduce a `RGBA8`. Por eso el lienzo es `BGR888`
   y el conversor SSE2 escribe `[B][G][R][A]` — el nombre del formato de VideoOut **se lee al revés**.

2. **`SDL_RenderCopy` no copia: CONVIERTE.** Sustituirlo por un `memcpy` "porque los bytes ya están
   bien" produjo colores alterados. La copia tiene que **deshacer el intercambio R↔B**.

3. **En el blit, el formato importa tanto como el tamaño.** `SDL_LowerBlitScaled` solo usa su camino
   rápido cuando los formatos de origen y destino **coinciden**; si no, convierte píxel a píxel
   (**20 ns/px**, medido). Se arregló **copiando por filas**, no cambiando formatos.

4. **El decodificador no es el cuello de botella, aunque el log lo parezca.** La diferencia
   `decoded - presented` es el número más llamativo del log y lleva a culpar al decodificador:
   **es lo contrario**. Entran 87,5 u/s y salen 87,5 f/s. Si `presented` va por debajo, **el frame se
   pierde en la presentación.**

5. **Una guarda que protege una cosa puede romper otra.** Envolver el bloque del stream en un `if`
   para no dibujar con `renderer = NULL` arregló un cierre **y congeló la imagen**, porque dentro de ese
   bloque también vive `Present()`. **El propio código ya avisaba: "este bloque hace dos cosas
   distintas y no se pueden tratar como una sola".**

6. **La presentación no puede depender de un contador que otra rama reescriba.** La puerta comparaba
   una generación que el camino de fallo marcaba como presentada **sin haber presentado**: un solo
   frame fallido detenía la presentación **para siempre**.

---

## Licencia y créditos

Proyecto **homebrew** sin relación con NVIDIA ni Sony.
Herramientas: **OpenOrbis**, **FFmpeg**, **libpeer**, **SDL2**, **Borealis**.
FSR es de **AMD** (MIT). Licencia del código propio: **MIT** (ver [`LICENSE`](LICENSE)).
