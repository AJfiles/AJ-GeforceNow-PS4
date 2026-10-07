param([switch]$Clean)
$ErrorActionPreference = 'Stop'

# =================================================================================================
# SONDA DEL ESCALADOR DE HARDWARE DE VideoOut
# =================================================================================================
# Construye un PKG AISLADO que registra un framebuffer de 960x540 en VideoOut y comprueba si la GPU
# de la PS4 lo presenta ESCALADO a la resolucion del display (1920x1080).
#
# POR QUE HACE FALTA ESTA SONDA Y NO BASTA CON LEER EL CODIGO:
# el driver de SDL fija el tamano del framebuffer de VideoOut al del DISPLAY e ignora a la aplicacion
# (`SDL_ps4video.c:212-223`), asi que la unica via es el camino directo. Y si VideoOut escala por si
# solo un buffer pequeno es una pregunta que **solo responde el firmware**, no el codigo.
#
# No toca el cliente ni comparte estado con el: es un PKG aparte, como las sondas de Piglet.
# =================================================================================================

. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$toolchain = $env:OO_PS4_TOOLCHAIN
$build = Join-Path $root 'build\research\videoout-scaler-probe'
$stage = Join-Path $build 'package'
$objDir = Join-Path $build 'obj'
$llvmBin = Split-Path -Parent (Get-Command clang.exe).Source
$clang = Join-Path $llvmBin 'clang.exe'
$lld = Join-Path $llvmBin 'ld.lld.exe'
$fself = Join-Path $toolchain 'bin\windows\create-fself.exe'
$gp4Tool = Join-Path $toolchain 'bin\windows\create-gp4.exe'
$pkgTool = Join-Path $toolchain 'bin\windows\PkgTool.Core.exe'
$titleId = 'VOSC00001'
$contentId = 'IV0000-VOSC00001_00-AJVIDEOOUTSCALER'
$source = Join-Path $root 'src\probes\videoout_scaler_probe.c'

