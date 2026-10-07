#!/usr/bin/env python3
"""
Refactoriza las filas de configuracion de src/ps4/main.cpp para la v3.30.

QUE HACE
--------
1. Elimina la fila de RESOLUCION DE VIDEO (el antiguo `scaleMode`), que era REDUNDANTE: su valor se
   aplicaba dentro de `resolution_to_wh()`, la misma funcion que ya usa la fila RESOLUCION DEL
   STREAM. Las dos escribian la misma variable y se apilaban, que es lo que llevaba a poner "720 y
   720" y acabar con 1080p. Ahora una sola fila controla framebuffer y peticion al servidor.

2. Elimina el ajuste DECODIFICADOR de la interfaz (punto D del plan): el decodificador por hardware
   no funciona (`hardware_decode=no` en todos los logs, `libSceVideodec2` no carga con 0x805A1000 y
   la arbitracion no tiene firmas verificadas). El probe v1 se conserva como diagnostico interno,
   pero el ajuste ya no sugiere una capacidad que no existe.

3. Renumera las etiquetas y los manejadores de X de forma CONSISTENTE: cada fila mantiene su pareja
   etiqueta/manejador, solo cambia el numero.

POR QUE CON PYTHON Y NO CON REEMPLAZOS DE TEXTO
-----------------------------------------------
En intentos anteriores los reemplazos literales fallaban por diferencias de indentacion y dejaban
etiquetas y manejadores DESALINEADOS (una fila mostraba una cosa y hacia otra). Aqui se procesa linea
a linea, identificando las filas por su CONTENIDO y no por su numero, asi que la correspondencia se
mantiene por construccion.
"""
import re
import sys

PATH = 'src/ps4/main.cpp'

# Filas que se ELIMINAN, identificadas por un texto que aparece en su etiqueta.
# El orden importa: se elimina el bloque completo de la etiqueta (label + value + comentarios).
ELIMINAR = [
    'RESOLUCION DE VIDEO',   # fila redundante con RESOLUCION DEL STREAM (punto B)
    'DECODIFICADOR',         # hardware no funcional (punto D)
]

with open(PATH, 'r', encoding='utf-8') as f:
    lines = f.readlines()

# ---------------------------------------------------------------------------
# PASO 1: localizar las lineas de etiqueta que hay que borrar y su bloque.
# ---------------------------------------------------------------------------
borrar = set()
for i, ln in enumerate(lines):
    if 'label(kLabelX,rowTextY(' not in ln:
        continue
    if not any(txt in ln for txt in ELIMINAR):
        continue
    # Subir hasta el comentario de cabecera de la fila ("// --- Fila N:") o hasta 6 lineas.
    start = i
    for j in range(i, max(0, i - 14), -1):
        if '// --- Fila' in lines[j]:
            start = j
            break
    # Bajar hasta cerrar el bloque de la etiqueta de valor: la primera linea con "}" que cierre,
    # o la linea del label(kValueX) mas una si es un bloque con llaves.
    end = i
    k = i
    while k < len(lines) and k < i + 14:
        end = k
        if 'label(kValueX' in lines[k]:
            # si la etiqueta de valor esta dentro de un bloque { }, buscar el cierre
            if lines[k].rstrip().endswith(';'):
                # comprobar si la linea anterior abrio un bloque
                if k > 0 and lines[k - 1].strip() == '{':
                    m = k + 1
                    while m < len(lines) and lines[m].strip() != '}':
                        m += 1
                    end = m
            break
        k += 1
    for b in range(start, end + 1):
        borrar.add(b)
    print(f'  eliminando fila "{ELIMINAR[0] if "RESOLUCION DE VIDEO" in ln else "DECONIFICADOR"}"'
          f' lineas {start+1}..{end+1}')

lines = [ln for idx, ln in enumerate(lines) if idx not in borrar]

# ---------------------------------------------------------------------------
# PASO 2: renumerar las etiquetas de forma CONSECUTIVA.
# ---------------------------------------------------------------------------
contador = [0]
def renumerar_label(m):
    nuevo = contador[0]
    contador[0] += 1
    return f'label(kLabelX,rowTextY({nuevo})'
lines = [re.sub(r'label\(kLabelX,rowTextY\((\d+)\)', renumerar_label, ln) for ln in lines]
total_filas = contador[0]
print(f'  filas de etiqueta renumeradas: {total_filas} (0..{total_filas-1})')

