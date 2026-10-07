param([switch]$Clean)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$stack = Join-Path $root 'build\research\opengnm-stack'
$project = Join-Path $stack 'opengnm'
$assets = Join-Path $stack 'freegnm-examples\videoout-linear'
$build = Join-Path $root 'build\research\opengnm-gpu-smoke'
$stage = Join-Path $build 'package'
$toolchain = $env:OO_PS4_TOOLCHAIN
$llvmBin = Split-Path -Parent (Get-Command clang.exe).Source
$clang = Join-Path $llvmBin 'clang.exe'
$lld = Join-Path $llvmBin 'ld.lld.exe'
$fself = Join-Path $toolchain 'bin\windows\create-fself.exe'
$gp4Tool = Join-Path $toolchain 'bin\windows\create-gp4.exe'
$pkgTool = Join-Path $toolchain 'bin\windows\PkgTool.Core.exe'
$titleId = 'OGNM00001'
$contentId = 'IV0000-OGNM00001_00-OPENGNMHWSMOKE00'

& (Join-Path $PSScriptRoot 'build-opengnm-ps4.ps1')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $root 'build\opengnm-ps4\libopengnm.a'))) {
    throw 'Building the vendored OpenGNM library failed.'
}
if ($Clean -and (Test-Path -LiteralPath $build)) {
    Remove-Item -LiteralPath $build -Recurse -Force
}
$objDir = Join-Path $build 'obj'
New-Item -ItemType Directory -Force -Path $objDir,$stage,(Join-Path $stage 'sce_sys\about'),(Join-Path $stage 'sce_module') | Out-Null

function Invoke-Checked([string]$File, [string[]]$Arguments, [string]$WorkingDirectory=$root) {
    Push-Location $WorkingDirectory
    try {
        & $File @Arguments
        if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File $($Arguments -join ' ')" }
    } finally { Pop-Location }
}

$source = Join-Path $project 'tests\hardware_smoke.c'
$object = Join-Path $objDir 'hardware_smoke.o'
$elf = Join-Path $objDir 'opengnm_hw_smoke.elf'
Invoke-Checked $clang @(
    '--target=x86_64-pc-freebsd12-elf','-std=c11','-O2','-fPIC','-D_BSD_SOURCE','-D__ORBIS__=1','-c',
    '-isysroot',$toolchain,'-isystem',(Join-Path $toolchain 'include'),
    '-I',(Join-Path $project 'include'),'-I',(Join-Path $project 'src'),$source,'-o',$object
)
Invoke-Checked $lld @(
    '-m','elf_x86_64','-pie','--script',(Join-Path $toolchain 'link.x'),'-e','main','--eh-frame-hdr',
    '-L',(Join-Path $toolchain 'lib'),'-o',$elf,(Join-Path $toolchain 'lib\crt1.o'),$object,
    (Join-Path $root 'build\opengnm-ps4\libopengnm.a'),'-lc','-lkernel','-lSceGnmDriver','-lSceVideoOut'
)
Invoke-Checked $fself @('-in',$elf,'-out',(Join-Path $build 'opengnm_hw_smoke.oelf'),'-eboot',(Join-Path $stage 'eboot.bin'),'-paid','0x3800000000000011') $build

Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libc.prx') -Destination (Join-Path $stage 'sce_module\libc.prx') -Force
Copy-Item -LiteralPath (Join-Path $toolchain 'samples\SDL2\sce_module\libSceFios2.prx') -Destination (Join-Path $stage 'sce_module\libSceFios2.prx') -Force
Copy-Item -LiteralPath (Join-Path $assets 'sce_sys\about\right.sprx') -Destination (Join-Path $stage 'sce_sys\about\right.sprx') -Force
Copy-Item -LiteralPath (Join-Path $assets 'sce_sys\icon0.png') -Destination (Join-Path $stage 'sce_sys\icon0.png') -Force
Copy-Item -LiteralPath (Join-Path $project 'LICENSE') -Destination (Join-Path $stage 'OPENGNM-LICENSE.txt') -Force

$sfo = Join-Path $stage 'sce_sys\param.sfo'
Invoke-Checked $pkgTool @('sfo_new',$sfo)
$entries = @(
    @('APP_TYPE','Integer','4','1'), @('APP_VER','Utf8','8','0.1'),
    @('ATTRIBUTE','Integer','4','0'), @('CATEGORY','Utf8','4','gd'),
    @('CONTENT_ID','Utf8','48',$contentId), @('DOWNLOAD_DATA_SIZE','Integer','4','0'),
    @('SYSTEM_VER','Integer','4','0'), @('TITLE','Utf8','128','AJ OpenGNM GPU Probe'),
    @('TITLE_ID','Utf8','12',$titleId), @('VERSION','Utf8','8','0.1')
)
foreach ($entry in $entries) {
    Invoke-Checked $pkgTool @('sfo_setentry',$sfo,$entry[0],'--type',$entry[1],'--maxsize',$entry[2],'--value',$entry[3])
}

$gp4 = Join-Path $stage 'opengnm_hw_smoke.gp4'
$fileList = 'eboot.bin OPENGNM-LICENSE.txt sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx'
Invoke-Checked $gp4Tool @('-out',$gp4,'-content-id',$contentId,'-files',$fileList) $stage
Invoke-Checked $pkgTool @('pkg_build',$gp4,$build) $stage
$pkg = Join-Path $build ($contentId+'.pkg')
if (-not (Test-Path -LiteralPath $pkg)) { throw "PKG output was not created: $pkg" }
Write-Output "Built isolated OpenGNM GPU probe: $pkg"
