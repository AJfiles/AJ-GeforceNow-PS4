# Analiza la cobertura de traducciones de la interfaz.
# Extrae la tabla de traduccion y los textos usados en label()/text(), y lista los que faltan.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$file = Join-Path $root 'src\ps4\main.cpp'
$src = Get-Content $file -Raw

# Tabla: {"ESPANOL","ENGLISH"}
$tabla = @{}
foreach ($m in [regex]::Matches($src, '\{"([^"]{3,})","([^"]*)"\}')) {
    $tabla[$m.Groups[1].Value] = $m.Groups[2].Value
}

# Textos de UI: literales que empiezan por mayuscula y solo contienen caracteres de texto.
$pattern = '(?:label|text)\([^;]*?"([A-Z][A-Z0-9 /|:._(),\-\[\]!?]{3,})"'
$usados = @{}
foreach ($m in [regex]::Matches($src, $pattern)) { $usados[$m.Groups[1].Value] = $true }

$faltan = @($usados.Keys | Where-Object { -not $tabla.ContainsKey($_) } | Sort-Object)

Write-Output ""
Write-Output "COBERTURA DE TRADUCCIONES"
Write-Output ("  Entradas en la tabla : {0}" -f $tabla.Count)
Write-Output ("  Textos de UI usados  : {0}" -f $usados.Count)
Write-Output ("  Traducidos           : {0}" -f ($usados.Count - $faltan.Count))
Write-Output ("  SIN traducir         : {0}" -f $faltan.Count)
if ($usados.Count -gt 0) {
    Write-Output ("  Cobertura            : {0}%" -f [math]::Round(100*($usados.Count-$faltan.Count)/$usados.Count,1))
}
Write-Output ""
Write-Output "SIN TRADUCIR:"
$faltan | ForEach-Object { Write-Output ("  {0}" -f $_) }
