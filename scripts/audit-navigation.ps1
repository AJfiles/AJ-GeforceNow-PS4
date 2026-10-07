# Auditoria de coherencia entre el DIBUJADO y la NAVEGACION del centro de juego.
#
# POR QUE EXISTE
# --------------
# En la v3.31 aparecieron TRES fallos del menu que ninguna auditoria detecto, y en la v3.36 DOS mas
# reportados desde la consola:
#
#   v3.31: tablas paralelas con nullptr; numero de tarjetas escrito a mano en tres sitios; filas mal
#          contadas en la navegacion vertical.
#   v3.36: "ACERCA DE" DESAPARECIO del menu (su rama del dispatch pedia indices que ya no existian),
#          y el foco se iluminaba en una tarjeta distinta de la seleccionada.
#
# La causa comun de los cinco: EL DIBUJADO Y EL DISPATCH USABAN LISTAS DISTINTAS. Esta auditoria
# comprueba que eso no pueda volver a pasar.
#
# QUE COMPRUEBA
# -------------
#   A) Existe UNA sola tabla de tarjetas (`buildHomeCards`) y el dibujado la usa.
#   B) El dispatch de la X usa la MISMA tabla, no una cadena de `selection==N`.
#   C) El numero de tarjetas es UNA constante y coincide con el numero de entradas.
#   D) Cada accion declarada en el enum tiene su rama en el `switch`.
#   E) La navegacion vertical usa las filas reales (kHomeRows) y no un numero suelto.
#   F) La rejilla es coherente: columnas x filas == numero de tarjetas.
#   G) El hit-test del raton usa la MISMA rejilla que el dibujado.
#   H) Existe el log de verificacion del foco (MENU_FOCUS / MENU_NAV).
#   I) No queda ningun `homeCardCount` suelto que pueda desincronizarse.
#
# Devuelve 0 si todo pasa, 1 si algo falla.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'src\ps4\main.cpp'

if (-not (Test-Path -LiteralPath $source)) {
    Write-Host "No existe $source" -ForegroundColor Red
    exit 2
}
$text = Get-Content -LiteralPath $source -Raw
$lines = Get-Content -LiteralPath $source

