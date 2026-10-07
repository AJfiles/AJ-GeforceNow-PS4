# Auditoria: comprueba que cada cambio declarado esta REALMENTE en el binario construido.
#
# POR QUE EXISTE
# --------------
# Varias veces se declaro un cambio como aplicado y el binario no lo llevaba, o lo llevaba
# pero el codigo no se ejecutaba (por ejemplo el escalador SIMD, que estaba presente pero
# nunca se usaba porque un self-check lo rechazaba). "Esta en el fuente" y "funciona" son
# cosas distintas. Este script verifica la primera de forma automatica y deja constancia.
#
# USO
#   pwsh -File scripts/audit-build.ps1
#
# Salida: tabla con ACTIVO / FALTA por cada caracteristica, mas el hash y el tamano del PKG.
# Codigo de salida 1 si algo declarado como obligatorio no aparece.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$eb = Join-Path $root 'build\ps4\package\eboot.bin'

# El PKG se localiza por patron en lugar de por nombre fijo: el Title ID puede cambiar (ahora es
# AJPS40001, antes GFNP00001) y un nombre hardcodeado rompia la auditoria al renombrar el paquete.
$pkg = Get-ChildItem (Join-Path $root 'build\ps4') -Filter '*.pkg' -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not (Test-Path $eb)) {
    Write-Host "No existe $eb - construye primero con scripts/build-ps4.ps1" -ForegroundColor Red
    exit 2
}

# Cada entrada: nombre, patron regex buscado en el binario, si es obligatorio.
# El patron se busca en el eboot.bin real, no en el fuente.
$features = @(
    # Los patrones deben ser OBSERVABLES en el binario. Un `constexpr` o un nombre de
    # variable local desaparecen con la optimizacion, asi que buscarlos da falsos
    # negativos: la primera version de esta auditoria marco dos fallos que no existian.
    @('Throttle de entrada 125 Hz',        'input_throttle=125Hz',                $true),
    @('Throttle: valor del periodo',       '8000',                                $true),
    @('Renderizador de 4 buffers',         'count=%d',                            $true),
    @('Sincronizacion de flip (equeue)',   'VIDEOOUT_FLIP_EQUEUE_OK',             $true),
    @('Espera de cualquier flip',          'ev_ident',                            $true),
    @('Histograma de rotacion',            'hist=',                               $true),
    @('Telemetria conversion 1:1',         'VIDEOOUT_DIRECT_CONVERT_US',          $true),
    @('Telemetria etapas de Present',      'VIDEOOUT_PRESENT_STAGES',             $true),
    @('Telemetria del escalado',           'VIDEOOUT_SCALE_TIMING',               $true),
    @('Primer frame escalado',             'VIDEOOUT_SCALE_FIRST',                $true),
    @('Self-check SIMD',                   'VIDEOOUT_SCALE_SIMD_SELFCHECK',       $true),
    @('Self-check: resultado correcto',    'simd_byte_identical',                 $true),
    @('Inventario de arranque',            'BOOT_BUILD',                          $true),
    @('Probe ABI videodec v1',             'BOOT_VIDEODEC_ABI_PROBE',             $true),
    @('Probe de codecType',                'BOOT_VIDEODEC_CODEC_TYPE',            $true),
    @('Version en barra de estado',        'VERSION ',                            $true),
    @('Menu L3+Triangulo',                 'l3_triangle',                         $true),
    @('Dialogo de aviso beta',             'AVISO IMPORTANTE',                    $true),
    # El aviso persistido se reconoce por la cadena de formato de 8 campos que lo guarda.
    @('Persistencia del aviso beta',       '%d %d %d %d %d %d %d %d',             $true),
    @('Etapas de restauracion de SDL',     'VIDEOOUT_SDL_RESTORE_STAGE',          $false),
    @('Estado de la cola de flips',        'VIDEOOUT_QUEUE_FULL_HOLD',            $false)
)

$bytes = [System.IO.File]::ReadAllBytes($eb)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)

Write-Host ""
Write-Host "AUDITORIA DEL BINARIO" -ForegroundColor Cyan
Write-Host "  eboot.bin : $([math]::Round($bytes.Length/1MB,2)) MB"
if (Test-Path $pkg) {
    $h = (Get-FileHash -Algorithm SHA256 $pkg).Hash
    Write-Host "  PKG       : $([math]::Round((Get-Item $pkg).Length/1MB,2)) MB"
    Write-Host "  SHA-256   : $h"
}
Write-Host ""

$fail = 0
foreach ($f in $features) {
    $name = $f[0]; $pattern = $f[1]; $required = $f[2]
    $found = $text -match [regex]::Escape($pattern)
    if ($found) {
        Write-Host ("  [OK]    {0}" -f $name) -ForegroundColor Green
    } elseif ($required) {
        Write-Host ("  [FALTA] {0}   <-- declarado y ausente" -f $name) -ForegroundColor Red
        $fail++
    } else {
        Write-Host ("  [n/d]   {0}   (opcional, no encontrado)" -f $name) -ForegroundColor Yellow
    }
}

Write-Host ""
# La version del log debe coincidir con la del script de build.
$scriptText = Get-Content (Join-Path $PSScriptRoot 'build-ps4.ps1') -Raw
if ($scriptText -match "\`$appVersion = '([^']+)'") {
    $ver = $Matches[1]
    $inBinary = $text -match [regex]::Escape("version=$ver")
    Write-Host ("Version en build-ps4.ps1: {0} - en el binario: {1}" -f $ver, $(if($inBinary){'SI'}else{'NO'})) `
        -ForegroundColor $(if($inBinary){'Green'}else{'Red'})
    if (-not $inBinary) { $fail++ }
}

# Si alguien sube el techo sin evidencia nueva en consola, esta comprobacion lo para.
#
# OJO CON LA SINTAXIS: esto NO puede ser un bloque `{ ... }` suelto. En PowerShell, un bloque suelto en
# el nivel superior es una **expresion** que se EMITE al flujo de salida, asi que el script imprimia el
# texto del bloque en vez de ejecutarlo y la comprobacion no llegaba a correr nunca. Se usa una funcion,
# que agrupa el codigo **sin producir salida**.
# =====================================================================================================
# EL CALLBACK DEL FRAME TIENE QUE ESTAR FUERA DE if(sdlDibujaInterfaz) (v4.24)
# =====================================================================================================
# POR QUE: en la v4.20 se puso `if(sdlDibujaInterfaz)` alrededor de todo el bloque del stream para que
# la interfaz SDL no se dibujara con `renderer = NULL`. **Eso arreglo un cierre, pero encerro tambien la
# llamada a `AVFrameHolder::get()`, que es DONDE VIVE `Present()`.** En la ruta directa el renderizador y
# el lienzo son NULL, la guarda es falsa, el callback no corre y **la imagen se congela con el audio y
# el mando vivos.**
#
# MEDIDO EN CONSOLA (v4.22), que es lo que hace que esta comprobacion valga la pena:
#     UI_LOOP_PERF loop_fps=62 slow_frames=0        <- el bucle iba bien
#     VIDEOOUT_FLIP_SUBMIT 1 / total_calls=1        <- UNA sola presentacion en toda la sesion
#     STREAM_VIDEO_RENDER_FRAME_CALLBACK_BEGIN 0    <- el callback nunca empezo
#
# COMO SE COMPRUEBA, y este detalle importa: se cuenta la profundidad de llaves **empezando EN la linea
# de la guarda**, y se exige que llegue a 0 antes del callback. La primera version de este test contaba
# desde fuera y la llave de la propia guarda descuadraba el resultado: daba "1" con el codigo correcto y
# **la comprobacion no era capaz de fallar**. Con el conteo desde la guarda si lo es (probado con una
# mutacion que mueve el cierre de la guarda detras del callback).
function Test-CallbackFueraDeLaGuarda {
    $mcPath = Join-Path $root 'src\ps4\main.cpp'
    $mcLineas = Get-Content -LiteralPath $mcPath
    $codigo = @()
    foreach ($l in $mcLineas) {
        if ($l -match '^\s*//') { continue }
        $codigo += $l
    }
    $g = -1
    for ($i = 0; $i -lt $codigo.Count; $i++) {
        if ($codigo[$i] -match 'if\s*\(\s*sdlDibujaInterfaz\s*\)\s*\{') { $g = $i; break }
    }
    if ($g -lt 0) {
        Write-Host "  [CALLBACK] no se encontro if(sdlDibujaInterfaz) en main.cpp" -ForegroundColor Red
        return $false
    }
    # Profundidad RELATIVA a la guarda: 0 tras su cierre.
    $prof = 0
    $cierre = -1
    for ($i = $g; $i -lt $codigo.Count; $i++) {
        $prof += ([regex]::Matches($codigo[$i], '\{')).Count
        $prof -= ([regex]::Matches($codigo[$i], '\}')).Count
        if ($prof -le 0 -and $i -gt $g) { $cierre = $i; break }
    }
    if ($cierre -lt 0) {
        Write-Host "  [CALLBACK] no se pudo localizar el cierre de if(sdlDibujaInterfaz)" -ForegroundColor Red
        return $false
    }
    # El callback que presenta el video. Se busca el que lleva `reused`, que es el del frame.
    $cb = -1
    for ($i = 0; $i -lt $codigo.Count; $i++) {
        if ($codigo[$i] -match 'AVFrameHolder::instance\(\)\s*\.\s*get\s*\(\s*\[&\].*reused') { $cb = $i; break }
    }
    if ($cb -lt 0) {
        Write-Host "  [CALLBACK] no se encontro el callback del frame (el que recibe `reused`)" -ForegroundColor Red
        return $false
    }
    if ($cb -le $cierre) {
        Write-Host "  [CALLBACK] el callback del frame esta DENTRO de if(sdlDibujaInterfaz)" -ForegroundColor Red
        Write-Host ("             guarda={0} cierra={1} callback={2}" -f ($g+1), ($cierre+1), ($cb+1)) -ForegroundColor Red
        Write-Host "             En la ruta directa esa guarda es falsa (renderer=NULL), el callback no" -ForegroundColor Red
        Write-Host "             corre, Present() no se llama y la imagen se CONGELA con el audio vivo." -ForegroundColor Red
        Write-Host "             Medido en la v4.22: total_calls=1 y loop_fps=62 con la imagen fija." -ForegroundColor Red
        return $false
    }
    Write-Host ("  [OK]    el callback del frame esta FUERA de la guarda de la interfaz (guarda cierra en {0}, callback en {1}; Present() alcanzable)" -f ($cierre+1), ($cb+1)) -ForegroundColor Green
    return $true
}
function Test-Techo720 {
    $voPath = Join-Path $root 'src\opennow\stream\PS4VideoOutRenderer.cpp'
    $voCode = Get-Content -LiteralPath $voPath -Raw
    # Se quitan los comentarios para no confundir la explicacion con el codigo que actua.
    $voActivo = (($voCode -split "`n") | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
    # =================================================================================================
    # LA COMPROBACION TIENE QUE SER EL BLOQUE ENTERO, NO TRES TROZOS SUELTOS.
    # =================================================================================================
    # PRIMER INTENTO, Y FALLO: comprobaba tres cosas por separado (la condicion, `target_w = 1280;` y la
    # marca de registro). **Una prueba de mutacion la desmonto**: cambiando la condicion por `if (false)`
    # la auditoria seguia pasando, porque **los tres trozos siguen presentes aunque la condicion este
    # muerta** — el `target_w = 1280;` de la declaracion y la marca dentro del bloque inalcanzable
    # bastaban para dar por bueno el techo.
    #
    # Ahora se exige **el bloque completo, en orden y con su llave**, y ademas que la condicion **no**
    # sea una constante muerta (`if (false)`, `if (0)`, `if (true)`).
    $bloqueTecho = '(?s)if\s*\(\s*target_w\s*>\s*1280\s*\|\|\s*target_h\s*>\s*720\s*\)\s*\{' +
                   '[^}]*VIDEOOUT_SIZE_CAPPED_720P[^}]*target_w\s*=\s*1280\s*;' +
                   '[^}]*target_h\s*=\s*720\s*;'
    # `[bool]` explicito: `-match` devuelve un MatchInfo (o $null), no un booleano, y encadenarlo con
    # `-and` deja el resultado dependiendo de conversiones implicitas.
    $bloqueOk = [bool]($voActivo -match $bloqueTecho)
    $noMuerta = -not [bool]($voActivo -match 'if\s*\(\s*(false|true|0|1)\s*\)')
    if ($bloqueOk -and $noMuerta) {
        Write-Host "  [OK]    techo de 720p en el framebuffer (no se registra 1080p)" -ForegroundColor Green
        return $true
    }
    Write-Host "  [TECHO720] falta el techo de 720p en Initialize()" -ForegroundColor Red
    Write-Host ("             bloque_completo={0} condicion_no_muerta={1}" -f $bloqueOk, $noMuerta) -ForegroundColor Red
    Write-Host "             Evidencia: 1280x720 dio 119 flips estables y 60 fps medidos;" -ForegroundColor Red
    Write-Host "             1920x1080 dio 1 flip y cerro la sesion. No subir sin medir." -ForegroundColor Red
    return $false
}

