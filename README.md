# AJ GeForce NOW para PS4

Cliente de GeForce NOW para PS4, homebrew. No es un navegador ni una web metida en un envoltorio: hace
el recorrido entero del servicio por su cuenta.

Versión 4.32. El PKG está en `build/`, listo para instalar.

## Qué hace

Arranca, te logueas escaneando un QR, navegas el catálogo y cuando le das a jugar se conecta al servidor
de NVIDIA y te trae el vídeo a la consola. Todo el camino:

```
Login (OAuth por QR) → Catálogo → Cola de sesión (CloudMatch)
    → Señalización WebSocket + SDP → WebRTC (libpeer)
    → Stream H.264 → FFmpeg lo decodifica → libSceVideoOut lo presenta
```

El detalle que importa para que vaya fino: **el vídeo no se reescala en la CPU**. El frame llega a 720p
y se presenta tal cual en un framebuffer de 1280x720. El estirón hasta el panel de 1080p lo hace el
hardware de la consola. Eso es lo que separa ir a 20 fps de ir a 60, y costó bastante darse cuenta.

## Cómo va

Esto está medido en consola, no estimado:

| | |
|---|---|
| Frames presentados | **3543 en 57 segundos = 62,2 fps** |
| Conversión | 1280x720 → 1280x720, sin escalar en ningún frame |
| Coste de presentar un frame | **1221 µs de media**, sobre un presupuesto de 16666 |
| Frames que se pasan del presupuesto | **12 de 3543** (un 0,3 %) |
| Arranque | bucle listo a los 7 ms |
| Audio y mando | sin pérdidas, mando a 125 Hz |
| Red | sin huecos ni paquetes perdidos |
| Memoria | 4608 MB constantes durante 12,9 horas |

El decodificador produce unos 64 fps y del servidor llegan unas 65 unidades por segundo, así que se
presenta el 96 % de lo que entra. No hay ningún cuello de botella.

## Lo que no funciona

**El decodificador por hardware no se puede usar.** `libSceVideodec2` no carga en el firmware que
tenemos (`rc=0x805A1000`) y la librería de arbitración no tiene firmas verificadas. Así que se decodifica
por software. Da igual, porque va sobrado: 108-118 µs por lote.

**El servidor a veces baja la resolución a 960x540 aunque le pidas 720p.** Lo hemos medido: empezó
entregando 720p y a mitad de sesión se pasó a 540p. Eso se nota en que la imagen pierde nitidez y
aparece pixelación, y desde el cliente no se puede arreglar, porque el detalle que no llegó no se puede
inventar. Hemos probado de todo para evitarlo: viewport 1080p en el SDP (que es lo que funcionaba en la
v3.15), REMB a 50 Mbps, modo de calidad Original, scalingFeature1 y el prefiltro del servidor. Ninguno
lo elimina del todo.

Hay además un círculo vicioso con esto. Si el servidor baja la resolución y tú estabas escalando a
1080p, tu coste no baja, así que él no ve mejoría y no la recupera. La ruta directa rompe ese círculo,
porque su framebuffer se adapta al stream.

**La ruta SDL, que es el respaldo, no puede llegar a 60 fps.** No es que esté mal optimizada: el driver
de SDL-PS4 fija el lienzo al tamaño del panel (1920x1080) y copia a VideoOut con un memcpy sin escalar,
así que un stream de 720p se tiene que escalar a 1080p en la CPU. Eso son unos 31000 µs sobre un
presupuesto de 16666. El techo está en unos 20 fps y no hay forma de bajarlo.

**Hay cierres esporádicos que no hemos conseguido explicar.** Las sesiones se han terminado a los 31,
42, 178, 214, 233, 269, 325, 497, 515, 603, 1037 y 3570 segundos. No hay un patrón de tiempo, así que
no es el sistema matando la app ni un límite de nada. Descartamos con medidas la memoria (plana durante
240 segundos), la red, el decodificador y la cola. Dos causas sí las pillamos y están corregidas:
dibujar con `renderer = NULL` y que el callback de presentación se quedara fuera de alcance.

## Lo que no pudimos hacer

**Decodificación por hardware.** El módulo no carga en este firmware y la ABI v1 responde `0x80C10001`
en `query_resource_info`. Sin firmas verificadas no hay forma segura de usarla.

