param([switch]$Clean)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$cmake = Join-Path $root 'tools\cmake-3.31.12\cmake-3.31.12-windows-x86_64\bin\cmake.exe'
$busybox = Join-Path $root 'tools\llvm-mingw-20260922-msvcrt-x86_64\llvm-mingw-20260922-msvcrt-x86_64\busybox\bin'
$gitTools = Join-Path $root 'tools\PortableGit\usr\bin'
$make = Join-Path $busybox 'make.exe'
$toolchain = Join-Path $PSScriptRoot 'cmake\orbis-ps4-x64.cmake'
$source = Join-Path $root 'src\opennow\ps4'
$build = Join-Path $root 'build\gfn-client-ps4'
$nativeDeps = Join-Path $root 'build\libpeer-ps4\dist'

if ($Clean -and (Test-Path -LiteralPath $build)) {
    Remove-Item -LiteralPath $build -Recurse -Force
}

if (-not (Test-Path -LiteralPath (Join-Path $nativeDeps 'lib\libjansson.a')) -or
    -not (Test-Path -LiteralPath (Join-Path $nativeDeps 'lib\libopus.a'))) {
    & (Join-Path $PSScriptRoot 'build-libpeer-ps4.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Building native PS4 dependencies failed' }
}
$env:Path = "$busybox;$gitTools;$env:Path"
& $cmake -S $source -B $build -G 'Unix Makefiles' `
    "-DCMAKE_MAKE_PROGRAM=$make" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DPS4_PROJECT_ROOT=$root" '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE -ne 0) { throw "GFN client CMake configure failed ($LASTEXITCODE)" }
& $cmake --build $build --target gfn_client_ps4 --parallel 3
if ($LASTEXITCODE -ne 0) { throw "GFN client PS4 build failed ($LASTEXITCODE)" }
Write-Output "Built GFN account/catalog/session client: $(Join-Path $build 'libgfn_client_ps4.a')"
$param
