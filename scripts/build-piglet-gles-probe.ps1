param([switch]$Clean)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$toolchain = $env:OO_PS4_TOOLCHAIN
$build = Join-Path $root 'build\research\piglet-gles-probe'
$stage = Join-Path $build 'package'
$objDir = Join-Path $build 'obj'
$llvmBin = Split-Path -Parent (Get-Command clang.exe).Source
$clang = Join-Path $llvmBin 'clang.exe'
$lld = Join-Path $llvmBin 'ld.lld.exe'
$fself = Join-Path $toolchain 'bin\windows\create-fself.exe'
$gp4Tool = Join-Path $toolchain 'bin\windows\create-gp4.exe'
$pkgTool = Join-Path $toolchain 'bin\windows\PkgTool.Core.exe'
$titleId = 'PGLT00001'
$contentId = 'IV0000-PGLT00001_00-AJGLESGPUPOC0001'
$source = Join-Path $root 'src\probes\piglet_gles_probe.c'

if ($Clean -and (Test-Path -LiteralPath $build)) { Remove-Item -LiteralPath $build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $objDir,$stage,(Join-Path $stage 'sce_sys\about'),(Join-Path $stage 'sce_module') | Out-Null

function Invoke-Checked([string]$File,[string[]]$Arguments,[string]$WorkingDirectory=$root) {
    Push-Location $WorkingDirectory
    try { & $File @Arguments; if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File $($Arguments -join ' ')" } }
    finally { Pop-Location }
}

$object = Join-Path $objDir 'piglet_gles_probe.o'
$elf = Join-Path $objDir 'piglet_gles_probe.elf'
Invoke-Checked $clang @(
    '--target=x86_64-pc-freebsd12-elf','-std=c11','-O2','-fPIC','-D_BSD_SOURCE','-D__ORBIS__=1','-c',
    '-isysroot',$toolchain,'-isystem',(Join-Path $toolchain 'include'),$source,'-o',$object
)
Invoke-Checked $lld @(
    '-m','elf_x86_64','-pie','--script',(Join-Path $toolchain 'link.x'),'-e','main','--eh-frame-hdr',
    '-L',(Join-Path $toolchain 'lib'),'-o',$elf,(Join-Path $toolchain 'lib\crt1.o'),$object,
    '-lc','-lkernel','-lScePigletv2VSH','-lScePrecompiledShaders'
)

# =============================================================================================
# PROGRAM AUTHORITY ID — AQUI ESTABA LA CAUSA DE EGL_NO_DISPLAY
# =============================================================================================
# Este valor estaba puesto a 0x3800000000000035 (el "default de PacBrew"), y el propio toolchain
# de DolphinPS4 documenta por que ese valor NO funciona con el Piglet del sistema. Cita literal de
# `references/emuladores/DolphinPS4-main/toolchain/ps4-dolphin.cmake:82-86`:
#
#     # Program authority ID of eboot.bin. The system Piglet (OpenGL ES) only gives a
#     # display to processes with a system authority ID; PacBrew's default
#     # 0x3800000000000035 gets EGL_NO_DISPLAY. This is RetroArch for PS4's value
#     # (verified on hardware by love-ps4). It also allows RWX mprotect.
#     set(PS4_PAID "0x3100000000000002" CACHE STRING "Program authority ID of eboot.bin")
#
# Y ese es EXACTAMENTE el error que teniamos registrado en el proyecto:
#
#     PIGLET_EGL_DISPLAY_TRY attempt=1 source=EGL_DEFAULT_DISPLAY eglGetError=0x3000
#     PIGLET_EGL_DISPLAY_TRY attempt=2 source=videoout_handle handle=1309737472 eglGetError=0x3000
#     PIGLET_EGL_GET_DISPLAY_FAIL rc=EGL_NO_DISPLAY both_attempts_failed videoout_open=1
#
# 0x3100000000000002 es el valor de RetroArch para PS4, VERIFICADO EN HARDWARE segun la nota. El
# 0x3800000000000035 es de otra clase de proceso y el Piglet del sistema le niega el display.
#
# Se usa el MISMO valor y el MISMO authinfo que DolphinPS4 (ps4-dolphin.cmake:86 y :90).
Invoke-Checked $fself @(
    '-in',$elf,'-out',(Join-Path $build 'piglet_gles_probe.oelf'),
    '-eboot',(Join-Path $stage 'eboot.bin'),
    '-paid','0x3100000000000002',
    '--authinfo','000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
) $build

Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libc.prx') -Destination (Join-Path $stage 'sce_module\libc.prx') -Force
Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libSceFios2.prx') -Destination (Join-Path $stage 'sce_module\libSceFios2.prx') -Force
$pigletSample = Join-Path $toolchain 'samples\piglet'
Copy-Item -LiteralPath (Join-Path $pigletSample 'sce_sys\about\right.sprx') -Destination (Join-Path $stage 'sce_sys\about\right.sprx') -Force
Copy-Item -LiteralPath (Join-Path $pigletSample 'sce_sys\icon0.png') -Destination (Join-Path $stage 'sce_sys\icon0.png') -Force

$sfo = Join-Path $stage 'sce_sys\param.sfo'
Invoke-Checked $pkgTool @('sfo_new',$sfo)
$entries = @(
    @('APP_TYPE','Integer','4','1'), @('APP_VER','Utf8','8','0.1'),
    @('ATTRIBUTE','Integer','4','0'), @('CATEGORY','Utf8','4','gd'),
    @('CONTENT_ID','Utf8','48',$contentId), @('DOWNLOAD_DATA_SIZE','Integer','4','0'),
    @('SYSTEM_VER','Integer','4','0'), @('TITLE','Utf8','128','AJ Piglet GLES Probe'),
    @('TITLE_ID','Utf8','12',$titleId), @('VERSION','Utf8','8','0.1')
)
foreach ($entry in $entries) { Invoke-Checked $pkgTool @('sfo_setentry',$sfo,$entry[0],'--type',$entry[1],'--maxsize',$entry[2],'--value',$entry[3]) }

$gp4 = Join-Path $stage 'piglet_gles_probe.gp4'
$fileList = 'eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx'
Invoke-Checked $gp4Tool @('-out',$gp4,'-content-id',$contentId,'-files',$fileList) $stage
Invoke-Checked $pkgTool @('pkg_build',$gp4,$build) $stage
$pkg = Join-Path $build ($contentId+'.pkg')
if (-not (Test-Path -LiteralPath $pkg)) { throw "PKG output was not created: $pkg" }
Write-Output "Built isolated Piglet EGL/GLES2 probe: $pkg"