**1080p.** Se intentó y la app se cerró. Los números: 1 flip presentado con framebuffer de 1080p frente
a 119 con 720p. Además, sobre un panel de 1080p un framebuffer de 720p ya se estira y llena la pantalla,
así que 1080p solo te haría escribir cuatro veces más píxeles a cambio de nada. Desde la v4.22 hay un
techo en `Initialize()` que hace imposible registrarlo, para que no vuelva a pasar por accidente.

**Evitar el 540p.** Ver arriba: cinco intentos, ninguno funcionó.

**Pedirle a VideoOut que escale a un tamaño concreto.** `sceVideoOutSubmitFlip` no acepta un rectángulo
de origen, así que no se le puede decir "esto a 960x540 estíralo a 1080p". Lo único que se aprovecha es
que el framebuffer se estira al panel.

**Garantizar los 60 fps siempre.** Depende de que la ruta directa no se caiga. Hay un vigilante que, si
deja de presentar, degrada a SDL sola en vez de dejarte la imagen congelada, pero eso te baja a ~20 fps.
Es mejor que una foto fija, pero no es lo que quieres.

## Requisitos

| | |
|---|---|
| Consola | PS4 Pro es donde se ha probado. Fat y Slim deberían ir igual |
| Jailbreak | **GoldHEN** |
| Firmware | **Cualquiera que GoldHEN soporte.** Solo hemos probado en 9.00 |
| Cuenta | GeForce NOW (Free, Priority o Ultimate) |
| Red | Cable mejor que WiFi |
| Mando | DualShock 4. Teclado y ratón USB opcionales |

Sobre el firmware: no lo hemos probado en todas las versiones, así que no vamos a jurar que funcione en
todas. Pero la app no hace nada específico de una versión concreta, solo usa las librerías normales del
sistema (VideoOut, AudioOut, Videodec, Pad), y esas no cambian entre firmwares. Si tienes GoldHEN
funcionando, lo normal es que arranque. Si pruebas en otra versión y falla, el `diagnostic.log` lo dirá.

No hace falta PSN.

## Instalación

1. Copia `build/IV0000-GFNP00001_00-GFNPS4CLIENT0001.pkg` a un USB (exFAT o FAT32).
2. En la PS4, con GoldHEN activo: **Package Installer**.
3. Instala y aparecerá **AJ GeForce NOW** en el menú.

El Title ID es `GFNP00001`. Es el único que la consola aceptaba después de muchas pruebas.

## Cómo se usa

1. Abres la app y aceptas el aviso beta.
2. Le das a **Iniciar sesión** y sale un QR. Lo escaneas, autorizas en tu cuenta de NVIDIA y ya está.
   Solo hay que hacerlo una vez.
3. Navegas el catálogo, eliges juego y esperas la cola.
4. Cuando entras, el mando va a 125 Hz. El menú de la app se abre con PS/Options.

| Botón | Qué hace |
|---|---|
| X | Confirmar |
| Círculo | Volver |
| Arriba/Abajo | Moverte por el menú |
| Izquierda/Derecha | Cambiar el valor de un ajuste |

## Ajustes

Son 13 filas. Las que importan:

| # | Ajuste | Notas |
|---|---|---|
| 0 | Idioma | |
| 1 | Dispositivo de entrada | Auto / Mando / Teclado |
| 2 | Resolución | Solo 720p. Es la que decide framebuffer y petición |
| 3 | FPS de transmisión | 30 o 60 |
| 4 | Límite de bitrate | 25 / 30 / 35 Mbps |
| 5 | Modo de calidad | Original / Clarity / Adaptive |
| 6 | Buffer de audio | 20-80 ms |
| 7 | Idioma del juego | 8 idiomas |
| 8 | Decodificador | Informativo: `NO DISPONIBLE (SW)` |
| 9 | Resolución de vídeo | Informativo: `720P (FIJO, 1080P DESCARTADO)` |
| 10 | Realce de nitidez | Cuesta ×3,9 en CPU. Desactivado por defecto |
| 11 | Región | |
| 12 | Guardar y volver | |

El 1080p está bloqueado por lo que contaba arriba: el único intento cerró la app. Si quieres entender
por qué, mira la sección de lo que no pudimos hacer.

## Los registros

Todo se guarda en `/data/gfnps4/`.

Desde la v4.25 hay una traza detallada que sirve para responder a "¿por qué no van a 60 fps?". Son tres
ficheros:

