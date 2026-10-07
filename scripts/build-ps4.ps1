param([switch]$Clean)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$tc = $env:OO_PS4_TOOLCHAIN
$llvm = Split-Path -Parent (Get-Command clang.exe).Source
$winBin = Join-Path $tc 'bin\windows'
$sample = Join-Path $tc 'samples\SDL2'
$src = Join-Path $root 'src\ps4\main.cpp'
$build = Join-Path $root 'build\ps4'
$stage = Join-Path $build 'package'
$objDir = Join-Path $build 'obj'
$nativeDist = Join-Path $root 'build\libpeer-ps4\dist'
$gfnArchive = Join-Path $root 'build\gfn-client-ps4\libgfn_client_ps4.a'
$peerArchive = Join-Path $root 'build\libpeer-ps4\src\libpeer.a'
$appVersion = '4.32'  # bump on every build
$titleId = 'GFNP00001'
$contentId = 'IV0000-GFNP00001_00-GFNPS4CLIENT0001'
# Sony permite letras Y digitos en el Title ID (A-Z0-9), 9 caracteres. La validacion
# anterior exigia 4 letras + 5 DIGITOS, que era demasiado estricta: rechazaba nombres validos
# como GFNAJPS4. Se comprueba el formato real y que no empiece por digito.
# Title ID de PS4: 9 caracteres, 2 letras de region + 7 alfanumericos. El formato anterior
# exigia 4 letras + 5 DIGITOS y rechazaba nombres validos. GFNAJ0001 es el formato probado.
if ($titleId -notmatch '^[A-Z]{2}[A-Z0-9]{7}$') { throw "Invalid PS4 Title ID: $titleId (esperado: 2 letras de region + 7 alfanumericos)" }
# El Content ID debe tener EXACTAMENTE 36 caracteres (lo exige PkgTool). Con Title ID de 9, el
# sufijo final es de 16, que es el formato que ya funcionaba en builds anteriores.
if ($contentId.Length -ne 36) { throw "Invalid PS4 Content ID length: $contentId ($($contentId.Length) caracteres, se necesitan 36)" }
if ($contentId -notmatch "^IV0000-$titleId`_00-[A-Z0-9]{16}$") { throw "Invalid PS4 Content ID: $contentId" }

if ($Clean -and (Test-Path $build)) { Remove-Item -LiteralPath $build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage,$objDir,(Join-Path $stage 'sce_sys\about'),(Join-Path $stage 'sce_module') | Out-Null

function Invoke-Checked([string]$File, [string[]]$Arguments, [string]$WorkingDirectory=$root) {
    Push-Location $WorkingDirectory
    try {
        & $File @Arguments
        if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File $($Arguments -join ' ')" }
    } finally { Pop-Location }
}

$clang = Join-Path $llvm 'clang++.exe'
$clangC = Join-Path $llvm 'clang.exe'
$lld = Join-Path $llvm 'ld.lld.exe'
$fself = Join-Path $winBin 'create-fself.exe'
$gp4Tool = Join-Path $winBin 'create-gp4.exe'
$pkgTool = Join-Path $winBin 'PkgTool.Core.exe'

# Reconstruir libpeer si falta el .a o si el fuente es mas nuevo que el artefacto. Antes solo se
# miraba que existiera, asi que un cambio en rtcp.c/peer_connection.c no se reflejaba nunca.
$peerSources = @(
    (Join-Path $root 'src\third_party\libpeer-orbis\src\rtcp.c'),
    (Join-Path $root 'src\third_party\libpeer-orbis\src\rtcp.h'),
    (Join-Path $root 'src\third_party\libpeer-orbis\src\peer_connection.c'),
    (Join-Path $root 'src\third_party\libpeer-orbis\src\peer_connection.h')
)
$peerStale = $false
if (Test-Path -LiteralPath $peerArchive) {
    $peerArchiveTime = (Get-Item -LiteralPath $peerArchive).LastWriteTime
    foreach ($ps in $peerSources) {
        if ((Test-Path -LiteralPath $ps) -and (Get-Item -LiteralPath $ps).LastWriteTime -gt $peerArchiveTime) {
            $peerStale = $true
            break
        }
    }
}
if (-not (Test-Path -LiteralPath $peerArchive) -or $peerStale -or
    -not (Test-Path -LiteralPath (Join-Path $nativeDist 'lib\libopus.a'))) {
    & (Join-Path $PSScriptRoot 'build-libpeer-ps4.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Building native WebRTC dependencies failed' }
}
if (-not (Test-Path -LiteralPath (Join-Path $nativeDist 'lib\libavcodec.a')) -or
    -not (Test-Path -LiteralPath (Join-Path $nativeDist 'lib\libavutil.a'))) {
    & (Join-Path $PSScriptRoot 'build-media-ps4.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Building the PS4 H.264 decoder failed' }
}
& (Join-Path $PSScriptRoot 'build-gfn-client-ps4.ps1')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $gfnArchive)) { throw 'Building the native GFN client failed' }

