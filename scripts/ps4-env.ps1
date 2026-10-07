$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$toolchainRoot = Join-Path $projectRoot 'tools\openorbis\OpenOrbis\PS4Toolchain'
$llvmBin = Join-Path $projectRoot 'tools\llvm-mingw-20260922-msvcrt-x86_64\llvm-mingw-20260922-msvcrt-x86_64\bin'
$dotnetRoot = Join-Path $projectRoot 'tools\dotnet'

foreach ($path in @($toolchainRoot, $llvmBin, $dotnetRoot)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required PS4 build tool path not found: $path"
    }
}

$env:OO_PS4_TOOLCHAIN = $toolchainRoot
$env:DOTNET_ROOT = $dotnetRoot
$env:Path = "$llvmBin;$dotnetRoot;$env:Path"

Write-Output "OO_PS4_TOOLCHAIN=$env:OO_PS4_TOOLCHAIN"
Write-Output "DOTNET_ROOT=$env:DOTNET_ROOT"
& (Join-Path $llvmBin 'clang.exe') --version | Select-Object -First 1
& (Join-Path $dotnetRoot 'dotnet.exe') --list-runtimes