# ---------------------------------------------------------------------------
# PASO 3: la etiqueta de valor de cada fila debe usar el MISMO numero que su etiqueta.
# Se emparejan por orden de aparicion dentro del mismo bloque de fila.
# ---------------------------------------------------------------------------
out = []
fila_actual = None
for ln in lines:
    m = re.search(r'label\(kLabelX,rowTextY\((\d+)\)', ln)
    if m:
        fila_actual = m.group(1)
    if 'label(kValueX,rowTextY(' in ln and fila_actual is not None:
        ln = re.sub(r'label\(kValueX,rowTextY\(\d+\)', f'label(kValueX,rowTextY({fila_actual})', ln)
    out.append(ln)
lines = out
print('  etiquetas de valor alineadas con su fila')

# ---------------------------------------------------------------------------
# PASO 4: renumerar los manejadores de X (selection==N) manteniendo su ORDEN.
# Cada manejador se identifica por su ORDEN de aparicion en el bloque de settings, no por el numero.
# ---------------------------------------------------------------------------
# Recopilar el orden de los manejadores actuales y su numero
indices = []
for ln in lines:
    m = re.search(r'page==1 && selection==(\d+)', ln)
    if m:
        indices.append(int(m.group(1)))
# El numero nuevo de cada manejador es su POSICION en la lista ordenada por numero actual
mapa = {}
for pos, viejo in enumerate(sorted(set(indices))):
    mapa[viejo] = pos

# Aplicar el mapa SOLO a las lineas de manejador (page==1 && selection==)
def remap_sel(m):
    viejo = int(m.group(1))
    return f'page==1 && selection=={mapa.get(viejo, viejo)}'
lines = [re.sub(r'page==1 && selection==(\d+)', remap_sel, ln) for ln in lines]
print(f'  manejadores de X remapeados: {mapa}')

# ---------------------------------------------------------------------------
# PASO 5: el switch de adjust() usa `case N`. Hay que remapear igual, manteniendo el orden.
# ---------------------------------------------------------------------------
casos = []
for ln in lines:
    if re.match(r'^\s+case \d+:', ln) and 'break;' in ln or re.match(r'^\s+case \d+: \{', ln):
        m = re.match(r'^\s+case (\d+):', ln)
        if m:
            casos.append(int(m.group(1)))
# Solo el rango de casos del switch de ajustes (0..N) y sin duplicados
unicos = sorted(set(c for c in casos if c < 100))
mapa_case = {viejo: pos for pos, viejo in enumerate(unicos)}
def remap_case(m):
    # El primer grupo de la expresion es el ESPACIO en blanco y el segundo el numero.
    # En la version anterior se leia m.group(1), que es el espacio, y `int('    ')` fallaba.
    viejo = int(m.group(2))
    return f'{m.group(1)}case {mapa_case.get(viejo, viejo)}:'
# Aplicar solo dentro del switch de ajustes: entre "switch(selection)" y el cierre
ini = None
fin = None
for i, ln in enumerate(lines):
    if ini is None and 'switch(selection)' in ln:
        ini = i
    elif ini is not None and fin is None and re.match(r'^\s+\}\s*$', ln):
        fin = i
        break
if ini is not None and fin is not None:
    for i in range(ini, fin):
        lines[i] = re.sub(r'^(\s+)case (\d+):', remap_case, lines[i])
    print(f'  cases de adjust() remapeados: {mapa_case}')
else:
    print('  AVISO: no se localizo el switch(selection); los case NO se han remapeado')

# ---------------------------------------------------------------------------
# PASO 6: actualizar kSettingsRows y los bucles de navegacion.
# ---------------------------------------------------------------------------
txt = ''.join(lines)
txt = re.sub(r'constexpr int kSettingsRows = \d+;', f'constexpr int kSettingsRows = {total_filas};', txt)
txt = re.sub(r'else if \(page==1\) selection=\(selection\+direction\+\d+\)%\d+;',
             f'else if (page==1) selection=(selection+direction+{total_filas})%{total_filas};', txt)
txt = re.sub(r'for\(int i=0;i<\d+;i\+\+\) \{ const int rowY=268\+i\*42;',
             f'for(int i=0;i<{total_filas};i++) {{ const int rowY=268+i*42;', txt)

with open(PATH, 'w', encoding='utf-8') as f:
    f.write(txt)

print(f'  kSettingsRows = {total_filas}')
print('LISTO')