$compileArgs = @(
    '--target=x86_64-pc-freebsd12-elf','-std=c++20','-D__ORBIS__=1','-fPIC','-funwind-tables','-O3','-march=btver2','-c',
    # La version se INYECTA desde aqui. Antes estaba escrita a mano en tres sitios de main.cpp
    # ("version=3.30", "app_version=3.30" y la barra de estado) ADEMAS de en $appVersion, y era
    # cuestion de tiempo que se desincronizaran: de hecho la auditoria de build fallo por eso al subir
    # a 3.31. Con esta definicion hay UNA sola fuente de verdad.
    ('-DGFN_APP_VERSION="' + $appVersion + '"'),
    '-isysroot',$tc,'-isystem',(Join-Path $tc 'include'),
    '-isystem',(Join-Path $tc 'include\c++\v1'),'-I',(Join-Path $root 'src\third_party\cJSON'),
    '-I',(Join-Path $root 'src\opennow'),'-I',(Join-Path $nativeDist 'include'),
    '-I',(Join-Path $root 'src\third_party\libpeer-orbis\src'),
    '-I',(Join-Path $root 'src\third_party\libpeer-orbis\third_party\coreHTTP\source\interface'),
    $src,'-o',(Join-Path $objDir 'main.o')
)
Invoke-Checked $clang $compileArgs
Invoke-Checked $clangC @(
    '--target=x86_64-pc-freebsd12-elf','-fPIC','-funwind-tables','-O3','-march=btver2','-c',
    '-isysroot',$tc,'-isystem',(Join-Path $tc 'include'),
    (Join-Path $root 'src\opennow\qrcodegen.c'),'-o',(Join-Path $objDir 'qrcodegen.o')
)
Invoke-Checked $clangC @(
    '--target=x86_64-pc-freebsd12-elf','-fPIC','-funwind-tables','-O3','-march=btver2','-c',
    '-isysroot',$tc,'-isystem',(Join-Path $tc 'include'),
    (Join-Path $root 'src\third_party\cJSON\cJSON.c'),'-o',(Join-Path $objDir 'cJSON.o')
)
# libSceVideodec (v1) ABI: the static_asserts in its header are the test, and the
# runtime probe it exports is called from the boot inventory.
Invoke-Checked $clang @(
    '--target=x86_64-pc-freebsd12-elf','-x','c++','-std=c++20','-D__ORBIS__=1','-fPIC','-funwind-tables','-O3','-march=btver2','-c',
    '-isysroot',$tc,'-isystem',(Join-Path $tc 'include'),
    '-isystem',(Join-Path $tc 'include\c++\v1'),
    '-I',(Join-Path $root 'src'),
    (Join-Path $root 'src\opennow\stream\videodec_abi_check.cpp'),'-o',(Join-Path $objDir 'videodec_abi_check.o')
)