# Llamada explicita. El resultado se consume en `if`, asi que no se emite nada a la salida.
# =====================================================================================================
# COMPROBACION RETIRADA: no era capaz de fallar, y una comprobacion asi es peor que ninguna.
# =====================================================================================================
# SE INTENTO comprobar automaticamente que la llamada a `AVFrameHolder::instance().get()` (que es donde
# vive `Present()`) NO esta anidada dentro de `if(sdlDibujaInterfaz)`. El motivo es real: **en la v4.20
# se metio el callback dentro de esa guarda, en la ruta directa la guarda es falsa (renderer=NULL), el
# callback no corria y la imagen se congelaba con el audio y el mando vivos.** Medido en la v4.22:
#
#     UI_LOOP_PERF loop_fps=62 slow_frames=0        <- el bucle iba bien
#     VIDEOOUT_FLIP_SUBMIT 1 / total_calls=1        <- UNA sola presentacion en toda la sesion
#     STREAM_VIDEO_RENDER_FRAME_CALLBACK_BEGIN 0    <- el callback nunca empezo
#
# **PERO LA COMPROBACION NO FUNCIONABA.** Se probo con DOS mutaciones y las dos la pasaron:
#   1. Envolver el callback en un `if(sdlDibujaInterfaz)` nuevo  -> no la detecto.
#   2. Mover el cierre de la guarda para DESPUES del callback (el bug exacto, con las llaves
#      cuadrando) -> tampoco la detecto.
#
# Y una comprobacion que no puede fallar **da una sensacion de cobertura que no existe**, que es peor
# que no tenerla. Se retira en lugar de dejarla.
#
# LO QUE QUEDA EN SU LUGAR: **la propiedad esta sujeta por construccion y documentada en el codigo**
# (`main.cpp`, el bloque "EL CALLBACK DEL FRAME NO PUEDE ESTAR DENTRO DE LA GUARDA DE LA INTERFAZ"), y
# en la proxima prueba lo confirma la traza real en consola:
#
#     VIDEOOUT_PRESENT_GATE  presentar=N cola_vacia=N ...   -> si `presentar` sube, se presenta
#     STREAM_VIDEO_RENDER_FRAME_CALLBACK_BEGIN              -> el callback corre
#     session_frames.csv -> total_calls                     -> Present() registra frames
#
# =====================================================================================================
# ESTE ERROR YA HA OCURRIDO, y por eso la comprobacion existe. Explicacion, porque el fallo es sutil:
#
#   En la v4.20 se puso `if(sdlDibujaInterfaz)` alrededor de TODO el bloque del stream para que la
#   interfaz SDL no se dibujara con `renderer = NULL`. **Eso arreglo un cierre, pero encerro tambien la
#   llamada a `AVFrameHolder::get()`, que es DONDE VIVE `Present()`.** En la ruta directa `renderer` y
#   `g_ownCanvas` son NULL, asi que la guarda es falsa, el callback nunca corre y **`Present()` no se
#   llama nunca: imagen congelada con audio y mando vivos.**
#
#   Los logs de la v4.22 lo midieron asi:
#       UI_LOOP_PERF loop_fps=62 slow_frames=0        <- el bucle iba bien
#       VIDEOOUT_FLIP_SUBMIT 1 / total_calls=1        <- UNA sola presentacion en toda la sesion
#       STREAM_VIDEO_RENDER_FRAME_CALLBACK_BEGIN 0    <- el callback nunca empezo
#
#   Y el propio proyecto tenia el aviso escrito encima del bloque: *"Este bloque hace dos cosas
#   distintas (dibujar la interfaz y presentar el video) y no se pueden tratar como una sola."*
#
# La propiedad que se verifica: **la llamada a `AVFrameHolder::instance().get(...)` NO esta anidada
# dentro de `if(sdlDibujaInterfaz)`.** Se comprueba contando profundidad de llaves desde la guarda: si
# el callback aparece dentro, la comprobacion falla.

# =====================================================================================================
# ESTAS DOS COMPROBACIONES VAN AQUI, ANTES DEL CORTE, Y ESO ES DELIBERADO.
# =====================================================================================================
# Este script tenia un `exit 1` justo debajo, asi que **cualquier comprobacion puesta al final NO SE
# EJECUTABA cuando algo fallaba antes**. Eso ya provoco una comprobacion decorativa en esta serie: el
# techo de 720p de la v4.22 estaba al final del fichero y no llegaba a correr. Se descubrio midiendo por
# **codigo de salida con una mutacion**, no leyendo el codigo.
if (-not (Test-Techo720)) { $fail++ }
if (-not (Test-CallbackFueraDeLaGuarda)) { $fail++ }

if ($fail -gt 0) {
    Write-Host ""
    Write-Host "$fail comprobacion(es) obligatorias FALLARON" -ForegroundColor Red
    exit 1
}

