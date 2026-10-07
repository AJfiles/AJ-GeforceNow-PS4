#!/usr/bin/env python3
"""
Refactor de filas de configuracion para la v3.30 — version atomica y verificada.

PROBLEMA QUE RESUELVE
---------------------
En intentos anteriores use reemplazos literales y borrados por numero de linea, y ambos fallaron:
los literales no encajaban por indentacion y los numeros se desajustaban al borrar. El resultado
fueron ficheros a medio refactorizar que no compilaban.

ESTE ENFOQUE
------------
1. Trabaja sobre el TEXTO COMPLETO con expresiones regulares ancladas a CONTENIDO, no a posiciones.
2. Elimina las dos filas y sus `case` asociados.
3. Renumera etiquetas, valores, `case` y `selection==` de forma CONSECUTIVA por orden de aparicion,
   que es invariante al borrado.
4. VERIFICA el balance de llaves y que las filas cuadran con la constante `kSettingsRows`, y si algo
   no cuadra NO escribe el fichero (asi nunca deja un estado roto).
"""
import re
import sys

PATH = 'src/ps4/main.cpp'

with open(PATH, 'r', encoding='utf-8') as f:
    txt = f.read()

original = txt

# ---------------------------------------------------------------------------
# 1. Eliminar las DOS filas de UI y sus `case` de adjust().
# ---------------------------------------------------------------------------
# Bloque de la fila DECODIFICADOR: comentario + label + "{" + contenido + "}".
# El cierre es la primera linea que sea SOLO "}" tras el label, no la del bloque interno.
txt = re.sub(
    r'\n[ \t]*// --- Fila \d+: modo de decodificador ---'
    r'(?:(?!\n[ \t]*\}\n).)*'          # todo hasta el primer "\n    }\n"
    r'\n[ \t]*\}\n',
    '\n', txt, flags=re.S)

# Bloque de la fila RESOLUCION DE VIDEO (mismo patron)
txt = re.sub(
    r'\n[ \t]*// --- Fila \d+: modo de escalado de video ---'
    r'(?:(?!\n[ \t]*\}\n).)*'
    r'\n[ \t]*\}\n',
    '\n', txt, flags=re.S)

# `case` de decoderMode y de scaleMode, con su comentario
txt = re.sub(r'\n[ \t]*// Fila \d+: modo de decodificador[^\n]*\n[ \t]*case \d+:\s*decoderMode[^\n]*\n', '\n', txt)
txt = re.sub(r'\n[ \t]*// Fila \d+: resolucion FIJA del framebuffer[^\n]*\n[ \t]*case \d+:\s*scaleMode[^\n]*\n', '\n', txt)

# Manejadores de X de esas dos filas (el de decoderMode tenia varias lineas)
txt = re.sub(r'\n[ \t]*// Fila \d+: modo de decodificador[^\n]*\n(?:[ \t]*else if \(page==1 && selection==\d+\) \{.*?\n[ \t]*\}\n)', '\n', txt, flags=re.S)
txt = re.sub(r'\n[ \t]*// Fila \d+: resolucion FIJA del framebuffer[^\n]*\n(?:[ \t]*else if \(page==1 && selection==\d+\) \{.*?\n[ \t]*\}\n)', '\n', txt, flags=re.S)

# ---------------------------------------------------------------------------
# 2. Renumerar ETIQUETAS de forma consecutiva por orden de aparicion.
# ---------------------------------------------------------------------------
contador = [0]
def ren_label(m):
    n = contador[0]; contador[0] += 1
    return f'label(kLabelX,rowTextY({n})'
txt = re.sub(r'label\(kLabelX,rowTextY\(\d+\)', ren_label, txt)
filas = contador[0]

# ---------------------------------------------------------------------------
# 3. Alinear cada etiqueta de VALOR con el numero de su etiqueta (mismo bloque de fila).
# ---------------------------------------------------------------------------
out = []
fila = None
for ln in txt.split('\n'):
    m = re.search(r'label\(kLabelX,rowTextY\((\d+)\)', ln)
    if m:
        fila = m.group(1)
    if 'label(kValueX,rowTextY(' in ln and fila is not None:
        ln = re.sub(r'label\(kValueX,rowTextY\(\d+\)', f'label(kValueX,rowTextY({fila})', ln)
    out.append(ln)
txt = '\n'.join(out)

# ---------------------------------------------------------------------------
# 4. Renumerar los `case` del switch de ajustes, consecutivos por orden.
# ---------------------------------------------------------------------------
ini = txt.find('switch(selection)')
if ini < 0:
    print('ERROR: no se encontro switch(selection)')
    sys.exit(1)
# el switch termina en la primera linea que sea solo "}" tras el inicio
fin = txt.find('\n    }', ini)
bloque = txt[ini:fin]
c = [0]
def ren_case(m):
    n = c[0]; c[0] += 1
    return f'{m.group(1)}case {n}:'
bloque = re.sub(r'^(\s+)case \d+:', ren_case, bloque, flags=re.M)
txt = txt[:ini] + bloque + txt[fin:]
cases = c[0]

# ---------------------------------------------------------------------------
# 5. Renumerar los `selection==` de los manejadores de X, por orden.
# ---------------------------------------------------------------------------
s = [0]
def ren_sel(m):
    n = s[0]; s[0] += 1
    return f'page==1 && selection=={n}'
txt = re.sub(r'page==1 && selection==\d+', ren_sel, txt)
sels = s[0]

# ---------------------------------------------------------------------------
# 6. Constantes de navegacion.
# ---------------------------------------------------------------------------
txt = re.sub(r'constexpr int kSettingsRows = \d+;', f'constexpr int kSettingsRows = {filas};', txt)
txt = re.sub(r'else if \(page==1\) selection=\(selection\+direction\+\d+\)%\d+;',
             f'else if (page==1) selection=(selection+direction+{filas})%{filas};', txt)
txt = re.sub(r'for\(int i=0;i<\d+;i\+\+\) \{ const int rowY=268\+i\*42;',
             f'for(int i=0;i<{filas};i++) {{ const int rowY=268+i*42;', txt)

# ---------------------------------------------------------------------------
# 7. VERIFICACION antes de escribir. Si algo no cuadra, no se toca el fichero.
# ---------------------------------------------------------------------------
abre = txt.count('{')
cierra = txt.count('}')
if abre != cierra:
    print(f'ERROR: llaves desbalanceadas (abre={abre} cierra={cierra}). NO se escribe el fichero.')
    sys.exit(1)

etiquetas = re.findall(r'label\(kLabelX,rowTextY\((\d+)\)', txt)
nums = sorted(int(x) for x in etiquetas)
if nums != list(range(len(nums))):
    print(f'ERROR: etiquetas no consecutivas: {nums}. NO se escribe el fichero.')
    sys.exit(1)

if 'decoderMode=' in txt or 'scaleMode=' in txt:
    print('ERROR: quedan referencias a decoderMode/scaleMode en el switch. NO se escribe.')
    sys.exit(1)

with open(PATH, 'w', encoding='utf-8') as f:
    f.write(txt)

print(f'OK  filas={filas}  cases={cases}  selection=={sels}')
print(f'    llaves equilibradas ({abre})')
print('    etiquetas consecutivas 0..%d' % (filas - 1))
print('    sin decoderMode/scaleMode en el switch')
