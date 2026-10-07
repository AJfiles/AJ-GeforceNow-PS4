$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null

$root = Split-Path -Parent $PSScriptRoot
$opengnm = Join-Path $root 'src\third_party\opengnm'
$build = Join-Path $root 'build\opengnm-ps4'
$toolchain = $env:OO_PS4_TOOLCHAIN
$llvmBin = Split-Path -Parent (Get-Command clang.exe).Source
$clang = Join-Path $llvmBin 'clang.exe'
$ar = Join-Path $llvmBin 'llvm-ar.exe'
if (-not (Test-Path -LiteralPath (Join-Path $opengnm 'LICENSE'))) {
    throw 'Vendored OpenGNM source or MIT license is missing.'
}
New-Item -ItemType Directory -Force -Path $build | Out-Null

$sources = @(
    'src/drawcommandbuffer.c','src/rendertarget.c','src/depthrendertarget.c','src/texture.c',
    'src/shader.c','src/dataformat.c','src/commandbuffer.c','src/error.c','src/helpers.c',
    'src/driver_orbis.c','src/platform_orbis.c',
    'src/gpuaddr/surface.c','src/gpuaddr/tilemodes.c','src/gpuaddr/tiler.c',
    'src/gpuaddr/decompress.c','src/gpuaddr/surfgen.c','src/gpuaddr/error.c',
    'src/gcn/analyzer.c','src/gcn/assembler.c','src/gcn/decoder.c','src/gcn/error.c',
    'src/gcn/format.c','src/gcn/types.c',
    'src/pm4/format.c','src/pm4/decoder.c','src/pm4/error.c','src/pm4/types.c'
)
$objects = [System.Collections.Generic.List[string]]::new()
foreach ($relative in $sources) {
    $input = Join-Path $opengnm $relative
    $object = Join-Path $build (($relative -replace '[\\/]','_') + '.o')
    & $clang '--target=x86_64-pc-freebsd12-elf' '-std=c11' '-O2' '-fPIC' `
        '-DOPENGNM_ORBIS' '-DOPENGNM_REQUIRE_ABI' '-isysroot' $toolchain `
        '-isystem' (Join-Path $toolchain 'include') `
        '-I' (Join-Path $opengnm 'include') '-I' (Join-Path $opengnm 'src') `
        '-c' $input '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "OpenGNM compile failed: $relative" }
    $objects.Add($object)
}

$archive = Join-Path $build 'libopengnm.a'
& $ar 'rcs' $archive $objects.ToArray()
if ($LASTEXITCODE -ne 0) { throw 'OpenGNM archive creation failed.' }
Write-Output "Built OpenGNM for PS4: $archive"
