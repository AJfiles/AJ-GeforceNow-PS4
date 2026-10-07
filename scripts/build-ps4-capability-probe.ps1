# Builds the standalone PS4 capability probe as its own PKG.
#
# The probe is deliberately separate from the GFN client: it must be safe to run
# without a streaming session, and it must not touch the client's VideoOut or
# network state. Modeled on build-piglet-sdl-compositor-probe.ps1.
param([switch]$Clean)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$toolchain = $env:OO_PS4_TOOLCHAIN
$build = Join-Path $root 'build\research\ps4-capability-probe'
$stage = Join-Path $build 'package'
$objDir = Join-Path $build 'obj'
$llvmBin = Split-Path -Parent (Get-Command clang.exe).Source
$clang = Join-Path $llvmBin 'clang.exe'
$lld = Join-Path $llvmBin 'ld.lld.exe'
$fself = Join-Path $toolchain 'bin\windows\create-fself.exe'
$gp4Tool = Join-Path $toolchain 'bin\windows\create-gp4.exe'
$pkgTool = Join-Path $toolchain 'bin\windows\PkgTool.Core.exe'
$titleId = 'CAPP00001'
$contentId = 'IV0000-CAPP00001_00-AJCAPPROBEP00001'
$source = Join-Path $root 'src\probes\ps4_capability_probe.cpp'

if ($titleId -notmatch '^[A-Z]{4}[0-9]{5}$') { throw "Invalid PS4 Title ID: $titleId" }
if ($contentId -notmatch "^IV0000-$titleId`_00-[A-Z0-9]{16}$") { throw "Invalid PS4 Content ID: $contentId" }

if ($Clean -and (Test-Path -LiteralPath $build)) { Remove-Item -LiteralPath $build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $objDir,$stage,(Join-Path $stage 'sce_sys\about'),(Join-Path $stage 'sce_module') | Out-Null

function Invoke-Checked([string]$File,[string[]]$Arguments,[string]$WorkingDirectory=$root) {
    Push-Location $WorkingDirectory
    try { & $File @Arguments; if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File $($Arguments -join ' ')" } }
    finally { Pop-Location }
}

$object = Join-Path $objDir 'ps4_capability_probe.o'
$elf = Join-Path $objDir 'ps4_capability_probe.elf'
# Compiled as C++ (like the main client): the probe uses <cstdarg>/<cstdint> and an
# extern "C" declaration, none of which exist in a C-only translation unit.
Invoke-Checked $clang @(
    '--target=x86_64-pc-freebsd12-elf','-x','c++','-std=c++20','-O2','-fPIC','-D_BSD_SOURCE','-D__ORBIS__=1','-c',
    '-isysroot',$toolchain,'-isystem',(Join-Path $toolchain 'include'),
    '-isystem',(Join-Path $toolchain 'include\c++\v1'),
    '-isystem',(Join-Path $toolchain 'include\SDL2'),$source,'-o',$object
)
Invoke-Checked $lld @(
    '-m','elf_x86_64','-pie','--script',(Join-Path $toolchain 'link.x'),'-e','main','--eh-frame-hdr',
    '-L',(Join-Path $toolchain 'lib'),'-o',$elf,(Join-Path $toolchain 'lib\crt1.o'),$object,
    '-lc','-lkernel','-lc++','-lm',
    '-lSDL2',
    # SDL2's PS4 backend archive references these even when only SDL_INIT_EVENTS is
    # requested, so they must be present at link time.
    '-lSceAudioOut','-lScePad',
    '-lSceSysmodule','-lSceUserService','-lSceVideoOut','-lSceNet','-lSceNetCtl','-lSceGnmDriver',
    # HTTP + SSL for the throughput measurement.
    '-lSceHttp','-lSceSsl'
)
Invoke-Checked $fself @('-in',$elf,'-out',(Join-Path $build 'ps4_capability_probe.oelf'),'-eboot',(Join-Path $stage 'eboot.bin'),'-paid','0x3800000000000055') $build

Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libc.prx') -Destination (Join-Path $stage 'sce_module\libc.prx') -Force
Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libSceFios2.prx') -Destination (Join-Path $stage 'sce_module\libSceFios2.prx') -Force
Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_sys\about\right.sprx') -Destination (Join-Path $stage 'sce_sys\about\right.sprx') -Force
Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_sys\icon0.png') -Destination (Join-Path $stage 'sce_sys\icon0.png') -Force

$sfo = Join-Path $stage 'sce_sys\param.sfo'
Invoke-Checked $pkgTool @('sfo_new',$sfo)
$entries = @(
    @('APP_TYPE','Integer','4','1'), @('APP_VER','Utf8','8','0.1'),
    @('ATTRIBUTE','Integer','4','0'), @('CATEGORY','Utf8','4','gd'),
    @('CONTENT_ID','Utf8','48',$contentId), @('DOWNLOAD_DATA_SIZE','Integer','4','0'),
    @('SYSTEM_VER','Integer','4','0'), @('TITLE','Utf8','128','AJ PS4 Capability Probe'),
    @('TITLE_ID','Utf8','12',$titleId), @('VERSION','Utf8','8','0.1')
)
foreach ($entry in $entries) { Invoke-Checked $pkgTool @('sfo_setentry',$sfo,$entry[0],'--type',$entry[1],'--maxsize',$entry[2],'--value',$entry[3]) }

$gp4 = Join-Path $stage 'ps4_capability_probe.gp4'
$fileList = 'eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx'
Invoke-Checked $gp4Tool @('-out',$gp4,'-content-id',$contentId,'-files',$fileList) $stage
Invoke-Checked $pkgTool @('pkg_build',$gp4,$build) $stage

$pkg = Join-Path $build ($contentId+'.pkg')
if (-not (Test-Path -LiteralPath $pkg)) { throw "PKG output was not created: $pkg" }
Write-Output "Built PS4 capability probe: $pkg"
Write-Output "Run it on the console, then copy /data/gfnps4/probe_report.txt off and hand it over."
