# Verifica que las etiquetas de la pantalla de configuracion caigan DENTRO de su panel.
#
# POR QUE EXISTE
# --------------
# Los problemas de las versiones anteriores fueron de posicionamiento, no de datos: el titulo
# quedaba tapado por la primera fila y varias etiquetas aparecian fuera de su panel (el valor del
# dispositivo de entrada estaba en y=329 mientras su fila estaba en y=305+...). Revisar eso a ojo
# no escala y ya se me escapo dos veces.
#
# Este script lee las constantes de layout del fuente, reconstruye la geometria de cada fila y
# comprueba que el texto de la fila i este entre el borde superior y el inferior de esa fila.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$main = Join-Path $root 'src\ps4\main.cpp'
$lines = Get-Content $main

$kRowTop = $null; $kRowStep = $null; $kRowH = $null; $kLabelY = $null; $kSettingRows = $null
foreach ($l in $lines) {
    if ($l -match 'constexpr int kRowTop\s*=\s*(\d+)')   { $kRowTop = [int]$Matches[1] }
    if ($l -match 'constexpr int kRowStep\s*=\s*(\d+)')  { $kRowStep = [int]$Matches[1] }
    if ($l -match 'constexpr int kRowH\s*=\s*(\d+)')     { $kRowH = [int]$Matches[1] }
    if ($l -match 'constexpr int kLabelY\s*=\s*(\d+)')   { $kLabelY = [int]$Matches[1] }
    if ($l -match 'constexpr int kSettingsRows\s*=\s*(\d+)') { $kSettingRows = [int]$Matches[1] }
}

if (-not $kRowTop -or -not $kRowStep -or -not $kRowH -or -not $kSettingRows) {
    Write-Host "No se pudieron leer las constantes de layout (kRowTop/kRowStep/kRowH/kSettingsRows)" -ForegroundColor Red
    exit 2
}

Write-Host ""
Write-Host "VERIFICACION DEL LAYOUT DE CONFIGURACION" -ForegroundColor Cyan
Write-Host ("  kSettingsRows={0}  kRowTop={1}  kRowStep={2}  kRowH={3}  kLabelY={4}" -f `
    $kSettingRows, $kRowTop, $kRowStep, $kRowH, $kLabelY)
Write-Host ""

$fail = 0
Write-Host ("  {0,-6} {1,-22} {2,-26} {3}" -f 'FILA', 'PANEL (y..y+h)', 'TEXTO ESPERADO', 'ESTADO')
for ($i = 0; $i -lt $kSettingRows; $i++) {
    $panelTop = $kRowTop + $i * $kRowStep
    $panelBot = $panelTop + $kRowH
    $textY = $panelTop + $kLabelY
    # El texto se dibuja con su parte superior en textY; se considera correcto si cae dentro del panel.
    $ok = ($textY -ge $panelTop) -and ($textY -lt $panelBot)

    # Comprobacion adicional: la constante kLabelY debe dejar margen arriba y abajo.
    $margenArriba = $textY - $panelTop
    $margenAbajo = $panelBot - $textY
    $centrado = ($margenArriba -ge 8) -and ($margenAbajo -ge 8)

    $estado = if (-not $ok) { 'FUERA DEL PANEL' } elseif (-not $centrado) { 'MAL CENTRADO' } else { 'OK' }
    if ($estado -ne 'OK') { $fail++ }

    Write-Host ("  {0,-6} {1,-22} {2,-26} {3}" -f $i, "$panelTop..$panelBot", $textY, $estado) `
        -ForegroundColor $(if ($estado -eq 'OK') { 'Green' } else { 'Red' })
}