$elf = Join-Path $objDir 'gfnps4.elf'
$ldArgs = @(
    '-m','elf_x86_64','-pie','--script',(Join-Path $tc 'link.x'),'--eh-frame-hdr',
    '-L',(Join-Path $tc 'lib'),'-L',(Join-Path $nativeDist 'lib'),'-o',$elf,(Join-Path $tc 'lib\crt1.o'),
    (Join-Path $objDir 'main.o'),(Join-Path $objDir 'qrcodegen.o'),(Join-Path $objDir 'cJSON.o'),
    (Join-Path $objDir 'videodec_abi_check.o'),
    '--start-group',$gfnArchive,$peerArchive,
    (Join-Path $nativeDist 'lib\libusrsctp.a'),(Join-Path $nativeDist 'lib\libsrtp2.a'),
    (Join-Path $nativeDist 'lib\libmbedtls.a'),(Join-Path $nativeDist 'lib\libmbedx509.a'),
    (Join-Path $nativeDist 'lib\libmbedcrypto.a'),(Join-Path $nativeDist 'lib\libavcodec.a'),
    (Join-Path $nativeDist 'lib\libavutil.a'),(Join-Path $nativeDist 'lib\libopus.a'),
    (Join-Path $nativeDist 'lib\libjansson.a'),(Join-Path $nativeDist 'lib\libcjson.a'),'--end-group',
    '-lc','-lkernel','-lc++','-lm','-lSceUserService',
    '-lSceVideoOut','-lSceAudioOut','-lScePad','-lSceSysmodule',
    '-lScePigletv2VSH','-lScePrecompiledShaders',
    '-lSceNet','-lSceHttp','-lSceSsl','-lSceRandom','-lSceKeyboard','-lSceMouse','-lSceImeDialog','-lSceCommonDialog','-lSDL2',
    # Decodificador v2 (H.264 por hardware) + ARBITRACION del decodificador.
    #
    # YouTube para PS4 carga libSceVideodec2 JUNTO a libSceVideoDecoderArbitration: en PS4 el
    # decodificador es un recurso COMPARTIDO con el sistema (grabacion, transmision), asi que hay
    # que solicitarlo por el modulo de arbitracion antes de usarlo. Ese es el paso que faltaba:
    # intentabamos cargar VIDEODEC2 directamente y fallaba con 0x805A1000.
    #
    # La v1 (libSceVideodec) se mantiene porque su sysmodule SI carga en este firmware y su ABI ya
    # esta declarada; la v2 es la que da acceso al decoder por hardware real.
    '-lSceVideodec',
    '-lSceVideodec2',
    '-lSceVideoDecoderArbitration'
)
Invoke-Checked $lld $ldArgs
Invoke-Checked $fself @('-in',$elf,'-out',(Join-Path $build 'gfnps4.oelf'),'-eboot',(Join-Path $stage 'eboot.bin'),'-paid','0x3800000000000011') $build

# SDL2 on PS4 expects these system modules alongside the executable.
Copy-Item -LiteralPath (Join-Path $sample 'sce_module\libc.prx') -Destination (Join-Path $stage 'sce_module\libc.prx') -Force
Copy-Item -LiteralPath (Join-Path $sample 'sce_module\libSceFios2.prx') -Destination (Join-Path $stage 'sce_module\libSceFios2.prx') -Force
Copy-Item -LiteralPath (Join-Path $sample 'sce_sys\about\right.sprx') -Destination (Join-Path $stage 'sce_sys\about\right.sprx') -Force
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'assets\misc') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'assets\fonts') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'assets\certs\mozilla-ca-bundle.pem') -Destination (Join-Path $stage 'assets\misc\mozilla-ca-bundle.pem') -Force
Copy-Item -LiteralPath (Join-Path $tc 'samples\system\assets\fonts\Gontserrat-Regular.ttf') -Destination (Join-Path $stage 'assets\fonts\Gontserrat-Regular.ttf') -Force
Copy-Item -LiteralPath (Join-Path $tc 'samples\system\assets\fonts\OFL.txt') -Destination (Join-Path $stage 'assets\fonts\OFL.txt') -Force