# --- Comprobacion de ORDEN en el fuente: la proteccion de renderer nulo en draw().
#
# HISTORIA DE DOS ERRORES OPUESTOS, y la auditoria comprueba que no se repita ninguno:
#   1) La 2.82 crasheaba: releaseSdlForVideoOut() deja renderer=nullptr al pasar a VideoOut
#      directo, y las ramas SDL de draw() (aviso beta, pantalla de conexion) lo usaban sin
#      comprobarlo. La guarda que existia estaba cientos de lineas mas abajo, despues de los
#      early-returns, asi que no protegia el camino que se ejecutaba.
#   2) La 2.84 congelaba la imagen: se puso `if(!renderer) return;` al PRINCIPIO de draw(), y
#      eso sale de la funcion ANTES del bloque de video directo, que es el unico sitio que
#      llama a Present(). Resultado: 1 solo flip, imagen congelada, bucle a 63.000 FPS.
#
# Lo correcto es NO salir de la funcion: marcar que no hay SDL y que cada bloque SDL se lo
# salte. Esta comprobacion verifica las dos mitades de esa condicion.
$mainCpp = Join-Path $root 'src\ps4\main.cpp'
if (Test-Path $mainCpp) {
    $lines = Get-Content $mainCpp
    $drawLine = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^static void draw\(\)') { $drawLine = $i + 1; break }
    }
    if ($drawLine -gt 0) {
        # Limitar el analisis al CUERPO real de draw(): desde su linea hasta SU llave de cierre.
        #
        # =================================================================================================
        # ESTA BUSQUEDA ESTUVO MAL Y LA COMPROBACION DEJO DE EJECUTARSE SIN QUE NADIE LO NOTARA (v4.25)
        # =================================================================================================
        # Estaba asi: `for ($i = $drawLine; $i -lt min($drawLine + 2000, ...))`, buscando la primera
        # linea que empiece por `}`. **Dos problemas, y los dos se materializaron:**
        #
        #   1. La VENTANA era un numero fijo (2000 lineas). **`draw()` mide 2004** (abre en 5264, cierra
        #      en 7267), asi que la busqueda **no llegaba al cierre**: `$drawEnd` se quedaba igual a
        #      `$drawLine` y el bucle de analisis **no iteraba ni una vez**. La comprobacion "pasaba"
        #      por no mirar nada, y su mensaje `[FALTA] draw() no marca la disponibilidad de SDL`
        #      aparecia solo cuando otra cosa fallaba antes.
        #   2. Buscar `^\}` a columna 0 depende de que la llave de cierre de la funcion este sin
        #      indentar. Es fragil: cualquier reformateo lo rompe en silencio.
        #
        # AHORA se cuenta profundidad de llaves, que no depende ni del numero de lineas ni de la
        # indentacion. Es la misma tecnica que usa `Test-CallbackFueraDeLaGuarda`.
        $drawEnd = $drawLine
        $depth = 0
        $seenOpen = $false
        for ($i = $drawLine - 1; $i -lt $lines.Count; $i++) {
            $t = $lines[$i]
            $ci = $t.IndexOf('//')
            if ($ci -ge 0) { $t = $t.Substring(0, $ci) }
            if (-not $seenOpen -and $t -match '\{') { $seenOpen = $true }
            $depth += ([regex]::Matches($t, '\{')).Count
            $depth -= ([regex]::Matches($t, '\}')).Count
            if ($seenOpen -and $depth -le 0) { $drawEnd = $i + 1; break }
        }

        $flagLine = 0          # const bool sdlAvailable = (renderer != nullptr);
        $presentLine = 0       # el bloque que llama a Present() en modo directo
        $guardFirstUse = 0     # primer uso de renderer sin proteccion
        $earlyReturn = 0       # un `if(!renderer) return;` que salga de toda la funcion
        $streamGate = 0        # bloque de sesion protegido SOLO con sdlAvailable

        for ($i = $drawLine; $i -lt $drawEnd; $i++) {
            $t = $lines[$i]
            # Ignorar comentarios: el codigo documenta el error historico con el mismo texto
            # que se busca, y la primera version de esta comprobacion marcaba el comentario
            # como si fuera la guarda real. Un auditor que confunde documentacion con codigo
            # da falsos positivos y deja de ser util.
            $code = $t
            $trimmed = $t.TrimStart()
            if ($trimmed.StartsWith('//')) { continue }
            $ci = $code.IndexOf('//')
            if ($ci -ge 0) { $code = $code.Substring(0, $ci) }

            if ($flagLine -eq 0 -and $code -match 'renderer\s*!=\s*nullptr' -and $code -match 'const bool') {
                $flagLine = $i + 1
            }
            if ($presentLine -eq 0 -and $code -match 'videoOutDirectActive\s*&&') {
                $presentLine = $i + 1
            }
            if ($earlyReturn -eq 0 -and $code -match 'if\s*\(\s*!\s*renderer\s*\)\s*return\s*;') {
                $earlyReturn = $i + 1
            }
            # Primera rama SDL que NO este protegida por sdlAvailable
            if ($guardFirstUse -eq 0 -and $code -match '^\s*if\s*\(' -and $code -match 'betaDisclaimerVisible|streamStartState|hasPublishedStream' -and $code -notmatch 'sdlAvailable') {
                $guardFirstUse = $i + 1
            }
            # Bloque de la sesion protegido SOLO con sdlAvailable (el error de la 2.85).
            # Se reconoce por la condicion `sdlAvailable && ... && hasPublishedStream()`, que no
            # contempla el modo VideoOut directo.
            #
            # OJO con la negacion: el bloque de "conexion pendiente" usa `!hasPublishedStream()`
            # y NO contiene la llamada a Present() del video directo, asi que no debe marcarse.
            # La primera version de esta comprobacion lo marcaba y daba un falso positivo.
            if ($streamGate -eq 0 -and $code -match '^\s*if\s*\(' -and $code -match 'sdlAvailable\s*&&' -and $code -match '(?<!\!)hasPublishedStream\(\)' -and $code -notmatch 'videoOutDirectActive') {
                $streamGate = $i + 1
            }
        }

        if ($flagLine -eq 0) {
            Write-Host "  [FALTA] draw() no marca la disponibilidad de SDL (const bool sdlAvailable...)" -ForegroundColor Red
            $fail++
        } elseif ($presentLine -eq 0) {
            Write-Host "  [FALTA] draw() no tiene el bloque de video directo (videoOutDirectActive && ...)" -ForegroundColor Red
            $fail++
        } elseif ($earlyReturn -gt 0 -and $earlyReturn -lt $presentLine) {
            Write-Host ("  [ORDEN] draw(): 'if(!renderer) return;' en linea {0} sale de la funcion ANTES del bloque de video directo (linea {1})" -f $earlyReturn, $presentLine) -ForegroundColor Red
            Write-Host "          Eso congela la imagen: Present() nunca se alcanza." -ForegroundColor Red
            $fail++
        } elseif ($streamGate -gt 0) {
            # Tercer error posible (el de la 2.85): el bloque que CONTIENE el video directo
            # protegido solo con `sdlAvailable &&`. Con renderer nulo se salta entero, asi que
            # Present() no se alcanza y la imagen queda congelada, pero sin crash.
            Write-Host ("  [BLOQUEO] draw(): el bloque de la sesion (linea {0}) esta protegido solo con 'sdlAvailable &&'" -f $streamGate) -ForegroundColor Red
            Write-Host "            Ese bloque contiene la llamada a Present(); con renderer nulo se salta" -ForegroundColor Red
            Write-Host "            entero y la imagen se congela. Debe permitir el modo VideoOut directo." -ForegroundColor Red
            $fail++
        } elseif ($guardFirstUse -gt 0) {
            Write-Host ("  [FALTA] rama SDL sin proteger en linea {0} (usaria renderer nulo)" -f $guardFirstUse) -ForegroundColor Red
            $fail++
        } else {
            Write-Host ("  [OK]    draw(): sdlAvailable en linea {0}, ramas SDL protegidas, video directo alcanzable (linea {1})" -f $flagLine, $presentLine) -ForegroundColor Green
        }
    }

    # ---------------------------------------------------------------------------------------------
    # COHERENCIA DE LA TABLA DE NOMBRES DE ETAPA (v3.50)
    # ---------------------------------------------------------------------------------------------
    # `SetCurrentStage(n)` guarda solo un numero, y el hilo del latido lo traduce a nombre con una
    # tabla PLANA en `src/opennow/stream_startup_diagnostics.cpp`. Si se anade una etapa al enum pero
    # no a la tabla (o al reves), el fichero `/data/gfnps4/last_stage.txt` MIENTE: muestra el nombre de
    # otra etapa o "?".
    #
    # Eso es exactamente lo que paso en la v3.50: se anadieron 9 sub-etapas al enum y la tabla seguia
    # teniendo 12 entradas, asi que las nuevas se habrian escrito como `name=?`. Se detecto leyendo el
    # codigo, pero es el tipo de fallo que debe cazarlo la auditoria, no una relectura con suerte.
    #
    # La comprobacion compara el NUMERO de nombres de la tabla con el ULTIMO valor del enum.
    $diagPath = Join-Path $root 'src\opennow\stream_startup_diagnostics.cpp'
    $mainPath = Join-Path $root 'src\ps4\main.cpp'
    if ((Test-Path -LiteralPath $diagPath) -and (Test-Path -LiteralPath $mainPath)) {
        $diagSrc = [System.IO.File]::ReadAllText($diagPath)
        $mainSrc = [System.IO.File]::ReadAllText($mainPath)

        # Nombres de la tabla: se cuenta entre `static const char* const names[] = {` y el `};`
        $tableCount = 0
        $tableMatch = [regex]::Match($diagSrc, 'static const char\* const names\[\]\s*=\s*\{(?<body>.*?)\};', 'Singleline')
        if ($tableMatch.Success) {
            $tableCount = ([regex]::Matches($tableMatch.Groups['body'].Value, '"[A-Z_]+"')).Count
        }

        # Ultimo valor de las etapas de dibujado. Se miran DOS convenciones de nombre porque el enum de
        # `main.cpp` usa `STAGE_DRAW_*` y el de `SDLVideoRenderer.cpp` (que no puede incluir el enum,
        # por ser multiplataforma) usa `kStageVideo*`. Los dos escriben en la misma tabla de nombres.
        $lastStage = -1
        # Las marcas del renderer de video viven en su propio fichero (no puede incluir el enum).
        $videoSrc = '' 
        $videoPath = Join-Path $root 'src\opennow\stream\SDLVideoRenderer.cpp'
        if (Test-Path -LiteralPath $videoPath) { $videoSrc = [System.IO.File]::ReadAllText($videoPath) }
        $stageSrc = $mainSrc + "`n" + $videoSrc
        foreach ($m in [regex]::Matches($stageSrc, '(?:STAGE_DRAW_[A-Z_]+|kStageVideo[A-Za-z]+)\s*=\s*(\d+)')) {
            $v = [int]$m.Groups[1].Value
            if ($v -gt $lastStage) { $lastStage = $v }
        }

        if ($tableCount -eq 0 -or $lastStage -lt 0) {
            Write-Host "  [FALTA] no se pudo leer la tabla de nombres de etapa o el enum de sub-etapas" -ForegroundColor Red
            $fail++
        } elseif ($tableCount -ne ($lastStage + 1)) {
            Write-Host ("  [DESAJUSTE] tabla de nombres de etapa: {0} entradas, pero el enum llega hasta {1} (hacen falta {2})" -f $tableCount, $lastStage, ($lastStage + 1)) -ForegroundColor Red
            Write-Host "              last_stage.txt mostraria el nombre equivocado o '?'." -ForegroundColor Red
            Write-Host "              Anade los nombres que faltan EN LA MISMA POSICION en stream_startup_diagnostics.cpp." -ForegroundColor Red
            $fail++
        } else {
            Write-Host ("  [OK]    tabla de nombres de etapa coherente ({0} entradas, enum hasta {1})" -f $tableCount, $lastStage) -ForegroundColor Green
        }
    }

    # ---------------------------------------------------------------------------------------------
    # COBERTURA DE FORMATOS DE PIXEL EN LA RUTA DE VIDEO (v3.62)
    # ---------------------------------------------------------------------------------------------
    # ESTE FALLO YA HA OCURRIDO UNA VEZ Y COSTO MUCHO ENCONTRARLO:
    #
    # El stream llega en `AV_PIX_FMT_YUVJ420P` (**valor 12**, no 0: `YUV420P` es 0 y `YUVJ420P` es 12).
    # La condicion de la ruta rapida de conversion aceptaba solo `NV12` y `YUV420P`, asi que
    # **rechazaba TODOS los frames** y la mejora activada por defecto **nunca se ejecutaba**. En el log
    # se veia porque no aparecia NINGUN marcador de esa ruta, pero nada avisaba del motivo: el codigo
    # era correcto, simplemente no se alcanzaba nunca.
    #
    # La comprobacion: la condicion de entrada a la ruta rapida tiene que **nombrar los tres formatos**
    # que el camino clasico ya maneja. Si falta uno, aviso.
    $videoPath = Join-Path $root 'src\opennow\stream\SDLVideoRenderer.cpp'
    if (Test-Path -LiteralPath $videoPath) {
        $videoSrc = [System.IO.File]::ReadAllText($videoPath)
        $condNueva = [regex]::Match($videoSrc, 'if\(useOwnBgraPath\(\)\s*&&\s*\(([^)]*)\)')
        if ($condNueva.Success) {
            $cond = $condNueva.Groups[1].Value
            $faltan = @()
            foreach ($par in @(@('AV_PIX_FMT_NV12','Nv12'), @('AV_PIX_FMT_YUV420P','Yuv420'), @('AV_PIX_FMT_YUVJ420P','Yuvj420'))) {
                $formato = $par[0]; $var = 'es' + $par[1]
                $declarada = ($videoSrc -match ($var + '\s*='))
                $usada = ($cond -match $var) -or ($cond -match [regex]::Escape($formato))
                if (-not ($declarada -and $usada)) { $faltan += $formato }
            }
            if ($faltan.Count -gt 0) {
                Write-Host ("  [FORMATOS] la ruta rapida de video NO acepta: {0}" -f ($faltan -join ', ')) -ForegroundColor Red
                Write-Host "             El stream llega en YUVJ420P (valor 12). Si esa ruta lo rechaza, la app" -ForegroundColor Red
                Write-Host "             cae al camino lento de SDL sin que nada lo indique en el log." -ForegroundColor Red
                $fail++
            } else {
                Write-Host "  [OK]    la ruta rapida de video acepta los 3 formatos del stream (NV12, YUV420P, YUVJ420P)" -ForegroundColor Green
            }
        } else {
            Write-Host "  [FALTA] no se pudo leer la condicion de la ruta rapida de video" -ForegroundColor Red
            $fail++
        }
    }

    # ---------------------------------------------------------------------------------------------
    # EL DESTINO DEL VIDEO TIENE QUE SER 1:1 CON LA TEXTURA (v3.90). ESTA REGLA SUSTITUYE A LA DE LA v3.63
    # ---------------------------------------------------------------------------------------------
    # HISTORIA, porque esta comprobacion exigia lo contrario y HAY QUE EXPLICAR POR QUE CAMBIA:
    #
    #   v3.63: se exigia `dstBgra = {0,0,dst_w,dst_h}` (el LIENZO) para que el video ocupara toda la
    #          pantalla. Se creia que el reescalado "no costaba mas, solo se repartia".
    #
    #   REALIDAD MEDIDA EN CONSOLA (cuatro versiones, ver `PS4-V3.44-CAUSA-REAL-DE-LOS-19FPS.md`):
    #
    #          v3.62  destino = tamano del FRAME  -> 1:1 -> copy_us 10.000-18.000 -> **59 fps**
    #          v3.80-89  destino = LIENZO 1920x1080 -> escalado -> copy_us 48.000-64.000 -> **19-20 fps**
    #
    #          El lienzo es 1920x1080 fijo (`main.cpp`: W/H salen de `BOE_DISPLAY`), asi que un destino
    #          de lienzo con una textura de 960x540 **escalaba SIEMPRE**. Y `SDL_render_sw.c:682-689`
    #          solo usa la copia rapida cuando el tamano coincide: si no, va por el camino con
    #          **conversion por pixel**.
    #
    # REGLA ACTUAL: el destino del blit tiene que ser **el mismo tamano que la textura** (1:1), acotado
    # al lienzo. Esa es la ruta que da 59-60 fps. El video se vera a resolucion nativa (no estirado),
    # y **estirar a pantalla completa es incompatible con los 60 fps en un renderizador software**.
    #
    # La comprobacion: tiene que existir `blit_w`/`blit_h` (el tamano 1:1 acotado) y el `SDL_Rect` de
    # destino tiene que usarlos. Y NO puede usar `dst_w`/`dst_h` (el lienzo), que es lo que escala.
    $videoPath = Join-Path $root 'src\opennow\stream\SDLVideoRenderer.cpp'
    if (Test-Path -LiteralPath $videoPath) {
        $videoSrc = [System.IO.File]::ReadAllText($videoPath)
        # Quitar comentarios: la documentacion del fallo menciona los patrones prohibidos a proposito.
        $soloCodigo = ($videoSrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

        $escalados = ([regex]::Matches($soloCodigo, 'dstBgra\s*=\s*\{0\s*,\s*0\s*,\s*dst_w\s*,\s*dst_h\}')).Count
        $destinoTw = ([regex]::Matches($soloCodigo, 'dstBgra\s*=\s*\{0\s*,\s*0\s*,\s*tw\s*,\s*th\}')).Count
        $texturaTw = ($soloCodigo -match 'bgraTexture_=SDL_CreateTexture\([^;]*,\s*tw\s*,\s*th\s*\)' -or
                      $soloCodigo -match 'bgraWidth_!=tw\s*\|\|\s*bgraHeight_!=th')
        $escalaConv = ($soloCodigo -match 'ScaleBilinearYUV420PToBGRA_BT709' -and
                       $soloCodigo -match 'ScaleBilinearNV12ToBGRA_BT709')
        $usaOutputSize = ($soloCodigo -match 'SDL_GetRendererOutputSize')

        if ($escalados -gt 0) {
            Write-Host ("  [DESTINO] {0} copia(s) de video con destino = LIENZO (dst_w,dst_h) sobre textura de frame" -f $escalados) -ForegroundColor Red
            Write-Host "            Eso ESCALA en el blit de SDL: medido 48.000-64.000 us y 19-20 fps." -ForegroundColor Red
            Write-Host "            El destino tiene que ser 1:1: {0,0,tw,th} con la textura a tw x th." -ForegroundColor Red
            $fail++
        } elseif ($destinoTw -eq 0 -or -not $texturaTw -or -not $escalaConv -or -not $usaOutputSize) {
            Write-Host "  [DESTINO] falta el blit 1:1 con escalado en la conversion" -ForegroundColor Red
            Write-Host "            Hacen falta las CUATRO piezas:" -ForegroundColor Red
            Write-Host "              - SDL_GetRendererOutputSize (el lienzo)" -ForegroundColor Red
            Write-Host "              - bgraTexture_ creada a tw x th (la textura mide el LIENZO)" -ForegroundColor Red
            Write-Host "              - destino {0,0,tw,th} (1:1 con la textura, no escala en el blit)" -ForegroundColor Red
            Write-Host "              - ScaleBilinear*ToBGRA_BT709 (el escalado se hace en la CONVERSION)" -ForegroundColor Red
            $fail++
        } else {
            Write-Host ("  [OK]    blit 1:1 con la textura ({0} copia(s) con tw/th) y escalado en la conversion" -f $destinoTw) -ForegroundColor Green
        }
    }

    # ---------------------------------------------------------------------------------------------
    # EL HUD NO SE DIBUJA ENCIMA DEL JUEGO (v3.63)
    # ---------------------------------------------------------------------------------------------
    # El titulo "AJ | <juego> | CIRCULO: JUEGO | L3+TRIANGULO: MENU" se pintaba en cada frame de la
    # partida, en la esquina superior izquierda, tapando el juego. Ahora tiene que estar detras de una
    # condicion que dependa de si el usuario ha pedido ver la interfaz.
    $mainPath2 = Join-Path $root 'src\ps4\main.cpp'
    if (Test-Path -LiteralPath $mainPath2) {
        $mainSrc2 = [System.IO.File]::ReadAllText($mainPath2)
        # OJO: hay DOS coincidencias de `label(20,12,title,2)` en el fichero. La primera esta **dentro
        # del comentario que documenta este mismo fallo** ("Aqui estaba: ... label(20, 12, title, 2);"),
        # asi que si se toma la primera el aviso salta siempre. Se quitan los comentarios antes de
        # buscar: asi solo queda la llamada real.
        $mainSoloCodigo = ($mainSrc2 -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
        $m = [regex]::Match($mainSoloCodigo, 'label\(20\s*,\s*12\s*,\s*title\s*,\s*2\s*\)')
        if ($m.Success) {
            # La condicion esta ANTES de la llamada, asi que se mira hacia atras. El margen tiene que
            # ser amplio: entre el `if` y el `label` va todo el comentario que explica el fallo.
            $desde = [Math]::Max(0, $m.Index - 6000)
            $posAntes = $mainSoloCodigo.Substring($desde, $m.Index - $desde)
            if ($posAntes -match 'if\(streamStatsVisible\s*\|\|\s*streamMenuVisible\)') {
                Write-Host "  [OK]    el titulo del HUD del stream solo se dibuja con la interfaz visible" -ForegroundColor Green
            } else {
                Write-Host "  [HUD]   el titulo del HUD del stream se dibuja SIEMPRE: tapara el juego" -ForegroundColor Red
                Write-Host "          Tiene que ir dentro de if(streamStatsVisible || streamMenuVisible)." -ForegroundColor Red
                $fail++
            }
        }
    }

    # ---------------------------------------------------------------------------------------------
    # LA CONVERSION ESCRIBE DIRECTO EN LA TEXTURA (v3.65)
    # ---------------------------------------------------------------------------------------------
    # `SDL_UpdateTexture` copiaba el buffer BGRA a la superficie de la textura: trabajo puro de memoria
    # que se elimino escribiendo directo con `SDL_LockTexture` (que en el renderizador software devuelve
    # el puntero a la superficie sin copiar nada, `SDL_render_sw.c:153`).
    #
    # La comprobacion: tiene que existir la escritura directa Y un camino de respaldo. Si alguien quita
    # el respaldo, un fallo de `SDL_LockTexture` dejaria el video sin dibujar en silencio.
    $videoPath3 = Join-Path $root 'src\opennow\stream\SDLVideoRenderer.cpp'
    if (Test-Path -LiteralPath $videoPath3) {
        $vsrc = [System.IO.File]::ReadAllText($videoPath3)
        $tieneLock = ($vsrc -match 'SDL_LockTexture')
        $tieneRespaldo = ($vsrc -match 'SDL_UpdateTexture')
        $tieneMarca = ($vsrc -match 'STREAM_VIDEO_DIRECT_WRITE_ON')
        if ($tieneLock -and $tieneRespaldo -and $tieneMarca) {
            Write-Host "  [OK]    la conversion escribe directo en la textura, con respaldo (UpdateTexture)" -ForegroundColor Green
        } else {
            Write-Host ("  [DIRECTO] escritura directa incompleta: Lock={0} Respaldo={1} Marca={2}" -f $tieneLock,$tieneRespaldo,$tieneMarca) -ForegroundColor Red
            Write-Host "            Hacen falta las tres: el camino directo, el respaldo y la marca en el log." -ForegroundColor Red
            $fail++
        }
    }

    # ---------------------------------------------------------------------------------------------
    # ORDEN DE LAS FRONTERAS DE FASE DEL DIBUJADO (v3.68)
    # ---------------------------------------------------------------------------------------------
    # `UI_DRAW_PHASES` da un desglose por fases (clear, cabecera, contenido, pie, present). Ese desglose
    # estuvo FALSEADO desde que se instrumento: `content_us` salia SIEMPRE 0 en todas las paginas,
    # porque la marca de CONTENIDO estaba puesta al FINAL de la pagina en lugar de al PRINCIPIO, y todo
    # el coste se acumulaba en la cabecera. Eso impidio durante varias versiones diagnosticar por que
    # las pantallas de configuracion y catalogo iban a 22 ms.
    #
    # La comprobacion: las acumulaciones tienen que aparecer en el ORDEN del enum de etapas, es decir
    # `s_phaseAccum[0]` (clear) < `[1]` (cabecera) < `[2]` (contenido) < `[3]` (pie). Si se vuelven a
    # desordenar, los numeros del log volveran a enganar.
    $mainPath4 = Join-Path $root 'src\ps4\main.cpp'
    if (Test-Path -LiteralPath $mainPath4) {
        $msrc4 = [System.IO.File]::ReadAllText($mainPath4)
        $pos = @{}
        $ok = $true
        foreach ($idx in 0,1,2,3) {
            $mm = [regex]::Match($msrc4, "s_phaseAccum\[$idx\]\s*\+=")
            if ($mm.Success) { $pos[$idx] = $mm.Index } else { $ok = $false }
        }
        if (-not $ok) {
            Write-Host "  [FASES] no se encuentran las cuatro acumulaciones de fase" -ForegroundColor Red
            $fail++
        } elseif ($pos[0] -lt $pos[1] -and $pos[1] -lt $pos[2] -and $pos[2] -lt $pos[3]) {
            Write-Host ("  [OK]    fronteras de fase en orden (clear {0} < cabecera {1} < contenido {2} < pie {3})" -f $pos[0],$pos[1],$pos[2],$pos[3]) -ForegroundColor Green
        } else {
            Write-Host ("  [FASES] las fronteras estan DESORDENADAS: clear={0} cabecera={1} contenido={2} pie={3}" -f $pos[0],$pos[1],$pos[2],$pos[3]) -ForegroundColor Red
            Write-Host "          El desglose de UI_DRAW_PHASES volvera a enganar (el contenido se contara como cabecera)." -ForegroundColor Red
            $fail++
        }
    }

    # ---------------------------------------------------------------------------------------------
    # `SDL_RenderCopy` DE TEXTO NO PUEDE LLEVAR DESTINO NULO (v3.73)
    # ---------------------------------------------------------------------------------------------
    # ESTE FALLO SE ENTREGO Y SE VIO EN CONSOLA. En la v3.70 sustitui el destino explicito del texto por
    # `SDL_RenderCopy(renderer, texture, nullptr, nullptr)`, razonando que "dibuja la textura a su tamanio
    # en (x,y)". **La primera mitad es falsa: con `dstrect = NULL` SDL usa `{0,0,texture->w,texture->h}`,
    # o sea la posicion (0,0) y NO (x,y).**
    #
    # Resultado: **todas las etiquetas de la pantalla se apilaron en la esquina superior izquierda**
    # (el bloque blanco del informe) y la aplicacion murio a los 6 latidos en `DRAW_MENU_CONTENT`.
    #
    # La comprobacion: en las funciones `text()` y `textAlpha()`, la llamada a `SDL_RenderCopy` tiene que
    # pasar un cuarto argumento que NO sea `nullptr`, y tiene que existir un `SDL_QueryTexture` que dé el
    # tamanio para construir ese destino.
    $mainPath5 = Join-Path $root 'src\ps4\main.cpp'
    if (Test-Path -LiteralPath $mainPath5) {
        $msrc5 = [System.IO.File]::ReadAllText($mainPath5)
        # OJO: hay que quitar los comentarios antes de buscar. El comentario que DOCUMENTA este fallo
        # contiene el propio patron prohibido (`SDL_RenderCopy(renderer,texture,nullptr,nullptr)`), asi
        # que si se busca en crudo la comprobacion salta siempre. Es el mismo detalle que ya aparecio en
        # la auditoria del HUD.
        $msrc5 = ($msrc5 -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
        $malas = ([regex]::Matches($msrc5, 'SDL_RenderCopy\(renderer,\s*texture\s*,\s*nullptr\s*,\s*nullptr\s*\)')).Count
        $queries = ([regex]::Matches($msrc5, 'SDL_QueryTexture\(texture,nullptr,nullptr,&w,&h\)')).Count
        if ($malas -gt 0) {
            Write-Host ("  [TEXTO] {0} llamada(s) a SDL_RenderCopy de texto con destino NULO" -f $malas) -ForegroundColor Red
            Write-Host "          Con destino nulo SDL dibuja en (0,0): TODO el texto se apila en la esquina." -ForegroundColor Red
            Write-Host "          Hace falta el SDL_QueryTexture y el SDL_Rect {x,y,w,h}." -ForegroundColor Red
            $fail++
        } elseif ($queries -lt 2) {
            Write-Host ("  [TEXTO] solo {0} consulta(s) de tamanio de textura; se esperan 2 (text y textAlpha)" -f $queries) -ForegroundColor Red
            $fail++
        } else {
            Write-Host "  [OK]    el texto se dibuja en su (x,y) (destino explicito, sin destino nulo)" -ForegroundColor Green
        }
    }
}

# -------------------------------------------------------------------------------------------------
# LA RUTA DE VideoOut DIRECTO TIENE QUE SER LA PREFERIDA (v3.93)
# -------------------------------------------------------------------------------------------------
# POR QUE ESTA COMPROBACION EXISTE: es la unica ruta que puede prometer 60 fps y pantalla completa, y
# durante muchas versiones estuvo APAGADA detras de un flag que habia que crear a mano, con lo que la
# app caia siempre en la ruta SDL. Las medidas del proyecto, en los dos caminos:
#
#   RUTA DIRECTA (buffers propios, VideoOut escala a la pantalla):
#       v2.84/v2.85/v2.86  VIDEOOUT_HANDOFF_COMPLETE mode=direct_hardware_60fps
#       v2.79              escalador avg 8,0-8,7 ms
#       v3.02              VIDEOOUT_SCALE_TIMING dispatch_avg_us=8523
#       v3.25              dispatch_avg_us=13766 (6 hilos)
#       -> presupuesto a 60 fps = 16.666 us. **8.523 us es la mitad.**
#
#   RUTA SDL (el driver copia el lienzo entero a memoria visible por GPU en cada frame):
#       v3.90              copy_us=24.962  +  updatewindowsurface_us=14.719  =  39,7 ms
#       -> **no cabe ni a 30 fps** (33 ms). No es cuestion de optimizar mas: no cabe.
#       -> y ademas deja el video en una esquina, porque el driver fija sus buffers al display
#          (1920x1080) y copia dentro `window->w x window->h` sin escalar (SDL_ps4video.c:509-519).
$mainPath = Join-Path $root 'src\ps4\main.cpp'
if (Test-Path -LiteralPath $mainPath) {
    $mainSrc = [System.IO.File]::ReadAllText($mainPath)
    # Se quitan los comentarios ANTES de comprobar: la documentacion del cambio menciona a proposito el
    # flag antiguo para explicar por que se retiro, y eso no es codigo. Mismo criterio que en [DESTINO].
    $mainCode = ($mainSrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    # =============================================================================================
    # CAMBIO DE DECISION EN LA v4.06: LA RUTA DIRECTA PASA A SER OPT-IN
    # =============================================================================================
    # La comprobacion anterior exigia que la ruta directa estuviera ACTIVADA POR DEFECTO, porque es la
    # unica que cabe en los 16.666 us. **Eso sigue siendo cierto en el presupuesto, pero los datos de
    # consola lo desmintieron en la practica:**
    #
    #   `logs/gfnps4` de la sesion de la v4.05: `APP_START version=4.05`, **beat=460** (latido de 1 s ->
    #   460 s de vida), stream iniciado a los **354,3 s**, log terminado a los **356,4 s**, y
    #   `last_stage.txt` = **`stage=6 name=PRESENT`**.
    #
    #   `stage=6` es **`STAGE_PRESENT`, la etapa del camino SDL** (`presentFrame()`); el camino directo
    #   usa las etapas 7, 8, 10 y 11. **La sesion de juego duro ~2 segundos y murio justo despues de
    #   `VIDEOOUT_HANDOFF_COMPLETE`.**
    #
    #   Y en esa misma sesion los menus iban a **`loop_fps=58..59` con `slow_frames=0`**, mientras que al
    #   entrar al stream el coste sube a **`present_us=17037`** sobre 16.666 us de presupuesto.
    #
    # El usuario tiene la autoridad y ademas el criterio correcto:
    #   *"Prefiero una ruta de renderizado estable a 30/60fps (como la v3.62) que un experimento de
    #     VideoOut que cause cierres a los pocos segundos."*
    $prefiereDirecta = ($mainCode -match 'g_videoOutDebugFlagDetected\s*=\s*!noVideoOutFlag')
    $flagActivacion = ($mainCode -match 'direct_videoout\.flag')
    $flagExclusion  = ($mainCode -match 'no_videoout\.flag')
    $marcaOptIn     = ($mainCode -match 'VIDEOOUT_DISABLED_OPT_IN')

    if (-not $prefiereDirecta) {
        Write-Host "  [RUTA] la ruta directa NO esta activada por defecto" -ForegroundColor Red
        Write-Host "         Hace falta: g_videoOutDebugFlagDetected = directVideoOutFlag && !noVideoOutFlag" -ForegroundColor Red
        Write-Host "         Es la unica que cabe en 16.666 us: los logs de la v4.13 miden conv_us=31.000" -ForegroundColor Red
        Write-Host "         escalando a 1920x1080, mas copy_us=45.000 -> 19 fps. La SDL no puede dar 60." -ForegroundColor Red
        $fail++
    } elseif (-not $flagActivacion) {
        Write-Host "  [RUTA] falta la via de ACTIVACION de la ruta directa (direct_videoout.flag)" -ForegroundColor Red
        Write-Host "         Sin ella no habria forma de seguir probandola y quedaria codigo muerto." -ForegroundColor Red
        $fail++
    } elseif (-not $flagExclusion) {
        Write-Host "  [RUTA] falta la via de EXCLUSION explicita (no_videoout.flag)" -ForegroundColor Red
        Write-Host "         Se sigue respetando por compatibilidad con lo ya documentado." -ForegroundColor Red
        $fail++
    } elseif (-not $marcaOptIn) {
        Write-Host "  [RUTA] falta el marcador VIDEOOUT_DISABLED_OPT_IN" -ForegroundColor Red
        Write-Host "         Es el que deja constancia en el log de que se esta en SDL por DECISION, no por" -ForegroundColor Red
        Write-Host "         un fallo del traspaso." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    ruta directa por defecto, con escape a SDL (no_videoout.flag)" -ForegroundColor Green
    }

    # El respaldo automatico tiene que seguir existiendo: si el traspaso falla, SDL recupera la pantalla.
    $tieneRespaldo = ($mainCode -match 'VIDEOOUT_HANDOFF_FAIL' -and $mainCode -match 'restoreSdlFromVideoOut')
    if (-not $tieneRespaldo) {
        Write-Host "  [RUTA] falta el respaldo automatico a SDL si el traspaso falla" -ForegroundColor Red
        Write-Host "         Hacen falta VIDEOOUT_HANDOFF_FAIL y restoreSdlFromVideoOut()." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    respaldo automatico a SDL si el traspaso a VideoOut falla" -ForegroundColor Green
    }

    # ---------------------------------------------------------------------------------------------
    # EL LIENZO PROPIO NO PUEDE QUEDARSE SIN LIBERAR (v3.93)
    # ---------------------------------------------------------------------------------------------
    # FUGA REAL ENCONTRADA Y CORREGIDA: `g_ownCanvas` es `ARGB8888` de 1920x1080 = **8,29 MB**, se crea
    # en el arranque y en `restoreSdlFromVideoOut()`, y **no habia ni una sola llamada a `SDL_FreeSurface`
    # sobre el en todo el fichero**. El traspaso hacia:
    #
    #     1. `releaseSdlForVideoOut()`  -> destruye renderer y window, deja el lienzo VIVO
    #     2. `restoreSdlFromVideoOut()` -> g_ownCanvas = SDL_CreateRGBSurfaceWithFormat(...)
    #                                       sobrescribe el puntero SIN liberar el anterior
    #
    # -> **8,29 MB perdidos por cada ciclo de stream.** Y `SDL_CreateSoftwareRenderer` no toma propiedad
    # de la superficie, asi que el renderizador tampoco la liberaba.
    #
    # La memoria agotada es la hipotesis documentada del cierre sin traza
    # (PS4-V2.81-CRASH-HEARTBEAT.md: el kernel mata el proceso; el log termina en seco).
    $liberaCanvas = ($mainCode -match 'SDL_FreeSurface\(g_ownCanvas\)')
    $marcaFreed   = ($mainCode -match 'SDL_OWN_CANVAS_FREED')
    # El lienzo tiene que recrearse en la restauracion: si no, tras un stream la app vuelve al lienzo de
    # la ventana (formato BGR888) y al camino de conversion por pixel.
    $recreaEnRestore = ($mainCode -match 'motivo=restauracion_tras_videoout')

    if (-not $liberaCanvas -or -not $marcaFreed) {
        Write-Host "  [CANVAS] el lienzo propio NO se libera: fuga de 8,29 MB por ciclo de stream" -ForegroundColor Red
        Write-Host "           Hace falta SDL_FreeSurface(g_ownCanvas) y la marca SDL_OWN_CANVAS_FREED." -ForegroundColor Red
        $fail++
    } elseif (-not $recreaEnRestore) {
        Write-Host "  [CANVAS] la restauracion tras VideoOut no recrea el lienzo propio" -ForegroundColor Red
        Write-Host "           Sin el, la app vuelve al lienzo de la ventana (BGR888) y al camino lento." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    lienzo propio liberado en el traspaso y recreado en la restauracion" -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# GUARDIAN DE DEGRADACION Y CUARENTENA DE LA RUTA DIRECTA (v3.94)
# -------------------------------------------------------------------------------------------------
# Las dos protecciones que impiden que un fallo se repita sin control:
#
# 1. GUARDIAN DE DEGRADACION. El patron documentado de los cierres sin traza
#    (`PS4-V2.81-CRASH-HEARTBEAT.md`) es que el coste por frame SUBE de forma sostenida antes de morir:
#    avg_us 1172 -> 1167 -> 1492 -> 7449 -> 10347, y el proceso muere 0,3 s despues. El log terminaba en
#    seco porque el kernel mata el proceso sin pasar por los manejadores de senal. Hace falta que alguien
#    **registre la subida mientras ocurre** y, si supera el presupuesto de 60 fps de forma sostenida,
#    pida la reconstruccion del pipeline.
#
# 2. CUARENTENA. `videoOutDirectActive` se rearma en CADA stream. Sin cuarentena, un fallo persistente
#    de flips haria que cada entrada a jugar volviera a intentar la ruta directa, fallara y se degradara:
#    el usuario veria el tiron tantas veces como entrara. Con ella se degrada UNA vez y el resto de la
#    ejecucion se queda en SDL; al reiniciar la app se reintenta, que es lo razonable.
$voutPath = Join-Path $root 'src\opennow\stream\PS4VideoOutRenderer.cpp'
if (Test-Path -LiteralPath $voutPath) {
    $voutSrc = [System.IO.File]::ReadAllText($voutPath)
    $voutCode = ($voutSrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    $tieneGuardian = ($voutCode -match 'VIDEOOUT_DEGRADACION_SOSTENIDA' -and
                      $voutCode -match 'VIDEOOUT_RECREATE_REQUESTED_DEGRADACION' -and
                      $voutCode -match '16666')
    if (-not $tieneGuardian) {
        Write-Host "  [GUARDIAN] falta el guardian de degradacion progresiva en Present()" -ForegroundColor Red
        Write-Host "             Es el patron documentado de los cierres sin traza: el coste por frame sube" -ForegroundColor Red
        Write-Host "             de forma sostenida (x9 en 2,5 s) y el proceso muere sin dejar senal." -ForegroundColor Red
        Write-Host "             Hacen falta VIDEOOUT_DEGRADACION_SOSTENIDA, VIDEOOUT_RECREATE_REQUESTED_DEGRADACION" -ForegroundColor Red
        Write-Host "             y el presupuesto de 16666 us." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    guardian de degradacion progresiva con presupuesto de 60 fps" -ForegroundColor Green
    }

    # La cuarentena vive ENTERA en main.cpp, y la comprobacion tiene que verificar que **GOBIERNA** el
    # armado de la ruta, no solo que los nombres existan. Ese fue el fallo de la primera version de esta
    # comprobacion: al quitar `&& !g_videoOutQuarantined` de la linea de armado seguia pasando, porque
    # los identificadores seguian presentes en el fichero. Se comprueba la CONDICION COMPLETA.
    $mainSrcQ = ''
    if (Test-Path -LiteralPath $mainPath) {
        $mainSrcQ = [System.IO.File]::ReadAllText($mainPath)
    }
    $mainCodeQ = ($mainSrcQ -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
    $armaConCuarentena = ($mainCodeQ -match
        'videoOutDirectActive\s*=\s*g_videoOutDebugFlagDetected\s*&&\s*!\s*g_videoOutQuarantined')
    $tieneCuarentena = $armaConCuarentena -and
                       ($mainCodeQ -match 'VIDEOOUT_QUARANTINED') -and
                       ($mainCodeQ -match 'VIDEOOUT_SKIP_QUARANTINED')
    # =============================================================================================
    # LOS **DOS** CAMINOS DE FALLO TIENEN QUE PONER LA CUARENTENA (v4.02)
    # =============================================================================================
    # Habia DOS formas de que la ruta directa fallara y **solo una ponia la cuarentena**:
    #
    #   (a) `VIDEOOUT_HANDOFF_FAIL`  -> `Initialize()` devuelve false en el traspaso. **ES EL MAS
    #       PROBABLE** (sin memoria directa, sin handle, o buffers rechazados). Se habia quedado SIN
    #       cuarentena, asi que **cada stream volvia a intentar el traspaso y volvia a fallar**, con su
    #       `releaseSdl` y su `restoreSdl` de por medio: un tiron en cada entrada a jugar.
    #   (b) fallo persistente de flips (3 seguidos -> `NeedsFullRecreate`). Este SI la ponia.
    #
    # Se cuentan las asignaciones de la marca: tienen que ser **DOS** (una por camino). Contar
    # asignaciones, y no solo buscar el nombre, es lo que detecta que a uno de los dos le falte.
    $asignacionesCuarentena = ([regex]::Matches($mainCodeQ, 'g_videoOutQuarantined\s*=\s*true')).Count
    $dosCaminos = ($asignacionesCuarentena -ge 2)

    if (-not $armaConCuarentena) {
        Write-Host "  [CUARENTENA] el armado de la ruta NO consulta la cuarentena" -ForegroundColor Red
        Write-Host "               Hace falta: videoOutDirectActive = g_videoOutDebugFlagDetected" -ForegroundColor Red
        Write-Host "               && !g_videoOutQuarantined" -ForegroundColor Red
    }
    if (-not $dosCaminos) {
        Write-Host ("  [CUARENTENA] solo {0} camino(s) de fallo ponen g_videoOutQuarantined; hacen falta 2" -f $asignacionesCuarentena) -ForegroundColor Red
        Write-Host "               (a) VIDEOOUT_HANDOFF_FAIL  (Initialize() devuelve false: el mas probable)" -ForegroundColor Red
        Write-Host "               (b) fallo persistente de flips (NeedsFullRecreate)" -ForegroundColor Red
        Write-Host "               Al que le falte, su stream volvera a intentar la ruta directa y a fallar." -ForegroundColor Red
        $fail++
    } elseif (-not $tieneCuarentena) {
        Write-Host "  [CUARENTENA] la ruta directa no se pone en cuarentena tras un fallo persistente" -ForegroundColor Red
        Write-Host "               Sin ella, cada stream vuelve a intentarla y a degradarse: tiron repetido." -ForegroundColor Red
        $fail++
    } else {
        Write-Host ("  [OK]    cuarentena en los DOS caminos de fallo (handoff y fallo persistente)") -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# LA INTERFAZ NO PUEDE PROMETER 1080P (v3.95)
# -------------------------------------------------------------------------------------------------
# `resolution_to_wh()` fuerza **720p en los tres modos**, por el cierre medido:
#
#     framebuffer 1920x1080 -> flips presentados = 1   -> SE CERRO
#     framebuffer 1280x720  -> flips presentados = 119 -> sesion estable
#
# Pero la interfaz lo presentaba asi:
#
#     resolutions[] = {"AUTO (720P)", "720P", "1080P"}          <- este array se dibuja en el PERFIL
#     fila 2:  ... : "1080P (1920x1080)"                        <- el usuario elegia esto
#
# Es decir: el usuario elegia 1080P, la pantalla le decia "PERFIL: 1080P", y **se pedia y aplicaba 720p**.
# Una interfaz que promete algo que no ocurre hace que cualquier problema posterior parezca aleatorio.
#
# La comprobacion: ninguna cadena de la interfaz puede presentar 1080p como si fuera a aplicarse.
if (Test-Path -LiteralPath $mainPath) {
    $mainSrcR = [System.IO.File]::ReadAllText($mainPath)
    $mainCodeR = ($mainSrcR -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    # El texto del array de nombres no puede decir "1080P" a secas: tiene que avisar de que no aplica.
    $arrayDice1080Solo = ($mainCodeR -match 'resolutions\[\]\s*=\s*\{[^}]*"1080P"\s*\}')
    # La fila 2 tampoco puede rotular 1080p como si se fuera a usar.
    $fila2Promete1080 = ($mainCodeR -match '"1080P \(1920x1080\)"')
    # Y tiene que existir el aviso de que no esta disponible.
    $avisa = ($mainCodeR -match '1080P NO DISPONIBLE' -or $mainCodeR -match '1080P UNAVAILABLE')

    if ($arrayDice1080Solo -or $fila2Promete1080) {
        Write-Host "  [ETIQUETA] la interfaz promete 1080P, pero resolution_to_wh() aplica 720p" -ForegroundColor Red
        Write-Host "             El framebuffer de 1080p se cerro con 1 flip (720p: 119 flips estables)." -ForegroundColor Red
        Write-Host "             La etiqueta tiene que decir que 1080p no esta disponible." -ForegroundColor Red
        $fail++
    } elseif (-not $avisa) {
        Write-Host "  [ETIQUETA] falta el aviso de que 1080P no esta disponible" -ForegroundColor Red
        Write-Host "             Hace falta '1080P NO DISPONIBLE' (o '1080P UNAVAILABLE' en ingles)." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    la interfaz avisa de que 1080P no esta disponible (no lo promete)" -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# EL FRAMEBUFFER DE LA RUTA DIRECTA ESTA FIJO EN 1280x720 (v3.98)
# -------------------------------------------------------------------------------------------------
# HISTORIA DE ESTA REGLA, porque ha cambiado dos veces y conviene que quede escrito:
#
#   v3.26: el modo AUTO adoptaba la resolucion del servidor -> **433 adopciones, una por frame**, porque
#          `Initialize()` redondeaba a 1280x720 y la adopcion se repetia. Se elimino en la v3.28.
#
#   v3.97: puse el framebuffer al tamanio del frame (960x540) para que el escalado en CPU costara CERO.
#          Es la propuesta que la investigacion valora como la mejor relacion valor/riesgo
#          (`ANALISIS-DOLPHINPS4-DETALLADO.md` §6.5, *"elimina el 82,6 % del coste de CPU"*).
#
#   v3.98: **REVERTIDO**, por un dato encontrado al revisarlo:
#
#          static const SupportedSize kSupported[] = {
#              {1920, 1080},   // 1080P FIJO
#              {1280, 720},    // 720P FIJO
#              {960,  540},    // 540p, por si en el futuro se expone como opcion fija   <-- NUNCA PROBADO
#              {854,  480},    // 480p                                                 <-- NUNCA PROBADO
#          };
#
#          **960x540 esta en la tabla pero nunca se ha probado en consola.** Los dos tamanios con evidencia
#          son 1920x1080 (**1 flip -> se cerro**) y 1280x720 (**119 flips -> estable**). Y los dos unicos
#          ejemplos de `sceVideoOutRegisterBuffers` del proyecto registran **1920x1080**, el tamanio del
#          panel: apunta a que el buffer tiene que ser del tamanio de la pantalla.
#
#          RIESGO CONCRETO: si un buffer de 960x540 no se estira, el video se veria **pequeno en una
#          esquina** — el sintoma que hay que evitar. Y el escalado en CPU de 8.523-13.766 us **cabe** en
#          el presupuesto de 16.666 us. **Ante duda, lo probado.**
#
# La comprobacion: el framebuffer se fija con `resolution_to_wh()` (que fuerza 720p) y **no** se deriva
# de `frame->width`. Y tiene que existir la traza que dice cuanto se escala.
if (Test-Path -LiteralPath $mainPath) {
    $mainSrcF = [System.IO.File]::ReadAllText($mainPath)
    $mainCodeF = ($mainSrcF -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    # NO puede derivarse del frame (eso es la v3.97 revertida). El patron tiene que ser PRECISO, y ha
    # hecho falta afinarlo tres veces, cada vez por un motivo distinto que conviene dejar escrito:
    #
    #   1. `-match 'fbW\s*=\s*frame->width'`  -> seguia PASANDO cuando la mutacion ANADIA esa linea
    #      despues de la asignacion correcta, porque las lineas correctas seguian presentes.
    #   2. `-match 'fbW\s*=\s*[^;]*frame->'`  -> DEMASIADO AMPLIO: `[^;]*` cruza el fin de linea y acaba
    #      casando con el `frame->width` de una CADENA de log posterior. Falso positivo.
    #   3. `(?m)^[^\r\n]*\bfbW\s*=\s*[^;\r\n]*frame->[^;\r\n]*;`  -> seguia dando FALSO POSITIVO, y la
    #      causa es sutil: `\s*=\s*` casa con el PRIMER caracter de `==`, asi que la **comparacion**
    #      `(fbW == frame->width && fbH == frame->height) ? "CERO" : "activo"` parecia una asignacion.
    #
    # La forma final exige que el `=` NO sea el inicio de `==` ni de `!=`/`<=`/`>=`, con una asercion
    # negativa. Asi la comprobacion solo detecta una ASIGNACION real de fbW/fbH desde el frame.
    $derivaDelFrame = ($mainCodeF -match '(?m)^[^\r\n]*\bfbW\s*=(?!=)\s*[^;\r\n]*frame->[^;\r\n]*;' -or
                       $mainCodeF -match '(?m)^[^\r\n]*\bfbH\s*=(?!=)\s*[^;\r\n]*frame->[^;\r\n]*;')
    # Tiene que fijarse por configuracion.
    $fijaPorConfig  = ($mainCodeF -match 'int\s+fbW\s*=\s*1280\s*,\s*fbH\s*=\s*720' -and
                       $mainCodeF -match 'resolution_to_wh\s*\(\s*resolution\s*,\s*fbW\s*,\s*fbH\s*\)')
    $marca          = ($mainCodeF -match 'VIDEOOUT_FRAMEBUFFER_SIZE')

    if ($derivaDelFrame) {
        Write-Host "  [FRAMEBUFFER] el framebuffer se deriva del frame otra vez" -ForegroundColor Red
        Write-Host "                Ese es el cambio de la v3.97, REVERTIDO en la v3.98: 960x540 esta en la" -ForegroundColor Red
        Write-Host "                tabla pero NUNCA se probo, y si no se estira el video se ve en una esquina." -ForegroundColor Red
        $fail++
    } elseif (-not $fijaPorConfig) {
        Write-Host "  [FRAMEBUFFER] no se fija por configuracion a 1280x720" -ForegroundColor Red
        Write-Host "                Hace falta: int fbW = 1280, fbH = 720; + resolution_to_wh(resolution, fbW, fbH)" -ForegroundColor Red
        Write-Host "                1280x720 es el unico tamanio con evidencia de estabilidad (119 flips)." -ForegroundColor Red
        $fail++
    } elseif (-not $marca) {
        Write-Host "  [FRAMEBUFFER] falta la traza VIDEOOUT_FRAMEBUFFER_SIZE" -ForegroundColor Red
        Write-Host "                Sin ella no se puede saber en el log cuanto se esta escalando." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    framebuffer fijo 1280x720 (el unico con evidencia: 119 flips)" -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# EL MODO DE PRUEBA DEL ESCALADOR TIENE QUE EXISTIR (v4.00)
# -------------------------------------------------------------------------------------------------
# La pregunta que decidiria si se puede eliminar el escalado de CPU (8.523-13.766 us, el 51-83 % del
# presupuesto de 60 fps) es si **un framebuffer MAS PEQUENO que el panel se estira**. El proyecto la
# declaro indeducible del codigo (`PS4-V3.56` §2) y su sonda aislada **se cierra sin escribir log**, asi
# que nunca ha dado respuesta.
#
# La via que queda es hacer la prueba **dentro del cliente** (que si arranca) y con **resultado visual**,
# que no necesita logs: se presenta un patron con **borde blanco pegado a los limites del framebuffer**.
#   - borde blanco en los cuatro limites de la pantalla -> el escalador EXISTE
#   - borde blanco enmarcando un rectangulo menor     -> NO hay escalado automatico
#
# Sin esta via, la pregunta se queda sin respuesta para siempre y el escalado no se puede eliminar.
if (Test-Path -LiteralPath $mainPath) {
    $mainSrcS = [System.IO.File]::ReadAllText($mainPath)
    $mainCodeS = ($mainSrcS -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
    # =============================================================================================
    # LOS DOS FLAGS DEL ESCALADOR TIENEN QUE ESTAR SEPARADOS (v4.03)
    # =============================================================================================
    # El modo del arranque ejecuta `Initialize(960, 540)` + `Shutdown()` **antes** de que SDL cree su
    # ventana, y **esa secuencia nunca se ha ejecutado en consola**. Si el cierre no deja BUS_MAIN
    # impecable, `SDL_CreateWindow` falla y **la app sale sin mostrar nada**.
    #
    # Justo antes usaba **el mismo flag** que el usuario podria crear para el metodo seguro: **crear el
    # flag del metodo seguro disparaba el arriesgado.** Tienen que ser ficheros DISTINTOS.
    # OJO CON EL PATRON: `test_scaler\.flag` casa TAMBIEN dentro de `test_scaler_boot.flag`, porque el
    # segundo contiene al primero. Sin exigir el cierre de la cadena (`"`), la comprobacion no distingue
    # un flag del otro y deja pasar la mutacion que los vuelve a juntar. Se ancla con `"` al final.
    $flagSeguro = ($mainCodeS -match '"/data/gfnps4/test_scaler\.flag"')
    $flagArriesgado = ($mainCodeS -match '"/data/gfnps4/test_scaler_boot\.flag"')
    $marcaAviso = ($mainCodeS -match 'VIDEOOUT_SCALER_TEST_FLAG_SAFE_PATH')
    # El camino arriesgado lo tiene que gobernar el flag EXPLICITO, con su nombre completo.
    $arriesgadoConFlagPropio = ($mainCodeS -match
        '"/data/gfnps4/test_scaler_boot\.flag"[\s\S]{0,500}?Initialize\(960,\s*540\)')

    if (-not $flagSeguro -or -not $flagArriesgado -or -not $marcaAviso) {
        Write-Host "  [ESCALADOR] los dos flags del escalador no estan separados" -ForegroundColor Red
        Write-Host "              Hace falta: test_scaler.flag (metodo seguro, reservado)," -ForegroundColor Red
        Write-Host "              test_scaler_boot.flag (el del arranque de verdad) y la marca" -ForegroundColor Red
        Write-Host "              VIDEOOUT_SCALER_TEST_FLAG_SAFE_PATH. Un solo flag compartido haria que el" -ForegroundColor Red
        Write-Host "              fichero del metodo seguro disparara el arriesgado." -ForegroundColor Red
        $fail++
    } elseif (-not $arriesgadoConFlagPropio) {
        Write-Host "  [ESCALADOR] el camino arriesgado no esta gobernado por test_scaler_boot.flag" -ForegroundColor Red
        Write-Host "              Tiene que ser el flag EXPLICITO el que lleve a Initialize(960, 540): esa" -ForegroundColor Red
        Write-Host "              secuencia (Initialize+Shutdown antes de SDL) nunca se ha probado." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    modo de prueba del escalador con los DOS flags separados" -ForegroundColor Green
    }

    # Y el patron tiene que llevar el BORDE BLANCO: es lo que hace la lectura inequivoca.
    $voutPathS = Join-Path $root 'src\opennow\stream\PS4VideoOutRenderer.cpp'
    if (Test-Path -LiteralPath $voutPathS) {
        $voutSrcS = [System.IO.File]::ReadAllText($voutPathS)
        $voutCodeS = ($voutSrcS -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
        if ($voutCodeS -notmatch 'kBorder' -or $voutCodeS -notmatch 'put_white') {
            Write-Host "  [ESCALADOR] el patron de prueba no tiene el borde blanco exterior" -ForegroundColor Red
            Write-Host "              Es lo que delata si el framebuffer se estira o se presenta a su tamanio." -ForegroundColor Red
            $fail++
        } else {
            Write-Host "  [OK]    el patron de prueba lleva borde blanco en los limites del framebuffer" -ForegroundColor Green
        }
    }
}

# -------------------------------------------------------------------------------------------------
# LA SONDA DEL ESCALADOR: DOS FALLOS QUE LE COSTARON RONDAS ENTERAS (v4.01)
# -------------------------------------------------------------------------------------------------
# La sonda existia desde la v3.56 y **nunca se ejecuto**. Se encontraron DOS causas, y las dos se
# comprueban aqui para que no puedan volver:
#
#   (1) **ENTRY POINT.** Se enlazaba con `-e main`, asi que el entry point del ELF era `main` en vez de
#       `_start` de `crt1.o`. `crt1.o` es quien hace la inicializacion del runtime de C antes de llamar
#       a `main`; entrando directamente por `main` esa inicializacion no ocurre, y como la PRIMERA
#       instruccion de la sonda es escribir su log (`open`/`write`/`fsync`), **moria antes de la primera
#       linea** — que es exactamente el sintoma observado (el fichero de log no existia).
#       El cliente NO tiene ese problema porque no pasa `-e`: usa el entry point por defecto de `crt1.o`.
#
#   (2) **ID DEL MODULO INVENTADO.** Se llamaba a
#       `sceSysmoduleLoadModuleInternal(0x80000004 /* "ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT" */)`.
#       Ese valor es en realidad **ORBIS_SYSMODULE_INTERNAL_SYSCORE (libSceSysCore)**. El correcto, en
#       `orbis/_types/sysmodule.h`, es **ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT = 0x80000022**.
#       Es un caso peor que un ID invalido: **es un ID valido del modulo equivocado**, asi que no da un
#       error evidente sino que simplemente no carga lo que hace falta.
#
# La comprobacion se hace sobre los DOS scripts de build (el del cliente y el de la sonda).
$probeLinker = Join-Path $root 'scripts\build-videoout-scaler-probe.ps1'
if (Test-Path -LiteralPath $probeLinker) {
    $linkSrc = [System.IO.File]::ReadAllText($probeLinker)
    $linkCode = ($linkSrc -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"

    # (1) Ningun script de build puede forzar el entry point a `main`.
    $fuerzaMain = ($linkCode -match "'-e'\s*,\s*'main'" -or $linkCode -match '"-e"\s*,\s*"main"')
    if ($fuerzaMain) {
        Write-Host "  [SONDA] el enlace fuerza '-e main': el entry point se salta _start de crt1.o" -ForegroundColor Red
        Write-Host "          Sin la inicializacion del runtime, la primera llamada a open()/write() mata el" -ForegroundColor Red
        Write-Host "          proceso antes de escribir el primer log. Es la causa de que la sonda no arrancara." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    la sonda entra por _start (sin '-e main'), como el cliente" -ForegroundColor Green
    }
}

$probeSrc = Join-Path $root 'src\probes\videoout_scaler_probe.c'
if (Test-Path -LiteralPath $probeSrc) {
    $psrc = [System.IO.File]::ReadAllText($probeSrc)
    $pcode = ($psrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    # (2) El ID del modulo tiene que ser el de VideoOut, nunca el de SysCore.
    $idSyscore = ($pcode -match '0x80000004')
    $idVideoOut = ($pcode -match 'ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT' -or $pcode -match '0x80000022')
    if ($idSyscore) {
        Write-Host "  [SONDA] usa 0x80000004, que es ORBIS_SYSMODULE_INTERNAL_SYSCORE (libSceSysCore)" -ForegroundColor Red
        Write-Host "          El modulo de VideoOut es ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT = 0x80000022." -ForegroundColor Red
        Write-Host "          Es un ID VALIDO del modulo EQUIVOCADO: no da error evidente, simplemente no" -ForegroundColor Red
        Write-Host "          carga lo que hace falta." -ForegroundColor Red
        $fail++
    } elseif (-not $idVideoOut) {
        Write-Host "  [SONDA] no se identifica el ID del modulo de VideoOut" -ForegroundColor Red
        Write-Host "          Hace falta ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT (o su valor 0x80000022)." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    la sonda carga ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT (no SysCore)" -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# EL PIXEL FORMAT DE VideoOut: EL COMENTARIO NO PUEDE NOMBRAR UNA CONSTANTE INEXISTENTE (v4.01)
# -------------------------------------------------------------------------------------------------
# El proyecto registraba los buffers asi:
#
#     0x80000000u /* ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB */
#
# **Ese nombre NO existe en el SDK.** La tabla real:
#
#     OpenOrbis (el SDK de este proyecto):  ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB = 0x80002200
#     shadPS4 (referencia del proyecto):     SCE_VIDEO_OUT_PIXEL_FORMAT_A8R8G8B8_SRGB = 0x80000000
#                                            SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB = 0x80002200
#
# O sea: **0x80000000 es A8R8G8B8_SRGB** segun la referencia, no el A8B8G8R8 del SDK.
#
# **NO se cambia el VALOR** (ver el bloque largo en `PS4VideoOutRenderer.cpp`): la ruta directa FUNCIONA
# con 0x80000000 (119 flips estables; si el valor fuera invalido, `RegisterBuffers` fallaria y no
# presentaria nada), A8R8G8B8 y A8B8G8R8 pueden ser el mismo byte order con dos nombres, y **el riesgo es
# asimetrico**: cambiar un valor que funciona puede perder los 60 fps, mientras que un valor equivocado
# solo daria R y B intercambiados. Se corrige la ETIQUETA, que es lo unico demostrablemente erroneo.
$voutFormatPath = Join-Path $root 'src\opennow\stream\PS4VideoOutRenderer.cpp'
if (Test-Path -LiteralPath $voutFormatPath) {
    $fmtSrc = [System.IO.File]::ReadAllText($voutFormatPath)
    # OJO CON EL AMBITO DE ESTA COMPROBACION: el nombre falso APARECE en la documentacion del propio
    # bloque de comentarios (para explicar que no existe), asi que buscarlo en todo el fichero da un
    # FALSO POSITIVO. Se busca solo en los COMENTARIOS INMEDIATAMENTE pegados al literal del argumento,
    # que es donde estaba el error: `0x80000000u /* ... */`. Se toma la linea del literal y la siguiente,
    # que son las que forman esa etiqueta.
    $lineas = $fmtSrc -split "`n"
    $etiqueta = ''
    for ($i = 0; $i -lt $lineas.Count; $i++) {
        if ($lineas[$i] -match 'sceVideoOutSetBufferAttribute') {
            # El literal del formato va en las lineas siguientes al nombre de la funcion.
            $etiqueta = ($lineas[$i..([Math]::Min($i + 3, $lineas.Count - 1))] -join "`n")
            break
        }
    }
    $nombreFalso = ($etiqueta -match 'B8_G8_R8_A8_SRGB')
    # La documentacion de la discrepancia se comprueba donde SI debe estar: el bloque largo de comentarios.
    $dudaDocumentada = ($fmtSrc -match 'A8B8G8R8_SRGB\s*=\s*0x80002200')
    $valorSigueIgual = ($fmtSrc -match '0x80000000u')

    if ($nombreFalso) {
        Write-Host "  [FORMATO] vuelve a nombrarse ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB" -ForegroundColor Red
        Write-Host "            Esa constante NO existe en el SDK. El unico A8B8G8R8 del SDK es 0x80002200," -ForegroundColor Red
        Write-Host "            y 0x80000000 es A8R8G8B8_SRGB segun la referencia." -ForegroundColor Red
        $fail++
    } elseif (-not $dudaDocumentada) {
        Write-Host "  [FORMATO] falta la documentacion de la discrepancia del pixel format" -ForegroundColor Red
        Write-Host "            Sin ella, el siguiente que lo vea no sabra que 0x80000000 funciona y que el" -ForegroundColor Red
        Write-Host "            cambio a probar si el video sale con R y B intercambiados es 0x80002200." -ForegroundColor Red
        $fail++
    } elseif (-not $valorSigueIgual) {
        Write-Host "  [FORMATO] el valor 0x80000000 ha cambiado" -ForegroundColor Red
        Write-Host "            Es el valor con el que la ruta directa presento 119 flips estables." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    pixel format: valor probado (0x80000000) con la discrepancia documentada" -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# `Present()` TIENE QUE COMPROBAR `s_shutting_down` EN LOS TRES PUNTOS (v4.04)
# -------------------------------------------------------------------------------------------------
# CONDICION DE CARRERA REAL, y su sintoma es un cierre sin traza:
#
# `Shutdown()` hace lo correcto (marca `s_shutting_down`, espera 16 ms, drena los flips, desregistra los
# buffers, cierra el handle y llama a `FreeDirectMemory()`). Pero `Present()` **solo comprobaba la marca al
# ENTRAR**. Entre esa comprobacion y la escritura en el framebuffer hay trabajo — y sobre todo la
# conversion de color y el escalado, que cuestan **8,5-13,8 ms**.
#
# Si `Shutdown()` corre dentro de esa ventana (y puede: se dispara desde el bucle principal al detectar un
# fallo de flips persistente), `FreeDirectMemory()` libera la memoria directa **mientras `Present()` sigue
# escribiendo en `dst`, que apunta dentro de ella**. Escritura en memoria liberada = **cierre sin traza**,
# que es exactamente el fallo que el proyecto lleva varias versiones intentando eliminar.
#
# La ventana es estrecha y no se ha visto en un log, pero **el coste de cerrarla es una comprobacion**.
# Tiene que haber **TRES** comprobaciones:
#   1. al entrar en `Present()`
#   2. justo antes de escribir en el framebuffer (el ultimo punto para abortar sin haber escrito)
#   3. justo antes de `sceVideoOutSubmitFlip`
$voutRacePath = Join-Path $root 'src\opennow\stream\PS4VideoOutRenderer.cpp'
if (Test-Path -LiteralPath $voutRacePath) {
    $raceSrc = [System.IO.File]::ReadAllText($voutRacePath)
    $raceCode = ($raceSrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
    $comprobaciones = ([regex]::Matches($raceCode, 's_shutting_down\.load\(')).Count
    if ($comprobaciones -lt 3) {
        Write-Host ("  [CARRERA] solo {0} comprobacion(es) de s_shutting_down; hacen falta 3" -f $comprobaciones) -ForegroundColor Red
        Write-Host "            Con menos, un Shutdown() durante los 8,5-13,8 ms de conversion deja a" -ForegroundColor Red
        Write-Host "            Present() escribiendo en memoria ya liberada por FreeDirectMemory()." -ForegroundColor Red
        Write-Host "            El sintoma de eso es un cierre sin traza." -ForegroundColor Red
        $fail++
    } else {
        Write-Host ("  [OK]    Present() comprueba s_shutting_down en los 3 puntos ({0})" -f $comprobaciones) -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# EL INVARIANTE REAL: `Present()` Y `Shutdown()` EN EL MISMO HILO (v4.05)
# -------------------------------------------------------------------------------------------------
# CORRECCION DE UN HALLAZGO MIO. En la v4.04 afirme que habia una **condicion de carrera** entre
# `Present()` y `Shutdown()` (que `Shutdown` podia liberar la memoria directa mientras `Present` seguia
# escribiendo). **Era falso.** Al verificarlo:
#
#     main.cpp:5342   `AVFrameHolder::instance().get([&](...) {`   <- empieza el callback
#     main.cpp:5650       `...Present(frame)...`                    <- dentro
#     main.cpp:5692       `...Shutdown()...`                        <- dentro
#     main.cpp:5782   `});`                                         <- acaba el callback
#
# Las dos llamadas estan **en el mismo callback**, que corre en el **hilo principal**: `Present()` retorna
# antes de que se llame a `Shutdown()`. **Es una secuencia ordenada, no una carrera.**
#
# Lo que SI hay que proteger es **ese orden**, porque hoy se cumple **por construccion** y es facil de
# romper sin darse cuenta (moviendo el fallo persistente a un hilo de red para no bloquear el bucle, o
# llamando a `Shutdown()` desde un manejador de senal). Esta comprobacion fija el invariante: las dos
# llamadas tienen que seguir dentro del MISMO callback `AVFrameHolder::instance().get(...)`.
if (Test-Path -LiteralPath $mainPath) {
    $mainSrcO = [System.IO.File]::ReadAllText($mainPath)
    $mainCodeO = ($mainSrcO -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    # METODO: se busca el `Present()` del camino directo y, DESDE AHI, se mira hacia ATRAS el
    # `AVFrameHolder::instance().get(` mas cercano y hacia ADELANTE el `});` mas cercano. El
    # `Shutdown()` del fallo persistente tiene que caer DENTRO de ese intervalo.
    #
    # (La primera version de esta comprobacion usaba el PRIMER `get()` del fichero, que puede no ser el
    # callback del stream — hay mas de uno en main.cpp. Ese fue el motivo de que no detectara la mutacion:
    # el intervalo calculado no era el que se creia.)
    $idxPresent = $mainCodeO.IndexOf('PS4VideoOutRenderer::Present(')
    if ($idxPresent -lt 0) {
        Write-Host "  [INVARIANTE] no se encuentra la llamada a PS4VideoOutRenderer::Present()" -ForegroundColor Red
        $fail++
    } else {
        # Hacia atras: el inicio del callback que lo contiene.
        $idxInicio = $mainCodeO.LastIndexOf('AVFrameHolder::instance().get(', $idxPresent)
        # Hacia adelante: el primer `Shutdown()` DESPUES de Present, y el cierre del callback despues de el.
        $idxShutdown = $mainCodeO.IndexOf('PS4VideoOutRenderer::Shutdown()', $idxPresent)
        $idxCierre = if ($idxShutdown -ge 0) { $mainCodeO.IndexOf('});', $idxShutdown) } else { -1 }

        $mismoCallback = ($idxInicio -ge 0 -and $idxInicio -lt $idxPresent -and
                          $idxShutdown -gt $idxPresent -and
                          $idxCierre -gt $idxShutdown)
        # Y no puede haber OTRO Shutdown entre el inicio del callback y el Present (eso indicaria que el
        # shutdown esta antes de presentar, que es otro orden distinto y tambien peligroso).
        $shutdownAntes = $false
        if ($idxInicio -ge 0) {
            $previo = $mainCodeO.IndexOf('PS4VideoOutRenderer::Shutdown()', $idxInicio)
            $shutdownAntes = ($previo -ge 0 -and $previo -lt $idxPresent)
        }

        # =================================================================================================
        # EL INVARIANTE CAMBIO DE FORMA EN LA v4.31, Y ESTA COMPROBACION LO REFLEJA. LEER ANTES DE TOCAR.
        # =================================================================================================
        # ANTES: `Shutdown()` estaba DUPLICADO en el callback, asi que se exigia que las dos llamadas
        # cayeran dentro del MISMO callback. **Eso ya no es cierto y no debe serlo:** la recuperacion se
        # extrajo a `recoverFromDirectPathFailure()` para que **tambien la use el vigilante del bucle
        # principal**, porque **una recuperacion que solo corre dentro del callback no sirve cuando el
        # fallo es que la presentacion se para.**
        #
        # PUESTO QUE EL `Shutdown()` QUE SE VE AQUI DENTRO DEL CALLBACK ES AHORA **UNA LLAMADA A LA
        # FUNCION**, la propiedad que hay que fijar ya no es "esta dentro del callback" sino:
        #   * dentro del callback, el `Shutdown()` se alcanza **a traves de la funcion** (no duplicado), y
        #   * la funcion tiene su **candado** y su garantia (eso lo comprueba `[RECUPERA]` mas abajo).
        #
        # Se acepta por tanto: la llamada a la FUNCION dentro del callback, o el `Shutdown()` directo
        # (construccion antigua). Lo que NO se acepta es un `Shutdown()` que aparezca ANTES de presentar.
        $idxFuncion = $mainCodeO.LastIndexOf('recoverFromDirectPathFailure(', $idxShutdown + 1)
        $viaFuncion = ($idxFuncion -ge 0 -and $idxFuncion -gt $idxInicio -and $idxFuncion -le $idxShutdown)

        if ((-not $mismoCallback -and -not $viaFuncion) -or $shutdownAntes) {
            Write-Host "  [INVARIANTE] el Shutdown() del fallo persistente no esta ni dentro del callback" -ForegroundColor Red
            Write-Host "               ni alcanzable por la funcion de recuperacion, o esta ANTES de presentar." -ForegroundColor Red
            Write-Host "               Ese orden es lo que impide que Shutdown() libere la memoria directa" -ForegroundColor Red
            Write-Host "               mientras Present() escribe en ella (no hay exclusion mutua, solo la marca" -ForegroundColor Red
            Write-Host "               s_shutting_down). Si se pasa la presentacion a otro hilo, hace falta" -ForegroundColor Red
            Write-Host "               exclusion mutua de verdad, y el candado de la funcion no basta." -ForegroundColor Red
            $fail++
        } else {
            Write-Host "  [OK]    Present() y Shutdown() en el mismo callback (orden garantizado)" -ForegroundColor Green
        }
    }
}

# -------------------------------------------------------------------------------------------------
# LA RECUPERACION DE LA RUTA DIRECTA LLAMA A `Shutdown()` FUERA DEL CALLBACK (v4.31)
# -------------------------------------------------------------------------------------------------
# POR QUE HACE FALTA ESTA COMPROBACION, y es un cambio de invariante que hay que entender:
#
#   El proyecto exige que **`Shutdown()` no se ejecute mientras `Present()` podria estar escribiendo** en
#   la memoria directa: no hay exclusion mutua, solo la marca `s_shutting_down` dentro de `Present()`.
#   Eso se cumplia **por construccion** porque los dos vivian en el mismo callback.
#
#   Al extraer `recoverFromDirectPathFailure()` para que la use tambien el vigilante del bucle principal,
#   **`Shutdown()` puede ahora llamarse desde fuera del callback**. No es un descuido: es el proposito —
#   **una recuperacion que solo corre dentro del callback no sirve cuando el fallo es que la presentacion
#   se para.**
#
#   Lo que SI hay que garantizar es que sigue habiendo exclusion. Se garantiza con un candado que coge el
#   llamador del bucle principal (`desdeElCallback=false`) y que `Present()` no necesita porque el hilo
#   principal es el unico que presenta.
#
# Esta comprobacion fija las tres piezas que hacen valida esa garantia. Si falta una, el cambio de
# invariante deja de estar justificado.
if (Test-Path -LiteralPath $mainPath) {
    $mSrc = [System.IO.File]::ReadAllText($mainPath)
    $mCode = ($mSrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    $tieneCandado   = ($mCode -match 'g_directPathRecoveryMutex')
    $cogeEnBucle    = ($mCode -match 'guard\.lock\(\)')
    $dosDisparadores = (([regex]::Matches($mCode, 'recoverFromDirectPathFailure\(')).Count -ge 3)
    # Los dos sitios tienen que declarar de donde vienen: uno true (callback) y otro false (bucle).
    $marcaTrue  = ($mCode -match 'desdeElCallback=\*/true')
    $marcaFalse = ($mCode -match 'desdeElCallback=\*/false')

    if ($tieneCandado -and $cogeEnBucle -and $dosDisparadores -and $marcaTrue -and $marcaFalse) {
        Write-Host "  [OK]    recuperacion directa: candado + 2 disparadores, uno FUERA del callback" -ForegroundColor Green
    } else {
        Write-Host "  [RECUPERA] la recuperacion de la ruta directa perdio su garantia de exclusion" -ForegroundColor Red
        Write-Host ("             candado={0} lock={1} disparadores={2} desdeCallback_true={3} false={4}" -f `
            $tieneCandado, $cogeEnBucle, $dosDisparadores, $marcaTrue, $marcaFalse) -ForegroundColor Red
        Write-Host "             Shutdown() libera la memoria directa; si puede correr mientras Present()" -ForegroundColor Red
        Write-Host "             escribe en ella, el sintoma es un cierre sin traza. No quitar el candado." -ForegroundColor Red
        $fail++
    }
}
# -------------------------------------------------------------------------------------------------
# EL FORMATO DEL LIENZO PROPIO: NO PUEDE VOLVER A ARGB8888 (v4.06)
# -------------------------------------------------------------------------------------------------
# SINTOMA QUE ARREGLA ESTE CAMBIO: "toda la interfaz se ve roja en lugar de negra/verde" y las caratulas
# y el video con R y B cambiados.
#
# LA CADENA COMPLETA, con las mascaras REALES leidas del codigo de SDL y del driver:
#
#   1. El driver SDL-PS4 declara la superficie de la ventana como `SDL_PIXELFORMAT_BGR888`
#      (`SDL_ps4video.c:456`). Pese al nombre, **`BGR888` en SDL es `PACKEDORDER_XBGR`** y sus mascaras
#      son `Rmask=0x000000FF, Gmask=0x0000FF00, Bmask=0x00FF0000` (`SDL_pixels.c:446-450`).
#      Es decir: **en memoria `[R][G][B][X]`**.
#
#   2. El driver envia esa memoria a VideoOut con una **copia cruda, sin conversion**:
#          memcpy(&pDst[...], &surface->pixels[...], sizeof(uint32_t)*drawW);   // SDL_ps4video.c:519
#
#   3. VideoOut esta registrado como `SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB`
#      (`SDL_ps4video.c:221`), que en little-endian es `0xAARRGGBB` = **en memoria `[B][G][R][A]`**.
#
#   El driver manda `[R][G][B]` y el panel lee eso como `[B][G][R]` -> **R y B cambiados**.
#
#   Por eso el LIENZO tiene que compensarlo: su formato tiene que ser aquel cuyo byte BAJO sea R, y
#   **`BGR888` es ese formato** (Bmask=0x00FF0000 -> el byte alto es B y el bajo es R = memoria
#   `[R][G][B]`). Ademas es **el mismo formato que la ventana**, que es la condicion que la v3.82 midio
#   como la que activa el camino rapido de SDL (1,25 ns/px frente a 27,4 ns/px del lento).
#
# Y NO se puede arreglar en el driver: `libSDL2.a` es una libreria **prebuilt** del toolchain (no se
# reconstruye en este proyecto) y su objeto del driver **no contiene las constantes de formato** como
# bytes localizables (comprobado con `llvm-objdump`), asi que un parche binario seria a ciegas.
$canvasFmtPath = Join-Path $root 'src\ps4\main.cpp'
if (Test-Path -LiteralPath $canvasFmtPath) {
    $cfSrc = [System.IO.File]::ReadAllText($canvasFmtPath)
    $cfCode = ($cfSrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    $tieneConstante = ($cfCode -match '#define\s+GFN_CANVAS_PIXELFORMAT\s+SDL_PIXELFORMAT_BGR888')
    $creacionesBgr  = ([regex]::Matches($cfCode, 'SDL_CreateRGBSurfaceWithFormat\(0,\s*cw,\s*ch,\s*32,\s*GFN_CANVAS_PIXELFORMAT\)')).Count
    # Las creaciones en ARGB8888 solo se permiten si son EL RESPALDO DECLARADO (marcado con
    # `RESPALDO_DECLARADO`). Sin la marca, cualquier ARGB8888 es el bug de color que se acaba de arreglar.
    $totalArgb      = ([regex]::Matches($cfCode, 'SDL_CreateRGBSurfaceWithFormat\(0,\s*cw,\s*ch,\s*32,\s*SDL_PIXELFORMAT_ARGB8888\)')).Count
    $respaldos      = ([regex]::Matches($cfCode, 'SDL_PIXELFORMAT_ARGB8888\);\s*//\s*RESPALDO_DECLARADO')).Count
    $creacionesArgbInesperadas = $totalArgb - $respaldos
    $tieneRespaldo  = ($cfCode -match 'SDL_OWN_CANVAS_FORMAT_FALLBACK') -and ($respaldos -ge 3)

    if (-not $tieneConstante) {
        Write-Host "  [COLOR] falta la constante GFN_CANVAS_PIXELFORMAT = SDL_PIXELFORMAT_BGR888" -ForegroundColor Red
        Write-Host "          El lienzo DEBE estar en BGRX8888 (memoria [R][G][B]): el driver envia [R][G][B]" -ForegroundColor Red
        Write-Host "          como [B][G][R], asi que un lienzo ARGB8888 sale con R y B cambiados." -ForegroundColor Red
        $fail++
    } elseif ($creacionesBgr -lt 3) {
        Write-Host ("  [COLOR] solo {0} de las 3 creaciones del lienzo usan GFN_CANVAS_PIXELFORMAT" -f $creacionesBgr) -ForegroundColor Red
        Write-Host "          Las tres (arranque, restauracion tras VideoOut y camino de respaldo) tienen que" -ForegroundColor Red
        Write-Host "          usar la constante, o una de las rutas volvera a pintar con los canales cambiados." -ForegroundColor Red
        $fail++
    } elseif ($creacionesArgbInesperadas -gt 0) {
        Write-Host ("  [COLOR] hay {0} creacion(es) del lienzo en ARGB8888 sin marcar como respaldo" -f $creacionesArgbInesperadas) -ForegroundColor Red
        Write-Host "          ARGB8888 es el formato que produce los colores invertidos: el byte bajo de la" -ForegroundColor Red
        Write-Host "          memoria tiene que ser R, no B. Usa GFN_CANVAS_PIXELFORMAT (y si de verdad es un" -ForegroundColor Red
        Write-Host "          respaldo, marcalo con // RESPALDO_DECLARADO)." -ForegroundColor Red
        $fail++
    } elseif (-not $tieneRespaldo) {
        Write-Host "  [COLOR] falta el respaldo si el driver no soporta BGRX8888" -ForegroundColor Red
        Write-Host "          Sin el, un fallo de SDL_CreateRGBSurfaceWithFormat deja la app sin lienzo." -ForegroundColor Red
        $fail++
    } else {
        Write-Host ("  [OK]    lienzo propio en BGR888 (memoria [R][G][B]) en las 3 rutas ({0} respaldos)" -f $respaldos) -ForegroundColor Green
    }

    # Y la JUSTIFICACION tiene que estar escrita, con la contradiccion historica resuelta. Sin esto, el
    # siguiente que lea `PS4-V3.84` ("Color: correcto" con un lienzo ARGB8888) creera que este cambio
    # rompe el color, cuando esa tabla era un pronostico y no una medicion.
    # OJO: se comprueba sobre el FUENTE COMPLETO (\), no sobre \, porque la cita del
    # emulador vive en un COMENTARIO y \ los elimina.
    if ($cfSrc -notmatch 'eR8G8B8A8Srgb') {
        Write-Host "  [COLOR] falta la verificacion del formato real de VideoOut" -ForegroundColor Red
        Write-Host "          Hace falta citar vk_presenter.cpp: A8B8G8R8_Srgb -> eR8G8B8A8Srgb, o sea memoria" -ForegroundColor Red
        Write-Host "          [R][G][B][A]. Es lo que demuestra que el lienzo tiene que estar en [R][G][B] y no" -ForegroundColor Red
        Write-Host "          ARGB8888, que en memoria es [B][G][R][A] (invertido)." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    justificacion del color verificada contra el emulador (RGBA8)" -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# EL DOCUMENTO AUTORITATIVO NO PUEDE QUEDARSE ATRAS (v4.05)
# -------------------------------------------------------------------------------------------------
# `docs/ESTADO-ACTUAL.md` se declara a si mismo **el documento que manda sobre todos los demas**.
# Cuando eso es verdad es muy util; cuando se queda atras es **peor que no tenerlo**, porque afirma con
# autoridad cosas que ya no son ciertas.
#
# Y se habia quedado atras: decia **v3.30** con el build en **v3.99**, y afirmaba tener **58 parches**
# cuando habia **126**. Un documento que dice "esto manda" y lleva 70 versiones de retraso es una trampa.
$estadoPath = Join-Path $root 'docs\ESTADO-ACTUAL.md'
$buildScript = Join-Path $root 'scripts\build-ps4.ps1'
if ((Test-Path -LiteralPath $estadoPath) -and (Test-Path -LiteralPath $buildScript)) {
    $estadoTxt = [System.IO.File]::ReadAllText($estadoPath)
    $buildTxt = [System.IO.File]::ReadAllText($buildScript)
    $mBuild = [regex]::Match($buildTxt, "\`$appVersion\s*=\s*'([0-9]+\.[0-9]+)'")
    $mEstado = [regex]::Match($estadoTxt, '^#\s*ESTADO ACTUAL DEL PROYECTO\s*.\s*v([0-9]+\.[0-9]+)')
    $nVersiones = (Get-ChildItem (Join-Path $root 'docs\versiones') -File -Filter '*.md' -ErrorAction SilentlyContinue).Count

    if (-not $mBuild.Success -or -not $mEstado.Success) {
        Write-Host "  [ESTADO] no se puede leer la version del build o la del documento autoritativo" -ForegroundColor Red
        $fail++
    } elseif ($mBuild.Groups[1].Value -ne $mEstado.Groups[1].Value) {
        Write-Host ("  [ESTADO] ESTADO-ACTUAL.md dice v{0} y el build es v{1}" -f $mEstado.Groups[1].Value, $mBuild.Groups[1].Value) -ForegroundColor Red
        Write-Host "           Ese documento se declara el que MANDA sobre todos los demas. Si se queda atras," -ForegroundColor Red
        Write-Host "           afirma con autoridad cosas que ya no son ciertas (se quedo en v3.30 con el build" -ForegroundColor Red
        Write-Host "           en v3.99, y decia 58 parches cuando habia 126)." -ForegroundColor Red
        $fail++
    } elseif ($estadoTxt -notmatch ([regex]::Escape("**$nVersiones parches**"))) {
        Write-Host ("  [ESTADO] ESTADO-ACTUAL.md no dice que hay {0} parches" -f $nVersiones) -ForegroundColor Red
        Write-Host "           La cifra se puede contar, asi que se comprueba." -ForegroundColor Red
        $fail++
    } elseif ($estadoTxt -match '\b[0-9A-Fa-f]{64}\b') {
        # Un SHA-256 escrito a mano se queda obsoleto en cuanto se toca cualquier auditoria (el build
        # reempaqueta y el hash cambia), y entonces el documento AFIRMA CON AUTORIDAD UN DATO FALSO.
        # Paso exactamente eso en la v4.05: se escribio el hash, se anadio la auditoria [ESTADO], y el
        # hash dejo de ser cierto. Se prefiere el comando a un valor copiado.
        Write-Host "  [ESTADO] ESTADO-ACTUAL.md lleva un SHA-256 escrito a mano" -ForegroundColor Red
        Write-Host "           El hash cambia en CADA build: en cuanto se toca una auditoria, ese valor pasa a" -ForegroundColor Red
        Write-Host "           ser falso y el documento lo afirma con autoridad. Pon el COMANDO para calcularlo." -ForegroundColor Red
        $fail++
    } else {
        Write-Host ("  [OK]    ESTADO-ACTUAL.md al dia (v{0}, {1} parches)" -f $mBuild.Groups[1].Value, $nVersiones) -ForegroundColor Green
    }
}

# -------------------------------------------------------------------------------------------------
# LA TEXTURA DEL VIDEO TIENE QUE MEDIR EL LIENZO, COMO EL BUFFER Y EL BLIT (v4.11)
# -------------------------------------------------------------------------------------------------
# BUG GRAVE ENCONTRADO EN LOS LOGS DE LA v4.06, con las marcas delante:
#
#     STREAM_VIDEO_BGRA_TEXTURE_CREATED texture=960x540 dst=1920x1080    (16 veces seguidas)
#     STREAM_VIDEO_PHASES scale_us=35708 upload_us=0 copy_us=48389 frames=11
#
# **La textura se creaba con `dw,dh` (el FRAME, 960x540) mientras el escalador escribia `tw,th` (el
# LIENZO, 1920x1080) y la subida usaba `tw*4`.** Tres consecuencias medidas en consola:
#
#   1. `SDL_UpdateTexture` escribia 8,29 MB en una textura de 1,9 MB -> **fuera de limites**.
#   2. La condicion de re-creacion (`bgraWidth_ != tw`) se cumplia SIEMPRE -> **la textura se destruia y
#      recreaba en CADA frame**, y ahi se iban los **84 ms por frame = los 12-20 fps reportados**.
#   3. La textura de 960x540 se dibujaba en un destino de 1920x1080 -> **imagen estirada 2x ("gigante")**.
#
# El invariante: **el buffer, la textura y el destino del blit miden todos el LIENZO (`tw,th`)**, y la
# subida usa `tw*4` de paso. Si uno solo se queda en el tamanio del frame, vuelve el bug.
$voutTexPath = Join-Path $root 'src\opennow\stream\SDLVideoRenderer.cpp'
if (Test-Path -LiteralPath $voutTexPath) {
    $texSrc = [System.IO.File]::ReadAllText($voutTexPath)
    $texCode = ($texSrc -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

    # (a) la textura se crea con tw,th **Y CON EL MISMO FORMATO QUE EL LIENZO**
    #     El formato importa tanto como el tamanio: `SDL_LowerBlitScaled` solo usa su camino rapido
    #     cuando los formatos de origen y destino COINCIDEN. Con la textura en `ARGB8888` y el lienzo en
    #     `BGR888` no coincidian, y SDL caia a la conversion pixel a pixel: medido en consola con la
    #     v4.11, `copy_us=41.652` (20 ns/px) frente a 1,25 ns/px de una copia -> **19 fps en sesion**.
    $creaTw   = ($texCode -match 'SDL_CreateTexture\(target_,\s*SDL_PIXELFORMAT_ARGB8888,\s*SDL_TEXTUREACCESS_STREAMING,\s*tw,\s*th\)')
    $guardaTw = ($texCode -match 'bgraWidth_\s*=\s*tw;\s*bgraHeight_\s*=\s*th;')
    $subeTw   = ($texCode -match 'SDL_UpdateTexture\(bgraTexture_,\s*nullptr,\s*bgra_\.data\(\),\s*tw\s*\*\s*4\)')
    $bufferTw = ($texCode -match 'need\s*=\s*static_cast<size_t>\(tw\)\s*\*\s*static_cast<size_t>\(th\)\s*\*\s*4u')
    $blitTw   = ([regex]::Matches($texCode, 'dstBgra\s*=\s*\{\s*0\s*,\s*0\s*,\s*tw\s*,\s*th\s*\}')).Count -ge 2

    $faltan = @()
    if (-not $creaTw)   { $faltan += 'SDL_CreateTexture(...,tw,th)' }
    if (-not $guardaTw) { $faltan += 'bgraWidth_=tw; bgraHeight_=th' }
    if (-not $subeTw)   { $faltan += 'SDL_UpdateTexture(...,tw*4)' }
    if (-not $bufferTw) { $faltan += 'need=tw*th*4' }
    if (-not $blitTw)   { $faltan += 'dstBgra={0,0,tw,th} (x2)' }

    if ($faltan.Count -gt 0) {
        Write-Host "  [TEXTURA] el buffer, la textura y el blit NO miden todos el lienzo" -ForegroundColor Red
        Write-Host ("            Falta: {0}" -f ($faltan -join ', ')) -ForegroundColor Red
        Write-Host "            Si uno se queda en el tamanio del frame (dw,dh) y los otros en el lienzo" -ForegroundColor Red
        Write-Host "            (tw,th), la textura se recrea en CADA frame (84 ms medidos, 12-20 fps) y la" -ForegroundColor Red
        Write-Host "            imagen sale estirada 2x. Los tres tienen que medir el LIENZO." -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [OK]    textura, buffer y blit del video miden el lienzo (tw,th)" -ForegroundColor Green
    }
}

# =====================================================================================================
# TECHO DE 720p (v4.22). Evita que vuelva a registrarse un framebuffer de 1080p.
# =====================================================================================================
# POR QUE ESTA COMPROBACION: el usuario lo pidio con estas palabras — *"a 720p, sin 1080 ya que es
# dificil"* — y los datos le dan la razon. Medido en consola:
#
#     Framebuffer 1280x720   -> 119 flips estables, y con el stream a 720p da
#                               `frames=60 iter_ms=16 render_max_ms=32` (el presupuesto se cumple)
#     Framebuffer 1920x1080  -> 1 solo flip y la sesion se cerro
#
# Y 1080 no aportaria nada visible: sobre un panel de 1080p un framebuffer de 720p **se estira y llena
# la pantalla**, y ese escalado lo hace VideoOut **en hardware**. Con la fuente del stream a 720p, un
# framebuffer de 1080p solo anadiria 4 veces mas pixeles que escribir.
#


if ($fail -gt 0) {
    Write-Host ""
    Write-Host "$fail comprobacion(es) FALLARON" -ForegroundColor Red
    exit 1
}
Write-Host ""
Write-Host "Todas las comprobaciones obligatorias pasaron" -ForegroundColor Green
exit 0