if ($Clean -and (Test-Path -LiteralPath $build)) { Remove-Item -LiteralPath $build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $objDir,$stage,(Join-Path $stage 'sce_sys\about'),(Join-Path $stage 'sce_module') | Out-Null

function Invoke-Checked([string]$File,[string[]]$Arguments,[string]$WorkingDirectory=$root) {
    Push-Location $WorkingDirectory
    try { & $File @Arguments; if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File $($Arguments -join ' ')" } }
    finally { Pop-Location }
}

$object = Join-Path $objDir 'videoout_scaler_probe.o'
$elf = Join-Path $objDir 'videoout_scaler_probe.elf'
Invoke-Checked $clang @(
    '--target=x86_64-pc-freebsd12-elf','-std=c11','-O2','-fPIC','-D_BSD_SOURCE','-D__ORBIS__=1','-c',
    '-isysroot',$toolchain,'-isystem',(Join-Path $toolchain 'include'),$source,'-o',$object
)
# Enlaza contra VideoOut, Sysmodule (para sceSysmoduleLoadModuleInternal) y el kernel.
#
# =================================================================================================
# `-e main` RETIRADO (v4.00). ESTE ERA EL MOTIVO DE QUE LA SONDA NO ARRANCARA.
# =================================================================================================
# La sonda se enlazaba con `-e main`, es decir **el entry point del ELF era `main`** en vez de `_start`.
# `crt1.o` (que SI se enlaza, en la linea de abajo) define `_start`, y es quien hace la inicializacion
# del runtime de C **antes** de llamar a `main`. Entrando directamente por `main`:
#
#   - NO se ejecuta la inicializacion de `crt1.o`.
#   - La primera cosa que hace la sonda es `probe_log()`, que llama a `open()`, `write()`, `fsync()` y
#     `close()` sobre `/data/aj_videoout_scaler_probe.log`. Si esas funciones dependen del runtime sin
#     inicializar, el proceso **muere antes de escribir la primera linea**.
#
# **Y eso es exactamente el sintoma observado**: `/data/aj_videoout_scaler_probe.log` **no existe**, o
# sea que no se escribio ni la primera linea (la primera llamada de `main` es precisamente esa).
#
# El CLIENTE no tiene este problema porque **no pasa `-e`**: usa el entry point por defecto de `crt1.o`,
# que es `_start`. Comparado linea a linea:
#
#     cliente (arranca):   '-m','elf_x86_64','-pie','--script',link.x,'--eh-frame-hdr', ...
#     sonda (no arranca):  '-m','elf_x86_64','-pie','--script',link.x,'-e','main','--eh-frame-hdr', ...
#
# Se retira `-e main` para que la sonda entre por `_start` igual que el cliente. Esto explica tambien por
# que el PAID no era el problema (se comprobo: es identico al del cliente) ni la estructura del paquete.
Invoke-Checked $lld @(
    '-m','elf_x86_64','-pie','--script',(Join-Path $toolchain 'link.x'),'--eh-frame-hdr',
    '-L',(Join-Path $toolchain 'lib'),'-o',$elf,(Join-Path $toolchain 'lib\crt1.o'),$object,
    '-lc','-lkernel','-lSceVideoOut','-lSceSysmodule','-lSceUserService'
)

# =============================================================================================
# PROGRAM AUTHORITY ID
# =============================================================================================
# Se usa el MISMO valor que en las sondas de Piglet: `0x3100000000000002`, el de RetroArch para PS4,
# que el toolchain de DolphinPS4 documenta como verificado en hardware y que ademas habilita RWX
# mprotect. Es el valor con el que un proceso homebrew tiene mas probabilidad de que el sistema le
# deje tomar el bus de VideoOut.
#
# NOTA: el camino directo de VideoOut NO necesita un display EGL, asi que el problema de
# `EGL_NO_DISPLAY` no aplica aqui; pero el PAID si puede influir en los permisos del bus.
Invoke-Checked $fself @(
    '-in',$elf,'-out',(Join-Path $build 'videoout_scaler_probe.oelf'),
    '-eboot',(Join-Path $stage 'eboot.bin'),
    '-paid','0x3100000000000002',
    '--authinfo','000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
) $build

Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libc.prx') -Destination (Join-Path $stage 'sce_module\libc.prx') -Force
Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libSceFios2.prx') -Destination (Join-Path $stage 'sce_module\libSceFios2.prx') -Force
$sample = Join-Path $toolchain 'samples\piglet'
Copy-Item -LiteralPath (Join-Path $sample 'sce_sys\about\right.sprx') -Destination (Join-Path $stage 'sce_sys\about\right.sprx') -Force
Copy-Item -LiteralPath (Join-Path $sample 'sce_sys\icon0.png') -Destination (Join-Path $stage 'sce_sys\icon0.png') -Force

$sfo = Join-Path $stage 'sce_sys\param.sfo'
Invoke-Checked $pkgTool @('sfo_new',$sfo)
$entries = @(
    @('APP_TYPE','Integer','4','1'), @('APP_VER','Utf8','8','0.1'),
    @('ATTRIBUTE','Integer','4','0'), @('CATEGORY','Utf8','4','gd'),
    @('CONTENT_ID','Utf8','48',$contentId), @('DOWNLOAD_DATA_SIZE','Integer','4','0'),
    @('SYSTEM_VER','Integer','4','0'), @('TITLE','Utf8','128','AJ VideoOut Scaler Probe'),
    @('TITLE_ID','Utf8','12',$titleId), @('VERSION','Utf8','8','0.1')
)
foreach ($entry in $entries) { Invoke-Checked $pkgTool @('sfo_setentry',$sfo,$entry[0],'--type',$entry[1],'--maxsize',$entry[2],'--value',$entry[3]) }

$gp4 = Join-Path $stage 'videoout_scaler_probe.gp4'
$fileList = 'eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx'
Invoke-Checked $gp4Tool @('-out',$gp4,'-content-id',$contentId,'-files',$fileList) $stage
Invoke-Checked $pkgTool @('pkg_build',$gp4,$build) $stage
$pkg = Join-Path $build ($contentId+'.pkg')
if (-not (Test-Path -LiteralPath $pkg)) { throw "PKG output was not created: $pkg" }
Write-Output "Built isolated VideoOut hardware-scaler probe: $pkg"
Write-Output "  Log en consola: /data/aj_videoout_scaler_probe.log"
Write-Output "  Patron en pantalla: si OCUPA TODO y esta estirado, el escalador de hardware EXISTE"