| Fichero | Qué tiene |
|---|---|
| `trace_boot.txt` | Un renglón por cada paso del arranque, con su tiempo. El de `APP_READY` es lo que tarda en estar lista |
| `trace_stream.txt` | Los hitos de la sesión y un resumen por segundo |
| `trace_stream.csv` | **Una fila por frame presentado** |

El CSV es el útil. Cada fila tiene:

| Columna | Qué te dice |
|---|---|
| `t_ms`, `frame`, `gen` | cuándo, qué frame, y su generación |
| `reused` | 1 = la cola estaba vacía, o sea que no es problema de presentación |
| `src_w/h`, `dst_w/h` | lo que mandó el servidor y el tamaño real del framebuffer |
| `scaled` | 1 = se escaló en CPU, que es lo que mata los 60 fps |
| `q`, `dec_fps` | cola del decodificador y su caudal |
| `pres_us` | lo que costó presentar ese frame |
| `budget_us`, `over` | 16666, y 1 si se pasó |

Con eso, si algo va mal se cuenta cuántas filas tienen `over=1` y se mira si es por `scaled` o por
`pres_us`. No hay que adivinar nada.

Luego está el log de siempre:

| Fichero | Qué tiene |
|---|---|
| `diagnostic.log` | El log completo de la sesión |
| `last_stage.txt` | En qué parte del código estaba la app en el último latido (uno por segundo) |
| `session_frames.csv` | Los últimos 4096 frames presentados |
| `session_summary.txt` | Resumen: frames, cambios de resolución, tamaños de paquete |
| `settings.cfg` | La configuración |

Y las marcas que merece la pena buscar:

```
APP_START version=4.32
VIDEOOUT_HANDOFF_COMPLETE mode=direct_hardware_60fps
VIDEOOUT_PRESENT_PATH path=direct_1to1 resolution=1280x720
VIDEOOUT_SIZE_CAPPED_720P ...                    <- el techo de 720p actuando
VIDEOOUT_PRESENT_GATE presentar=N cola_vacia=N   <- la puerta de presentación
VIDEOOUT_PRESENT_WATCHDOG ...                    <- el vigilante degradando a SDL
VIDEOOUT_RECOVERY_FROM_MAIN_LOOP ...             <- la recuperación desde el bucle
```

## Compilar

El repo no trae las herramientas: son más de 3 GB y cada una tiene su licencia. Hay que conseguirlas:

| Herramienta | Para qué |
|---|---|
| OpenOrbis PS4 Toolchain | El compilador y el enlazador de PS4 (`clang`, `ld`, `link.x`) |
| LLVM-MinGW | `make`, `ar` y utilidades POSIX para Windows |
| CMake 3.31 | Configurar el cliente |
| PortableGit | El shell POSIX que pide el `configure` de FFmpeg |
| .NET | `PkgTool.Core.exe`, para construir y validar el PKG |

Van en `tools/`, que está en el `.gitignore`. Luego:

```powershell
. .\scripts\ps4-env.ps1        # prepara el entorno
.\scripts\build-ps4.ps1        # compila el PKG y pasa las auditorías
```

El resultado sale en `build/ps4/IV0000-GFNP00001_00-GFNPS4CLIENT0001.pkg`.

El build pasa estas comprobaciones y falla si alguna no cuadra:

| Script | Qué comprueba |
|---|---|
| `scripts/audit-build.ps1` | 50 comprobaciones sobre el binario y el código |
| `scripts/audit-navigation.ps1` | la navegación del menú |
| `scripts/audit-translations.ps1` | que los idiomas cuadren |
| `scripts/audit-settings-layout.ps1` | la geometría de las filas |
| `scripts/run-host-tests.ps1` | 60 comprobaciones en 5 programas que corren en el PC |
| `PkgTool.Core.exe pkg_validate` | 28/28 sobre el PKG |

Un aviso sobre las auditorías: no son de adorno, varias salieron de fallos reales. Y **tres veces en este
proyecto una comprobación no era capaz de fallar**, y nos dimos cuenta probándolas rompiendo a propósito
lo que vigilaban. Si añades una, haz lo mismo: rómpelo y mira si salta.

## Cómo está organizado