# CODIGO SIN LINEAS DE COMENTARIO.
#
# POR QUE HACE FALTA: los comentarios de este fichero EXPLICAN los fallos que se corrigieron y citan
# literalmente los patrones prohibidos ("estaba con ys[]={265,440,615,790}", "tenia una rama especial
# else if(nextRow==2) selection=4", "esto era una funcion homeCardCount()"). Buscando sobre el texto
# completo, la propia documentacion del fallo hacia saltar la comprobacion que lo vigila.
#
# SE QUITAN SOLO LAS LINEAS QUE EMPIEZAN POR // (tras espacios). Es deliberadamente conservador:
# intentar quitar tambien los comentarios de bloque con una expresion regular SE COMIO MEDIO FICHERO
# (403.388 -> 189.326 caracteres), porque `/*` aparece dentro de cadenas de texto. Una auditoria que
# analiza codigo equivocado es peor que no tenerla: aqui se sacrifica cobertura (un comentario al final
# de una linea de codigo se sigue viendo) a cambio de no romper el analisis.
$code = ($lines | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"

$fail = 0
function Check($name, $ok, $detail) {
    if ($ok) {
        Write-Host ("    [OK]    {0}" -f $name) -ForegroundColor Green
    } else {
        Write-Host ("    [FALLO] {0}" -f $name) -ForegroundColor Red
        if ($detail) { Write-Host ("            {0}" -f $detail) -ForegroundColor DarkGray }
        $script:fail++
    }
}
function Val($pattern) {
    $m = [regex]::Match($code, $pattern)
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

Write-Host ""
Write-Host "  AUDITORIA DE NAVEGACION Y DIBUJADO (centro de juego)" -ForegroundColor Cyan
Write-Host ""

# --- A) Una sola tabla de tarjetas ---
$builders = ([regex]::Matches($code, 'static void buildHomeCards\(')).Count
Check "existe UNA sola funcion buildHomeCards()" ($builders -eq 1) ("encontradas: {0}" -f $builders)

# El dibujado tiene que llamarla, no tener su propia tabla.
$drawCalls = ([regex]::Matches($code, 'buildHomeCards\(cards,')).Count
Check "el dibujado y el dispatch llaman a buildHomeCards()" ($drawCalls -ge 3) `
      ("llamadas: {0} (se esperan al menos 3: dibujado, dispatch y logs de foco)" -f $drawCalls)

# --- B) El dispatch NO usa indices magicos ---
$magicDispatch = ([regex]::Matches($code, 'if\(selection==\d\)\s*\{\s*page=')).Count
Check "el dispatch de la X no usa selection==N" ($magicDispatch -eq 0) `
      ("apariciones: {0} (debe usar switch sobre la accion de la tarjeta)" -f $magicDispatch)

$switchOnAction = [regex]::IsMatch($code, 'switch\(cards\[index\]\.action\)')
Check "el dispatch hace switch(cards[index].action)" $switchOnAction `
      "no se encontro el switch sobre la accion de la tarjeta"

# --- C) El numero de tarjetas es una constante coherente ---
$count = Val 'constexpr int kHomeCardCount = (\d+);'
Check "kHomeCardCount esta definido" ($null -ne $count) "no se encontro kHomeCardCount"

# Contar las asignaciones `out[N]={` dentro de buildHomeCards.
$builderBody = [regex]::Match($code, 'static void buildHomeCards\([^\)]*\)\s*\{(.*?)\n\}', 'Singleline')
$entries = 0
if ($builderBody.Success) {
    $entries = ([regex]::Matches($builderBody.Groups[1].Value, 'out\[\d+\]=')).Count
}
Check "la tabla tiene tantas entradas como dice kHomeCardCount" `
      ($null -ne $count -and [int]$count -eq $entries) `
      ("kHomeCardCount={0} entradas={1}" -f $count, $entries)

# --- D) Cada accion tiene su rama ---
$actionEnum = [regex]::Match($code, 'enum class HomeAction \{(.*?)\};', 'Singleline')
$declared = @()
if ($actionEnum.Success) {
    $declared = [regex]::Matches($actionEnum.Groups[1].Value, '(k[A-Za-z]+)') | ForEach-Object { $_.Groups[1].Value }
}
Check "el enum HomeAction declara acciones" ($declared.Count -gt 0) "no se pudo leer el enum"

$sinRama = @()
foreach ($a in $declared) {
    if (-not [regex]::IsMatch($code, ("case HomeAction::{0}:" -f $a))) { $sinRama += $a }
}
Check "todas las acciones tienen rama en el switch" ($sinRama.Count -eq 0) `
      ("sin rama: {0}" -f ($sinRama -join ', '))

# Y todas las acciones estan USADAS por alguna tarjeta: una accion sin tarjeta es codigo muerto.
$sinTarjeta = @()
foreach ($a in $declared) {
    $usos = ([regex]::Matches($code, ("HomeAction::{0}" -f $a))).Count
    # 1 uso = solo la rama del switch; 2 o mas = declarada, usada por una tarjeta y con rama.
    if ($usos -lt 2) { $sinTarjeta += $a }
}
Check "todas las acciones las usa alguna tarjeta" ($sinTarjeta.Count -eq 0) `
      ("acciones sin tarjeta (codigo muerto): {0}" -f ($sinTarjeta -join ', '))

# --- E) Filas reales de la rejilla ---
$rows = Val 'constexpr int kHomeRows = (\d+);'
Check "kHomeRows esta definido" ($null -ne $rows) "no se encontro kHomeRows"

$navRows = Val 'const int nextRow=\(row\+direction\+kHomeRows\)%kHomeRows;'
Check "la navegacion vertical usa kHomeRows" ($null -ne $navRows) `
      "la navegacion vertical no usa (row+direction+kHomeRows)%kHomeRows"

# Ya no debe existir la rama especial de la fila incompleta: con filas completas sobra.
$ramaEspecial = ([regex]::Matches($code, 'else if\(nextRow==\d\) selection=\d')).Count
Check "no queda la rama especial de fila incompleta" ($ramaEspecial -eq 0) `
      ("apariciones: {0}" -f $ramaEspecial)

# --- F) La rejilla cuadra ---
$cols = Val 'const int cardXs\[(\d+)\]'
if ($null -ne $cols -and $null -ne $rows -and $null -ne $count) {
    $producto = [int]$cols * [int]$rows
    Check "columnas x filas == numero de tarjetas" ($producto -eq [int]$count) `
          ("{0} columnas x {1} filas = {2}, pero hay {3} tarjetas" -f $cols, $rows, $producto, $count)
} else {
    Check "se pudo leer la rejilla" $false "faltan cardXs, kHomeRows o kHomeCardCount"
}

# --- G) El hit-test del raton usa la MISMA rejilla ---
$mouseBody = [regex]::Match($code, 'static void handleMousePosition\(int x,int y\) \{(.*?)\n\}', 'Singleline')
if ($mouseBody.Success) {
    $mb = $mouseBody.Groups[1].Value
    # La primera rama (page==0) es la del centro de juego.
    $primerBloque = $mb.Substring(0, [Math]::Min(700, $mb.Length))
    Check "el hit-test del raton usa kHomeRows" ($primerBloque -match 'ys\[kHomeRows\]') `
          "no se encontro 'ys[kHomeRows]' en la rama del centro de juego"
    Check "el hit-test del raton usa kHomeCardCount" ($primerBloque -match 'kHomeCardCount') `
          "no se encontro kHomeCardCount en la rama del centro de juego"
    # Un array de y con cuatro valores seria la rejilla vieja.
    $ysCuatro = [regex]::IsMatch($primerBloque, 'ys\[\]\s*=\s*\{[^}]*,[^}]*,[^}]*,[^}]*\}')
    Check "el hit-test no tiene cuatro filas (rejilla vieja)" (-not $ysCuatro) `
          "se encontro un array de y con cuatro valores"
} else {
    Check "se pudo leer handleMousePosition" $false "no se encontro la funcion"
}

# --- H) Logs de verificacion del foco ---
$foco = ([regex]::Matches($code, '"MENU_FOCUS"')).Count
Check "existe el log MENU_FOCUS al mover la cruceta" ($foco -ge 2) `
      ("apariciones: {0} (se esperan 2: navegacion vertical y horizontal)" -f $foco)
$navLog = ([regex]::Matches($code, '"MENU_NAV"')).Count
Check "existe el log MENU_NAV al pulsar X" ($navLog -ge 1) `
      ("apariciones: {0}" -f $navLog)

# --- I) No queda la funcion trampa ---
$trampa = ([regex]::Matches($code, 'homeCardCount')).Count
Check "no queda ningun homeCardCount suelto" ($trampa -eq 0) `
      ("apariciones: {0} (el numero de tarjetas debe salir solo de kHomeCardCount)" -f $trampa)

Write-Host ""
if ($fail -eq 0) {
    Write-Host "  Todas las comprobaciones de navegacion pasaron" -ForegroundColor Green
    exit 0
} else {
    Write-Host ("  {0} comprobacion(es) FALLARON" -f $fail) -ForegroundColor Red
    exit 1
}
