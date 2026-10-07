param([switch]$Clean)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'src\third_party\ffmpeg'
$llvm = Join-Path $root 'tools\llvm-mingw-20260922-msvcrt-x86_64\llvm-mingw-20260922-msvcrt-x86_64\bin'
$busybox = Join-Path $root 'tools\llvm-mingw-20260922-msvcrt-x86_64\llvm-mingw-20260922-msvcrt-x86_64\busybox\bin'
$bash = Join-Path $root 'tools\PortableGit\usr\bin\bash.exe'
$cygpath = Join-Path $root 'tools\PortableGit\usr\bin\cygpath.exe'
$make = Join-Path $busybox 'make.exe'
$toolchain = $env:OO_PS4_TOOLCHAIN
$dist = Join-Path $root 'build\libpeer-ps4\dist'
$libDir = Join-Path $dist 'lib'

foreach ($required in @($source,$bash,$cygpath,$make,$toolchain)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required media build path not found: $required" }
}

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class GfnPs4ShortPath {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern uint GetShortPathName(string longPath, StringBuilder shortPath, uint bufferLength);
}
'@
function Get-ShortPath([string]$Path) {
    $buffer = [System.Text.StringBuilder]::new(1024)
    [void][GfnPs4ShortPath]::GetShortPathName($Path,$buffer,1024)
    if ($buffer.Length -eq 0) { throw "Unable to resolve short path: $Path" }
    return $buffer.ToString()
}

$sourceShort = Get-ShortPath $source
$toolchainShort = Get-ShortPath $toolchain
$rootShort = Get-ShortPath $root
# PortableGit's cygpath can fail to create its user mapping inside restricted
# Windows processes. Git Bash mounts drive roots as /c, /d, etc.; convert the
# already-resolved short Windows path directly for FFmpeg's configure script.
$sysroot = '/' + $toolchainShort.Substring(0,1).ToLowerInvariant() + $toolchainShort.Substring(2).Replace('\','/')
$sysrootWindows = $toolchainShort.Replace('\','/')
$env:Path = "$llvm;$busybox;$env:Path"
$env:CFLAGS = "--target=x86_64-pc-freebsd12-elf -isystem $sysrootWindows/include -fPIC -funwind-tables -O3 -march=btver2"
$env:LDFLAGS = "--target=x86_64-pc-freebsd12-elf -nostdlib -L$sysroot/lib -Wl,-e,main -lkernel -lc -lm"

Push-Location $sourceShort
try {
    if ($Clean) {
        & $make clean
        if ($LASTEXITCODE -ne 0) { throw "FFmpeg clean failed ($LASTEXITCODE)" }
    }
    & $bash './configure' `
        '--cc=clang' '--ld=clang' '--ar=llvm-ar' '--ranlib=llvm-ranlib' '--nm=llvm-nm' `
        "--prefix=$rootShort/build/libpeer-ps4/dist" `
        '--target-os=freebsd' '--arch=x86_64' '--cpu=btver2' '--enable-cross-compile' `
        '--enable-static' '--disable-shared' '--disable-programs' '--disable-doc' '--disable-debug' `
        '--disable-autodetect' '--disable-network' '--disable-everything' '--enable-avcodec' '--enable-avutil' `
        '--enable-decoder=h264' '--enable-decoder=mjpeg' '--enable-decoder=png' `
        '--enable-parser=h264' '--enable-demuxer=h264' '--enable-protocol=file' `
        '--disable-x86asm' '--disable-asm' '--enable-pic'
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg configure failed ($LASTEXITCODE)" }

    $configHeader = Join-Path $source 'config.h'
    $configText = [IO.File]::ReadAllText($configHeader)
    $configText = $configText.Replace('#define HAVE_SYSCTL 1','#define HAVE_SYSCTL 0')
    $configText = $configText.Replace('#define CONFIG_RUNTIME_CPUDETECT 1','#define CONFIG_RUNTIME_CPUDETECT 0')
    [IO.File]::WriteAllText($configHeader,$configText,[Text.UTF8Encoding]::new($false))

    & $make -j3 'libavcodec/libavcodec.a' 'libavutil/libavutil.a'
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg H.264 build failed ($LASTEXITCODE)" }
    & $make install-headers
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg header install failed ($LASTEXITCODE)" }
} finally { Pop-Location }

New-Item -ItemType Directory -Force -Path $libDir | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'libavcodec\libavcodec.a') -Destination (Join-Path $libDir 'libavcodec.a') -Force
Copy-Item -LiteralPath (Join-Path $source 'libavutil\libavutil.a') -Destination (Join-Path $libDir 'libavutil.a') -Force
Write-Output "Built PS4 H.264 decoder: $(Join-Path $libDir 'libavcodec.a')"