```
src/
  ps4/main.cpp            La aplicación entera (interfaz, bucle, estado)
  opennow/
    gfn/                  El cliente del servicio: login, catálogo, sesión, persistencia
    webrtc/               La sesión WebRTC, SDP, señalización, entrada
    stream/               El vídeo
      PS4VideoOutRenderer.cpp   <- la ruta directa, la que da 60 fps
      SDLVideoRenderer.cpp      <- la ruta SDL, el respaldo
      color_simd.cpp            <- conversión YUV y escalado en SSE2
    trace_detail.cpp      La traza detallada
    session_recorder.cpp  El anillo de frames
  third_party/            Las dependencias
scripts/                  Build y auditorías
tests/                    Pruebas que corren en el PC, no en la consola
cmake/                    Toolchain de CMake para PS4
assets/                   Fuentes e imágenes
build/                    El PKG
```

Las dependencias que van incluidas en `src/third_party/`:

| Dependencia | Para qué | Licencia |
|---|---|---|
| FFmpeg | Decodificar H.264 por software (solo `libavcodec` y `libavutil`) | LGPL/GPL según el build |
| libpeer | El WebRTC: ICE, DTLS, SRTP, SCTP | MIT |
| Opus | El audio | BSD |
| cJSON, jansson | El JSON del protocolo | MIT |
| opengnm | Una implementación de referencia | mira su LICENSE |

FFmpeg se compila con `--disable-everything` y solo con `h264`, `mjpeg` y `png`.

## Cosas de terceros que miramos

Estos no van en el repo, son cientos de MB y traen licencias GPL que arrastrarían obligaciones. Los
teníamos solo para leer:

| Proyecto | Qué sacamos |
|---|---|
| OpenNOW | La implementación de referencia del protocolo NVST |
| prosper | La ABI de `libSceVideodec2` y cómo se hace `videoout_present` |
| shadPS4 | De aquí salió que `A8B8G8R8` es en realidad `RGBA8`, que fue lo que resolvió el orden de canales |
| SDL-PS4 | El driver de vídeo de PS4 para SDL2, de donde salió la cadena de color |

## Notas para quien siga con esto

Estas son las cosas que nos costaron caro. Están también comentadas en el código, donde toca.

**La cadena de color tiene tres eslabones y tienen que coincidir los tres.** El driver declara la
ventana como `BGR888`, pero en SDL eso significa `PACKEDORDER_XBGR`, o sea memoria `[R][G][B]`. Y
VideoOut está registrado como `A8B8G8R8`, que el emulador traduce a `RGBA8`. Por eso el lienzo es
`BGR888` y el conversor SSE2 escribe `[B][G][R][A]`. El nombre del formato de VideoOut se lee al revés,
y eso despista mucho.

**`SDL_RenderCopy` no copia, convierte.** Lo cambiamos por un memcpy pensando que los bytes ya estaban
bien y salieron los colores cambiados. La copia tiene que deshacer el intercambio de rojo y azul.

**En el blit el formato importa tanto como el tamaño.** `SDL_LowerBlitScaled` solo usa el camino rápido
cuando los formatos de origen y destino coinciden. Si no, convierte píxel a píxel, y eso son 20 ns por
píxel medidos. Se arregló copiando por filas, no cambiando formatos.

**El decodificador no es el cuello de botella, aunque el log lo parezca.** La diferencia entre
`decoded` y `presented` es el número más llamativo de todo el log y lleva a culpar al decodificador.
Es justo al revés: entran 87,5 unidades por segundo y salen 87,5. Si `presented` va por debajo, el frame
se pierde al presentarlo.

**Una guarda que arregla una cosa puede romper otra.** Envolvimos el bloque del stream en un `if` para
no dibujar con `renderer = NULL`, y eso arregló un cierre pero congeló la imagen, porque dentro de ese
bloque también vive `Present()`. El propio código ya avisaba: "este bloque hace dos cosas distintas y no
se pueden tratar como una sola". Lo teníamos escrito y aun así caímos.

**La presentación no puede depender de un contador que otra rama reescriba.** La puerta comparaba una
generación que el camino de fallo marcaba como presentada sin haberla presentado. Con eso, un solo frame
que fallara detenía la presentación para siempre.

## Licencia y créditos

Proyecto homebrew, sin relación con NVIDIA ni con Sony. El código propio es MIT, mira `LICENSE`.
Herramientas: OpenOrbis, FFmpeg, libpeer, SDL2, Borealis. FSR es de AMD y también es MIT.
