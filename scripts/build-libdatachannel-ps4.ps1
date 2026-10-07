# Configura y compila libdatachannel para Orbis (PS4).
# Uso: pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-libdatachannel-ps4.ps1 [-SkipBuild]
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # sube de scripts/ a la raiz

. (Join-Path $root 'scripts\ps4-env.ps1') | Out-Null

$cmake = Join-Path $root 'tools\cmake-3.31.12\cmake-3.31.12-windows-x86_64\bin\cmake.exe'
$make  = (Get-ChildItem (Join-Path $root 'tools') -Recurse -Filter 'make.exe' | Select-Object -First 1).FullName
$buildDir = Join-Path $root 'build\libdatachannel-ps4'
$srcDir   = Join-Path $root 'references\libdatachannel'

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Write-Output "=== Configurando libdatachannel para Orbis ==="
Write-Output "  cmake: $cmake"
Write-Output "  make : $make"

Push-Location $buildDir
try {
    & $cmake -G 'Unix Makefiles' `
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $root 'cmake\orbis-ps4-x64.cmake')" `
        "-DCMAKE_MAKE_PROGRAM=$make" `
        -DUSE_MBEDTLS=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON `
        -DBUILD_SHARED_LIBS=OFF `
        -DPREFER_SYSTEM_LIB=OFF -DWARNINGS_AS_ERRORS=OFF `
        -DUSE_SYSTEM_SRTP=ON `
        "-DSRTP2_INCLUDE_DIRS=$(Join-Path $root 'build\libpeer-ps4\dist\include')" `
        "-DSRTP_INCLUDE_DIRS=$(Join-Path $root 'build\libpeer-ps4\dist\include')" `
        "-DSRTP_LIBRARIES=$(Join-Path $root 'build\libpeer-ps4\dist\lib\libsrtp2.a')" `
        "-DMbedTLS_ROOT=$(Join-Path $root 'build\libpeer-ps4\dist')" `
        "-DMbedTLS_INCLUDE_DIR=$(Join-Path $root 'build\libpeer-ps4\dist\include')" `
        "-DMbedTLS_LIBRARY=$(Join-Path $root 'build\libpeer-ps4\dist\lib\libmbedtls.a')" `
        "-DMbedCrypto_LIBRARY=$(Join-Path $root 'build\libpeer-ps4\dist\lib\libmbedcrypto.a')" `
        "-DMbedX509_LIBRARY=$(Join-Path $root 'build\libpeer-ps4\dist\lib\libmbedx509.a')" `
        -DUSE_SYSTEM_USRSCTP=ON `
        "-DUsrsctp_INCLUDE_DIR=$(Join-Path $root 'build\libpeer-ps4\dist\include')" `
        "-DUsrsctp_LIBRARY=$(Join-Path $root 'build\libpeer-ps4\dist\lib\libusrsctp.a')" `
        "-DCMAKE_C_FLAGS=-D__ORBIS__ -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -isystem `"$($env:OO_PS4_TOOLCHAIN)/include`"" `
        "-DCMAKE_CXX_FLAGS=-D__ORBIS__ -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -isystem `"$($env:OO_PS4_TOOLCHAIN)/include`" -isystem `"$($env:OO_PS4_TOOLCHAIN)/include/c++/v1`"" `
        "-DCMAKE_INSTALL_PREFIX=$(Join-Path $buildDir 'dist')" `
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "cmake configure fallo ($LASTEXITCODE)" }

    if (-not $SkipBuild) {
        Write-Output "=== Compilando (make) ==="
        & $make -j6
        if ($LASTEXITCODE -ne 0) { throw "make fallo ($LASTEXITCODE)" }

        # No se ejecuta 'make install': el export set de CMake no incluye plog/juice y falla,
        # pero las librerias estaticas ya estan construidas en el arbol y se usan desde alli.
        Write-Output "=== Librerias construidas (sin install, se usan del arbol) ==="
    }
} finally {
    Pop-Location
}

Write-Output "LISTO: $buildDir"
