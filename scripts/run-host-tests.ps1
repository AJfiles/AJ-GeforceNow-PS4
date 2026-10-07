# Compila y ejecuta las pruebas de host del proyecto.
#
# QUE SON LAS PRUEBAS DE HOST
# ---------------------------
# Son programas que se compilan y ejecutan EN EL PC, no en la PS4. Sirven para verificar la LOGICA que
# no depende del hardware: la rejilla del menu, el parseo de JSON, el QR. Corren en segundos y no
# necesitan consola, asi que se pueden ejecutar en cada build.
#
# POR QUE `-static`
# ----------------
# Los ejecutables de mingw necesitan sus DLLs en el PATH, que no lo estan. Con enlazado estatico el
# binario es autonomo y se ejecuta siempre. Sin esto, la prueba muere con 0xC0000135
# (STATUS_DLL_NOT_FOUND) y parece que "falla" cuando en realidad no llega a arrancar.
#
# USO
# ---
#     pwsh -File scripts/run-host-tests.ps1
#
# Devuelve 0 si todas pasan, 1 si alguna falla.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# El compilador de host: el de x86_64 (NO el i686, que es para el toolchain de 32 bits).
$clang = Get-ChildItem (Join-Path $root 'tools') -Recurse -File -Filter 'clang++.exe' -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -notmatch 'i686' } | Select-Object -First 1
if (-not $clang) { throw 'No se encontro clang++ de host en tools/' }

$outDir = Join-Path $root 'build\host-tests'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# TMP dentro del proyecto: el temporal del sistema puede no ser escribible desde el sandbox.
$env:TMP = Join-Path $root 'build\tmp'
$env:TEMP = $env:TMP
New-Item -ItemType Directory -Force -Path $env:TMP | Out-Null

# Cada prueba: nombre visible y fichero fuente.
#
# OJO CON LA ESTRUCTURA: un array de arrays en PowerShell se APLANA, asi que `$t[0]` devolvia el primer
# CARACTER en lugar del primer elemento (salio "[FALTA] R"). Se usan objetos con nombre de campo, que
# no tienen ese problema.
$tests = @(
    [pscustomobject]@{ Name = 'Rejilla y navegacion del centro de juego'; File = 'ps4_menu_grid_test.cpp' }
    # La segunda prueba necesita ademas `color_simd.cpp` (el conversor vive ahi). Se declara su fuente
    # extra en `Extra`; las pruebas que no la necesiten lo dejan vacio.
    [pscustomobject]@{
        Name  = 'Conversor de color de la ruta de video (NV12/YUV420P -> BGRA)'
        File  = 'video_color_path_test.cpp'
        Extra = 'src\opennow\stream\color_simd.cpp'
    }
    # Esta no necesita `color_simd.cpp`: comprueba la LOGICA DEL DESTINO (que el video se estire
    # siempre al lienzo y no al tamanio del frame). Es la prueba que impide que vuelva el fallo del
    # video encogido en la esquina con la resolucion dinamica de GeForce NOW.
    [pscustomobject]@{ Name = 'Blit del video: 1:1 con escalado en la conversion'; File = 'video_destination_test.cpp' }
    # Esta comprueba que el blit del MENU EN PARTIDA no escribe fuera del framebuffer. Usa canarios
    # alrededor del buffer en vez de calcular indices a mano: un byte de canario cambiado es prueba de
    # escritura fuera, sin depender de que la aritmetica del test sea correcta.
    [pscustomobject]@{ Name = 'Blit del menu en partida: sin escrituras fuera del framebuffer'; File = 'menu_overlay_clip_test.cpp' }
    # ESTA ES LA PRUEBA DEL ERROR DE LA v4.15, y por eso existe.
    #
    # En la v4.15 sustitui `SDL_RenderCopy` por un `memcpy` por filas razonando que "el orden de bytes ya
    # era el correcto". **Era falso**: `SDL_RenderCopy` **convertia** de formato, y esa conversion era la
    # que dejaba los bytes en el orden que el driver necesita. El usuario lo vio en pantalla (colores
    # alterados).
    #
    # La correccion deshace el intercambio R<->B dentro de la copia, con SSE2. **Razonar sobre rotaciones
    # de bits es justo lo que ha estado mal varias veces en este proyecto**, asi que aqui se comprueba
    # byte a byte contra una referencia escalar escrita de otra forma, con el camino vectorizado y el de
    # cola, y con la propiedad de que la rotacion es su propia inversa.
    [pscustomobject]@{ Name = 'Copia por filas: rotacion R<->B exacta (SSE2 vs referencia)'; File = 'video_row_swap_test.cpp' }
)

$fail = 0
Write-Host ''
Write-Host '  PRUEBAS DE HOST' -ForegroundColor Cyan

foreach ($t in $tests) {
    $name = $t.Name
    $src  = Join-Path $root ('tests\' + $t.File)
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Host ("    [FALTA] {0} ({1} no existe)" -f $name, $t.File) -ForegroundColor Red
        $fail++
        continue
    }
    $exe = Join-Path $outDir ([System.IO.Path]::GetFileNameWithoutExtension($t.File) + '.exe')
    # Fuentes adicionales opcionales (por ejemplo color_simd.cpp para la prueba del conversor).
    $extraSources = @()
    if ($t.PSObject.Properties.Name -contains 'Extra' -and $t.Extra) {
        $extraSources += (Join-Path $root $t.Extra)
    }
    & $clang.FullName '-std=c++20' '-O1' '-static' '-I' $root $src $extraSources '-o' $exe 2>&1 | ForEach-Object { Write-Host ("      {0}" -f $_) }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
        Write-Host ("    [FALLO] {0}: no compila" -f $name) -ForegroundColor Red
        $fail++
        continue
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("    [FALLO] {0}" -f $name) -ForegroundColor Red
        $fail++
    } else {
        Write-Host ("    [OK]    {0}" -f $name) -ForegroundColor Green
    }
}

Write-Host ''
if ($fail -eq 0) {
    Write-Host '  Todas las pruebas de host pasaron' -ForegroundColor Green
    exit 0
}
Write-Host ("  {0} prueba(s) FALLARON" -f $fail) -ForegroundColor Red
exit 1