# QR codes for the "ABOUT" screen, from the repository's pngs/ folder.
#
# THEY GO IN assets/misc/ ON PURPOSE, NOT IN THEIR OWN assets/qr/ FOLDER.
# `create-gp4.exe` builds the GP4 directory tree from a FIXED list of subdirectories under assets/
# (audio, fonts, images, misc, videos). A new folder such as assets/qr is never declared as a <dir>
# node, and then `pkg_build` fails with "Sequence contains no elements" while trying to resolve the
# file's parent directory. Putting them in an existing folder avoids fighting that tool.
#
# The names here MUST match the `qrImages` table in src/ps4/main.cpp.
#
# SE CONVIERTEN A RGBA CRUDO EN TIEMPO DE COMPILACION, NO SE COPIAN LOS PNG.
#
# POR QUE: el FFmpeg de este proyecto se compila con UN UNICO decodificador (`CONFIG_H264_DECODER 1`;
# `CONFIG_PNG_DECODER`, `CONFIG_MJPEG_DECODER` y `CONFIG_BMP_DECODER` estan a 0). Comprobado en
# `build/ffmpeg-ps4/config_components.h`. Por eso en la consola salia siempre:
#
#     ABOUT_QR_LOAD_FAILED tiktoklink.png bytes=147635     <-- los bytes eran CORRECTOS
#
# El fichero se leia bien; lo que no existia era el DECODIFICADOR de PNG.
#
# Convirtiendo aqui, en el PC, se elimina esa dependencia por completo:
#   - CERO decodificacion en la consola.
#   - CERO librerias nuevas en el binario.
#   - El QR se ve con bordes NITIDOS (sin artefactos de compresion que rompan los modulos).
#   - Y el PKG encoge: los PNG suman 525 KB con pixeles que nunca se mostrarian.
#
# Formato del fichero .rgba (el mismo que lee `loadQrThread` en src/ps4/main.cpp):
#     bytes 0..3 : ancho (uint32 little-endian)
#     bytes 4..7 : alto  (uint32 little-endian)
#     bytes 8..  : pixeles RGBA8888, fila por fila, sin relleno
#
# 190x190 es el tamano al que se dibuja (156 px) mas un margen para que el reescalado del renderizador
# nunca amplie la imagen y emborrone los modulos.
$qrRawSize = 190
$qrAssets = @(
    @{ Source = 'tiktoklink.png';  Target = 'tiktok_qr.rgba'  },
    @{ Source = 'youtubelink.png'; Target = 'youtube_qr.rgba' },
    @{ Source = 'miweblink.png';   Target = 'miweb_qr.rgba'   }
)
foreach ($qr in $qrAssets) {
    $qrSource = Join-Path $root ('pngs\' + $qr.Source)
    if (-not (Test-Path -LiteralPath $qrSource)) { throw "About-screen QR asset not found: $qrSource" }

    $qrImage = [System.Drawing.Image]::FromFile($qrSource)
    try {
        $qrBitmap = New-Object System.Drawing.Bitmap $qrRawSize, $qrRawSize
        try {
            $qrGraphics = [System.Drawing.Graphics]::FromImage($qrBitmap)
            try {
                # HighQualityBicubic para que la rejilla del QR conserve los bordes definidos al
                # reducir de 1584 a 190. Con NearestNeighbor se perderian modulos.
                $qrGraphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $qrGraphics.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $qrGraphics.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                # Fondo BLANCO explicito: el QR es negro sobre blanco y ese blanco es el "quiet zone"
                # que el lector necesita. Si quedara un borde transparente, no se leeria.
                $qrGraphics.Clear([System.Drawing.Color]::White)
                $qrGraphics.DrawImage($qrImage, 0, 0, $qrRawSize, $qrRawSize)
            } finally { $qrGraphics.Dispose() }

            # Extraer los pixeles en RGBA8888. Se bloquea el bitmap para leer la memoria directamente en
            # lugar de llamar a GetPixel pixel a pixel (que seria 36.100 llamadas por imagen).
            $qrRect = New-Object System.Drawing.Rectangle 0, 0, $qrRawSize, $qrRawSize
            $qrData = $qrBitmap.LockBits($qrRect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                                         [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $qrStride = $qrData.Stride
                $qrBuffer = New-Object byte[] ($qrStride * $qrRawSize)
                [System.Runtime.InteropServices.Marshal]::Copy($qrData.Scan0, $qrBuffer, 0, $qrBuffer.Length)
            } finally { $qrBitmap.UnlockBits($qrData) }
        } finally { $qrBitmap.Dispose() }

        # Escribir cabecera + pixeles. OJO: `Format32bppArgb` en memoria es BGRA en little-endian, hay
        # que intercambiar los canales R y B para dejar RGBA de verdad.
        $qrOut = New-Object byte[] (8 + $qrRawSize * $qrRawSize * 4)
        [BitConverter]::GetBytes([uint32]$qrRawSize).CopyTo($qrOut, 0)
        [BitConverter]::GetBytes([uint32]$qrRawSize).CopyTo($qrOut, 4)
        $qrDst = 8
        for ($y = 0; $y -lt $qrRawSize; $y++) {
            $qrRow = $y * $qrStride
            for ($x = 0; $x -lt $qrRawSize; $x++) {
                $s = $qrRow + $x * 4
                $qrOut[$qrDst]     = $qrBuffer[$s + 2]   # R  <- B en memoria
                $qrOut[$qrDst + 1] = $qrBuffer[$s + 1]   # G
                $qrOut[$qrDst + 2] = $qrBuffer[$s]       # B  <- R en memoria
                $qrOut[$qrDst + 3] = 255                 # A opaco: el fondo ya se pinto blanco
                $qrDst += 4
            }
        }
        $qrDest = Join-Path $stage ('assets\misc\' + $qr.Target)
        [System.IO.File]::WriteAllBytes($qrDest, $qrOut)
        Write-Output ("QR asset: {0} -> {1} ({2}x{2} RGBA, {3} KB)" -f `
            $qr.Source, $qr.Target, $qrRawSize, [math]::Round($qrOut.Length / 1KB, 1))
    } finally { $qrImage.Dispose() }
}

# Scale the supplied AJ / GeForce NOW artwork to the PS4 application icon size.
Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap 512,512
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$sourceLogo = [System.Drawing.Image]::FromFile((Join-Path $root 'assets\branding\aj-geforce-now-ps4.png'))
$graphics.DrawImage($sourceLogo,0,0,512,512)
$sourceLogo.Dispose(); $graphics.Dispose()
$icon = Join-Path $stage 'sce_sys\icon0.png'
$bitmap.Save($icon,[System.Drawing.Imaging.ImageFormat]::Png); $bitmap.Dispose()

$sfo = Join-Path $stage 'sce_sys\param.sfo'
Invoke-Checked $pkgTool @('sfo_new',$sfo)
$entries = @(
    @('APP_TYPE','Integer','4','1'), @('APP_VER','Utf8','8',$appVersion),
    @('ATTRIBUTE','Integer','4','0'), @('CATEGORY','Utf8','4','gd'),
    @('CONTENT_ID','Utf8','48',$contentId), @('DOWNLOAD_DATA_SIZE','Integer','4','0'),
    @('SYSTEM_VER','Integer','4','0'), @('TITLE','Utf8','128','AJ - GeForce NOW PS4'),
    @('TITLE_ID','Utf8','12',$titleId), @('VERSION','Utf8','8',$appVersion)
)
foreach ($entry in $entries) {
    Invoke-Checked $pkgTool @('sfo_setentry',$sfo,$entry[0],'--type',$entry[1],'--maxsize',$entry[2],'--value',$entry[3])
}

$gp4 = Join-Path $stage 'gfnps4.gp4'
$fileList = 'eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx assets/misc/mozilla-ca-bundle.pem assets/fonts/Gontserrat-Regular.ttf assets/fonts/OFL.txt assets/misc/tiktok_qr.rgba assets/misc/youtube_qr.rgba assets/misc/miweb_qr.rgba'
Invoke-Checked $gp4Tool @('-out',$gp4,'-content-id',$contentId,'-files',$fileList) $stage
Invoke-Checked $pkgTool @('pkg_build',$gp4,$build) $stage

$pkg = Join-Path $build ($contentId+'.pkg')
if (-not (Test-Path -LiteralPath $pkg)) { throw "PKG output was not created: $pkg" }
Write-Output "Built PS4 test PKG: $pkg"
Write-Output "ELF: $elf"
Write-Output 'Install via GoldHEN Package Installer, then test UI, DualShock, keyboard and mouse.'

# Auditoria automatica: comprueba que cada cambio declarado esta REALMENTE en el eboot.bin
# construido, no solo en el fuente. Existe porque en varias iteraciones se dio por aplicado
# un cambio que el binario no llevaba (el caso mas costoso: el escalador SIMD estaba presente
# pero un self-check lo rechazaba, y nadie lo supo hasta que se midio).
# Verificacion del layout de la pantalla de configuracion: comprueba que cada etiqueta caiga
# dentro de su panel. Los fallos de posicionamiento ya se escaparon dos veces a revision manual.
$layoutAudit = Join-Path $PSScriptRoot 'audit-settings-layout.ps1'
if (Test-Path $layoutAudit) {
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $layoutAudit
    if ($LASTEXITCODE -ne 0) { throw "El layout de configuracion tiene problemas ($LASTEXITCODE)." }
}

$audit = Join-Path $PSScriptRoot 'audit-build.ps1'
if (Test-Path $audit) {
    Write-Output ''
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $audit
    if ($LASTEXITCODE -ne 0) {
        throw "La auditoria del binario fallo ($LASTEXITCODE). El PKG existe pero le faltan caracteristicas declaradas; revisa la tabla de arriba."
    }
}

# Auditoria de NAVEGACION: comprueba que la navegacion y el dibujado coincidan.
#
# Existe porque en la v3.31 se encontraron tres fallos del centro de juego (tablas paralelas con
# nullptr, el numero de tarjetas escrito a mano en tres sitios, y filas mal contadas) que NINGUNA de
# las otras auditorias detecto: cubrian el binario, las traducciones y el layout de CONFIGURACION,
# pero no la relacion entre lo que se DIBUJA y lo que la navegacion SELECCIONA.
$navAudit = Join-Path $PSScriptRoot 'audit-navigation.ps1'
if (Test-Path $navAudit) {
    Write-Output ''
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $navAudit
    if ($LASTEXITCODE -ne 0) {
        throw "La auditoria de navegacion fallo ($LASTEXITCODE). El dibujado y la navegacion no coinciden."
    }
}

# PRUEBAS DE HOST: comprueban la LOGICA de la rejilla del menu EJECUTANDOLA de verdad en el PC.
#
# POR QUE ADEMAS DE LAS AUDITORIAS: las auditorias analizan el TEXTO del fuente; estas pruebas
# EJECUTAN las formulas. La diferencia importa: una auditoria puede comprobar que la formula esta
# escrita, pero solo ejecutarla demuestra que produce el indice correcto en las cuatro direcciones y
# para las seis tarjetas.
#
# Se validaron con MUTACIONES: se introdujeron a proposito cuatro fallos (desfase +1, quitar el modulo
# de filas, rejilla de 4 columnas, y mover la fila con la horizontal) y los cuatro fueron detectados.
# Una prueba que nunca falla no demuestra nada.
$hostTests = Join-Path $PSScriptRoot 'run-host-tests.ps1'
if (Test-Path $hostTests) {
    Write-Output ''
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $hostTests
    if ($LASTEXITCODE -ne 0) {
        throw "Las pruebas de host fallaron ($LASTEXITCODE). La logica de la interfaz no es correcta."
    }
}
