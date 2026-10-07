param([switch]$Clean)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ps4-env.ps1') | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$cmake = Join-Path $root 'tools\cmake-3.31.12\cmake-3.31.12-windows-x86_64\bin\cmake.exe'
$busybox = Join-Path $root 'tools\llvm-mingw-20260922-msvcrt-x86_64\llvm-mingw-20260922-msvcrt-x86_64\busybox\bin'
$gitTools = Join-Path $root 'tools\PortableGit\usr\bin'
$make = Join-Path $busybox 'make.exe'
$libpeer = Join-Path $root 'src\third_party\libpeer-orbis'
$jansson = Join-Path $root 'src\third_party\jansson'
$opus = Join-Path $root 'src\third_party\opus'
$toolchain = Join-Path $PSScriptRoot 'cmake\orbis-ps4-x64.cmake'
$build = Join-Path $root 'build\libpeer-ps4'
$pythonRuntime = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python'

foreach ($required in @($cmake,$make,$libpeer,$jansson,$opus,$toolchain)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required port dependency not found: $required" }
}
$env:Path = "$busybox;$gitTools;$env:Path"
$env:PYTHONPATH = Join-Path $root 'tools\python-deps'
if ($Clean -and (Test-Path -LiteralPath $build)) {
    Remove-Item -LiteralPath $build -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $build | Out-Null

# Mbed TLS generates a small source file with the host Python interpreter.
$pythonShimDir = Join-Path $build 'host-python'
New-Item -ItemType Directory -Force -Path $pythonShimDir | Out-Null
if (Test-Path -LiteralPath (Join-Path $pythonRuntime 'python.exe')) {
    Copy-Item -LiteralPath (Join-Path $pythonRuntime 'python.exe') -Destination (Join-Path $pythonShimDir 'python3.exe') -Force
    $env:PYTHONHOME = $pythonRuntime
    $env:GFN_PS4_HOST_PYTHON = Join-Path $pythonRuntime 'python.exe'
    $env:Path = "$pythonShimDir;$pythonRuntime;$env:Path"
} else {
    throw "Host Python runtime not found: $pythonRuntime"
}

$janssonBuild = Join-Path $build 'jansson'
& $cmake -S $jansson -B $janssonBuild -G 'Unix Makefiles' `
    "-DCMAKE_MAKE_PROGRAM=$make" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_INSTALL_PREFIX=$(Join-Path $build 'dist')" `
    '-DCMAKE_BUILD_TYPE=Release' `
    '-DJANSSON_BUILD_SHARED_LIBS=OFF' '-DJANSSON_EXAMPLES=OFF' `
    '-DJANSSON_BUILD_DOCS=OFF' '-DJANSSON_WITHOUT_TESTS=ON' `
    '-DUSE_URANDOM=OFF' '-DUSE_DTOA=OFF'
if ($LASTEXITCODE -ne 0) { throw "Jansson PS4 configure failed ($LASTEXITCODE)" }
& $cmake --build $janssonBuild --target install --parallel 3
if ($LASTEXITCODE -ne 0) { throw "Jansson PS4 build failed ($LASTEXITCODE)" }

$opusBuild = Join-Path $build 'opus'
& $cmake -S $opus -B $opusBuild -G 'Unix Makefiles' `
    "-DCMAKE_MAKE_PROGRAM=$make" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_INSTALL_PREFIX=$(Join-Path $build 'dist')" `
    '-DCMAKE_BUILD_TYPE=Release' `
    '-DOPUS_BUILD_SHARED_LIBRARY=OFF' '-DOPUS_BUILD_TESTING=OFF' `
    '-DOPUS_BUILD_PROGRAMS=OFF' '-DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF' `
    '-DOPUS_DISABLE_INTRINSICS=OFF' '-DOPUS_HARDENING=ON'
if ($LASTEXITCODE -ne 0) { throw "Opus PS4 configure failed ($LASTEXITCODE)" }
& $cmake --build $opusBuild --target install --parallel 3
if ($LASTEXITCODE -ne 0) { throw "Opus PS4 build failed ($LASTEXITCODE)" }

& $cmake -S $libpeer -B $build -G 'Unix Makefiles' `
    "-DCMAKE_MAKE_PROGRAM=$make" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    '-DBUILD_SHARED_LIBS=OFF' '-DENABLE_TESTS=OFF' '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE -ne 0) { throw "libpeer CMake configure failed ($LASTEXITCODE)" }

& $cmake --build $build --target peer --parallel 3
if ($LASTEXITCODE -ne 0) { throw "libpeer PS4 build failed ($LASTEXITCODE)" }

$archive = Join-Path $build 'src\libpeer.a'
if (-not (Test-Path -LiteralPath $archive)) { throw "libpeer archive was not produced: $archive" }
Write-Output "Built PS4 native WebRTC dependency: $archive"