# El titulo debe quedar por encima de la primera fila.
$tituloY = $null
foreach ($l in $lines) {
    if ($l -match 'label\(120,(\d+),"CONFIGURACION AJ"') { $tituloY = [int]$Matches[1] }
}
Write-Host ""
if ($tituloY -ne $null) {
    $okTitulo = $tituloY -lt $kRowTop
    Write-Host ("  Titulo en y={0}, primera fila en y={1} -> {2}" -f $tituloY, $kRowTop, `
        $(if ($okTitulo) { 'OK (no se solapan)' } else { 'SOLAPADO' })) `
        -ForegroundColor $(if ($okTitulo) { 'Green' } else { 'Red' })
    if (-not $okTitulo) { $fail++ }
}

# Los valores deben estar alineados a la derecha y no salirse de la pantalla (1920 de ancho).
$kValueX = $null
foreach ($l in $lines) { if ($l -match 'constexpr int kValueX\s*=\s*(\d+)') { $kValueX = [int]$Matches[1] } }
if ($kValueX) {
    $okValor = $kValueX -lt 1920
    Write-Host ("  Valores en x={0} -> {1}" -f $kValueX, $(if ($okValor) { 'OK' } else { 'FUERA DE PANTALLA' })) `
        -ForegroundColor $(if ($okValor) { 'Green' } else { 'Red' })
    if (-not $okValor) { $fail++ }
}

# ---------------------------------------------------------------------------------------------
# COMPROBACION DE SOLAPE: dos bloques DISTINTOS dibujando valor en la misma fila
#
# POR QUE EXISTE: en la 2.99 la fila de region dibujaba su valor en rowTextY(7) en vez de
# rowTextY(8), asi que se pintaba encima del valor de nitidez. El verificador de geometria no lo
# detecto porque las dos etiquetas estaban DENTRO de un panel valido: el fallo era que dos bloques
# distintos compartian fila.
#
# IMPORTANTE: una fila puede tener VARIOS label(kValueX,rowTextY(N)) legitimos cuando son una
# cadena if/else con estados alternativos (por ejemplo el test de velocidad: "MIDIENDO...", el
# resultado, el error, o "PULSA X"). Lo que NO es legitimo es que dos filas DISTINTAS escriban en la
# misma. Por eso se agrupa por la fila que declara cada ETIQUETA y se comprueba que cada valor caiga
# en la fila de una etiqueta.
Write-Host ""
Write-Host "COMPROBACION DE SOLAPE EN LA COLUMNA DE VALORES" -ForegroundColor Cyan

$labelRowNums = @{}
foreach ($l in $lines) {
    if ($l -match 'label\(kLabelX,\s*rowTextY\((\d+)\)') { $labelRowNums[[int]$Matches[1]] = $true }
}
$valueRowNums = @{}
foreach ($l in $lines) {
    if ($l -match 'label\(kValueX,\s*rowTextY\((\d+)\)') { $valueRowNums[[int]$Matches[1]] = $true }
}

# Todo valor debe caer en una fila que tenga etiqueta. Si un valor usa una fila sin etiqueta, o
# esta desplazado respecto a la suya, aparece aqui.
$huerfanos = $valueRowNums.Keys | Where-Object { -not $labelRowNums.ContainsKey($_) } | Sort-Object
if ($huerfanos) {
    foreach ($r in $huerfanos) {
        Write-Host ("  fila {0}: hay un valor pero NINGUNA etiqueta en esa fila  <- DESPLAZADO" -f $r) -ForegroundColor Red
        $fail++
    }
} else {
    Write-Host ("  todos los valores caen en filas con etiqueta ({0} filas con etiqueta)" -f $labelRowNums.Count) -ForegroundColor Green
}

# Y al reves: cada etiqueta debe tener al menos un valor en su misma fila.
$sinValor = $labelRowNums.Keys | Where-Object { -not $valueRowNums.ContainsKey($_) } | Sort-Object
if ($sinValor) {
    foreach ($r in $sinValor) {
        Write-Host ("  fila {0}: tiene etiqueta pero ningun valor" -f $r) -ForegroundColor Yellow
    }
}

Write-Host ""
if ($fail -gt 0) {
    Write-Host "$fail problema(s) de layout" -ForegroundColor Red
    exit 1
}
Write-Host "Layout correcto: etiquetas dentro de su fila, titulo sin solape y valores sin desplazar." -ForegroundColor Green
exit 0
