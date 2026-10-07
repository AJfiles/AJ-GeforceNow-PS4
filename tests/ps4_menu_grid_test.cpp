// PRUEBA DE LA REJILLA Y LA NAVEGACION DEL CENTRO DE JUEGO.
//
// POR QUE EXISTE
// --------------
// El menu principal ha tenido DOS fallos de sincronizacion entre el mando y la pantalla:
//
//   v3.36: (1) "ACERCA DE" desaparecio porque el dispatch de la X pedia indices que la tabla ya no
//          producia; (2) el foco se iluminaba en una tarjeta distinta de la seleccionada.
//
// Ambos se debian a lo mismo: EL MISMO DATO ESCRITO EN DOS SITIOS. Revisar la aritmetica a ojo no
// basta, porque el fallo aparece justo cuando alguien cambia una tabla y olvida el otro sitio.
//
// Esta prueba REPLICA la logica de `nav()`, `moveHorizontal()` y del bucle de dibujado (con exactamente
// las mismas formulas) y comprueba, de forma exhaustiva, que:
//
//   1. Las cuatro direcciones producen SIEMPRE un indice dentro de [0..5].
//   2. El indice al que se llega es el que corresponde segun la posicion en la rejilla.
//   3. La columna se conserva al subir y bajar.
//   4. La fila se conserva al ir a izquierda y derecha.
//   5. Desde cualquier tarjeta se puede alcanzar cualquier otra (el foco no se queda atrapado).
//   6. La posicion que calcula el DIBUJADO (fila=i/2, col=i%2) coincide con la que asume la NAVEGACION.
//
// La comprobacion 6 es la que habria cazado el desfase original: si el dibujado y la navegacion usan
// formulas distintas para "donde esta la tarjeta i", el foco se ilumina en el sitio equivocado aunque
// `selection` sea correcto.
//
// Compilacion y ejecucion (host, sin PS4):
//     clang++ -std=c++20 -O1 tests/ps4_menu_grid_test.cpp -o build/menu_grid_test && ./build/menu_grid_test

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Copia EXACTA de las constantes del cliente (src/ps4/main.cpp). Si alli cambian, aqui deben cambiar:
// la prueba compila con las suyas propias para poder ejecutarse sin el toolchain de PS4.
// ---------------------------------------------------------------------------------------------
static constexpr int kHomeCardCount = 6;
static constexpr int kHomeRows      = 3;
static constexpr int kHomeColumns   = 2;

// Las seis tarjetas, en el MISMO orden que `buildHomeCards()`.
static const char* kTitles[kHomeCardCount] = {
    "PRUEBA DE CONEXION",
    "AJ - CONFIGURACION",
    "INICIAR SESION GFN",
    "JUGAR DESDE GEFORCE NOW",
    "PROBAR MANDO",
    "ACERCA DE",
};

// ---------------------------------------------------------------------------------------------
// Formulas del cliente, copiadas literalmente.
// ---------------------------------------------------------------------------------------------

// Dibujado: en que fila y columna se pinta la tarjeta `i`.
static void drawCell(int i, int& row, int& col) {
    row = i / 2;
    col = i % 2;
}

// Navegacion vertical: `nav(direction)` con direction = -1 (arriba) o +1 (abajo).
static int navVertical(int selection, int direction) {
    const int row = selection / 2, col = selection % 2;
    const int nextRow = (row + direction + kHomeRows) % kHomeRows;
    return nextRow * 2 + col;
}

// Navegacion horizontal: `moveHorizontal(direction)`.
static int moveHorizontal(int selection, int direction) {
    const int row = selection / 2, col = selection % 2;
    const int nextCol = (col + direction + 2) % 2;
    const int candidate = row * 2 + nextCol;
    return (candidate < kHomeCardCount) ? candidate : selection;
}

// Nombre de una tarjeta, protegido contra indices fuera de rango.
//
// POR QUE HACE FALTA: las comprobaciones de mas abajo formatean mensajes con el nombre de la tarjeta a
// la que se ha llegado. Si la formula que se esta probando devuelve un indice invalido (que es
// justamente el fallo que se busca), indexar `kTitles` directamente TUMBA la prueba con un fallo de
// segmento en lugar de reportar el problema. Una prueba que crashea no informa de nada.
static std::string TitleOf(int index) {
    if (index < 0 || index >= kHomeCardCount) return "<FUERA DE RANGO:" + std::to_string(index) + ">";
    return kTitles[index];
}

// ---------------------------------------------------------------------------------------------
static int g_failures = 0;
static void Check(const char* name, bool ok, const std::string& detail = "") {
    if (ok) {
        std::printf("    [OK]    %s\n", name);
    } else {
        std::printf("    [FALLO] %s\n", name);
        if (!detail.empty()) std::printf("            %s\n", detail.c_str());
        ++g_failures;
    }
}

int main() {
    std::printf("\n  PRUEBA DE LA REJILLA DEL CENTRO DE JUEGO\n\n");

    // --- 1 y 6: la rejilla del dibujado cubre exactamente las seis tarjetas, sin huecos ni repetidos ---
    {
        bool celdas[8][2] = {};
        bool duplicada = false, fuera = false;
        for (int i = 0; i < kHomeCardCount; ++i) {
            int row = 0, col = 0;
            drawCell(i, row, col);
            if (row < 0 || row >= kHomeRows || col < 0 || col >= kHomeColumns) { fuera = true; continue; }
            if (celdas[row][col]) duplicada = true;
            celdas[row][col] = true;
        }
        Check("el dibujado coloca las 6 tarjetas dentro de la rejilla 2x3", !fuera);
        Check("el dibujado no coloca dos tarjetas en la misma celda", !duplicada);

        int ocupadas = 0;
        for (int r = 0; r < kHomeRows; ++r)
            for (int c = 0; c < kHomeColumns; ++c)
                if (celdas[r][c]) ++ocupadas;
        Check("la rejilla 2x3 esta COMPLETA (6 celdas ocupadas)", ocupadas == kHomeCardCount,
              "ocupadas=" + std::to_string(ocupadas) + " esperadas=" + std::to_string(kHomeCardCount));
    }

    // --- 2: la navegacion devuelve siempre un indice valido ---
    {
        bool ok = true;
        std::string detalle;
        for (int s = 0; s < kHomeCardCount; ++s) {
            for (int d : {-1, 1}) {
                const int v = navVertical(s, d);
                if (v < 0 || v >= kHomeCardCount) { ok = false; detalle = "nav(" + std::to_string(s) + "," + std::to_string(d) + ")=" + std::to_string(v); }
                const int h = moveHorizontal(s, d);
                if (h < 0 || h >= kHomeCardCount) { ok = false; detalle = "horizontal(" + std::to_string(s) + "," + std::to_string(d) + ")=" + std::to_string(h); }
            }
        }
        Check("toda direccion devuelve un indice en [0..5]", ok, detalle);
    }

    // --- 3: la COLUMNA se conserva al subir y bajar ---
    {
        bool ok = true;
        std::string detalle;
        for (int s = 0; s < kHomeCardCount; ++s) {
            int row = 0, col = 0;
            drawCell(s, row, col);
            for (int d : {-1, 1}) {
                const int v = navVertical(s, d);
                int vrow = 0, vcol = 0;
                drawCell(v, vrow, vcol);
                if (vcol != col) {
                    ok = false;
                    detalle = std::string("desde '") + TitleOf(s) + "' col=" + std::to_string(col) +
                              " se llega a '" + TitleOf(v) + "' col=" + std::to_string(vcol);
                }
            }
        }
        Check("al subir/bajar se conserva la COLUMNA", ok, detalle);
    }

    // --- 4: la FILA se conserva al ir a izquierda y derecha ---
    {
        bool ok = true;
        std::string detalle;
        for (int s = 0; s < kHomeCardCount; ++s) {
            int row = 0, col = 0;
            drawCell(s, row, col);
            for (int d : {-1, 1}) {
                const int h = moveHorizontal(s, d);
                int hrow = 0, hcol = 0;
                drawCell(h, hrow, hcol);
                if (hrow != row) {
                    ok = false;
                    detalle = std::string("desde '") + TitleOf(s) + "' fila=" + std::to_string(row) +
                              " se llega a '" + TitleOf(h) + "' fila=" + std::to_string(hrow);
                }
            }
        }
        Check("al ir a izquierda/derecha se conserva la FILA", ok, detalle);
    }

    // --- 5: desde cualquier tarjeta se alcanza cualquier otra (sin foco atrapado) ---
    {
        // Recorrido en anchura sobre el grafo de las cuatro direcciones.
        bool ok = true;
        std::string detalle;
        for (int origen = 0; origen < kHomeCardCount && ok; ++origen) {
            bool visto[kHomeCardCount] = {};
            std::vector<int> pendientes{origen};
            visto[origen] = true;
            while (!pendientes.empty()) {
                const int s = pendientes.back();
                pendientes.pop_back();
                const int vecinos[4] = {navVertical(s, -1), navVertical(s, 1),
                                        moveHorizontal(s, -1), moveHorizontal(s, 1)};
                for (int v : vecinos) {
                    if (v >= 0 && v < kHomeCardCount && !visto[v]) { visto[v] = true; pendientes.push_back(v); }
                }
            }
            for (int i = 0; i < kHomeCardCount; ++i) {
                if (!visto[i]) {
                    ok = false;
                    detalle = std::string("desde '") + TitleOf(origen) + "' NO se alcanza '" + TitleOf(i) + "'";
                    break;
                }
            }
        }
        Check("desde cualquier tarjeta se alcanzan las otras cinco", ok, detalle);
    }

    // --- 2b: comprobacion concreta de los dos sintomas reportados ---
    {
        // Abajo desde "PRUEBA DE CONEXION" (0) debe ir a la MISMA columna de la fila siguiente: (1,0) = 2.
        Check("abajo desde 'PRUEBA DE CONEXION' va a 'INICIAR SESION GFN'",
              navVertical(0, 1) == 2, "llego a indice " + std::to_string(navVertical(0, 1)));

        // Abajo desde "AJ - CONFIGURACION" (1) debe ir a (1,1) = 3.
        Check("abajo desde 'AJ - CONFIGURACION' va a 'JUGAR DESDE GEFORCE NOW'",
              navVertical(1, 1) == 3, "llego a indice " + std::to_string(navVertical(1, 1)));

        // Derecha desde "PRUEBA DE CONEXION" (0) debe ir a 1.
        Check("derecha desde 'PRUEBA DE CONEXION' va a 'AJ - CONFIGURACION'",
              moveHorizontal(0, 1) == 1, "llego a indice " + std::to_string(moveHorizontal(0, 1)));

        // El indice que se ilumina es el de la tarjeta con ese texto.
        Check("el indice 5 corresponde a 'ACERCA DE'",
              std::string(kTitles[5]) == "ACERCA DE", std::string("es '") + kTitles[5] + "'");
    }

    // =============================================================================================
    // ARITMETICA EXACTA DE LAS CUATRO DIRECCIONES
    // =============================================================================================
    // Esta seccion comprueba LITERALMENTE la regla que se pidio:
    //
    //     x = (i % 2)   columna          y = (i / 2)   fila
    //     ABAJO     -> el indice suma exactamente +2
    //     DERECHA   -> suma +1, y SOLO si i % 2 == 0
    //
    // Y ademas resuelve el caso que la regla "+2" no cubre: la ULTIMA fila. Ahi `i + 2` daria 6 o 7,
    // indices que no existen (la rejilla tiene 6 tarjetas, de la 0 a la 5). La decision tomada es
    // ENVOLVER conservando la columna: 4 -> 0 y 5 -> 1. Es coherente con la intencion de la regla
    // (la columna no cambia nunca) y evita que el foco se quede atrapado abajo.
    {
        // --- ABAJO = +2 exacto en TODO el interior de la rejilla ---
        bool okInterior = true;
        std::string detalle;
        for (int i = 0; i < kHomeCardCount; ++i) {
            const int fila = i / 2;
            if (fila >= kHomeRows - 1) continue;          // la ultima fila se comprueba aparte
            const int abajo = navVertical(i, 1);
            if (abajo != i + 2) {
                okInterior = false;
                detalle = std::string("desde ") + TitleOf(i) + " (indice " + std::to_string(i) +
                          ") abajo dio " + std::to_string(abajo) + ", esperado " + std::to_string(i + 2);
            }
        }
        Check("ABAJO suma exactamente +2 en todo el interior de la rejilla", okInterior, detalle);

        // --- ARRIBA = -2 exacto en todo el interior ---
        bool okArriba = true;
        detalle.clear();
        for (int i = 0; i < kHomeCardCount; ++i) {
            const int fila = i / 2;
            if (fila == 0) continue;                      // en la fila 0 se envuelve
            const int arriba = navVertical(i, -1);
            if (arriba != i - 2) {
                okArriba = false;
                detalle = std::string("desde ") + TitleOf(i) + " (indice " + std::to_string(i) +
                          ") arriba dio " + std::to_string(arriba) + ", esperado " + std::to_string(i - 2);
            }
        }
        Check("ARRIBA resta exactamente -2 en todo el interior de la rejilla", okArriba, detalle);

        // --- La ULTIMA fila envuelve y CONSERVA LA COLUMNA (el +2 no puede aplicarse: daria 6 y 7) ---
        bool okEnvolver = true;
        detalle.clear();
        for (int i = 0; i < kHomeCardCount; ++i) {
            const int fila = i / 2, columna = i % 2;
            if (fila != kHomeRows - 1) continue;
            const int abajo = navVertical(i, 1);
            const int esperado = columna;                 // fila 0, misma columna
            if (abajo != esperado) {
                okEnvolver = false;
                detalle = std::string("desde ") + TitleOf(i) + " (fila 2, col " + std::to_string(columna) +
                          ") abajo dio " + std::to_string(abajo) + ", esperado " + std::to_string(esperado);
            }
        }
        Check("en la ultima fila, ABAJO envuelve a la fila 0 conservando la columna", okEnvolver, detalle);

        // --- DERECHA = +1 y SOLO desde columna par ---
        bool okDerecha = true;
        detalle.clear();
        for (int i = 0; i < kHomeCardCount; ++i) {
            const int columna = i % 2;
            const int derecha = moveHorizontal(i, 1);
            if (columna == 0) {
                if (derecha != i + 1) {
                    okDerecha = false;
                    detalle = std::string("desde ") + TitleOf(i) + " (col 0) derecha dio " +
                              std::to_string(derecha) + ", esperado " + std::to_string(i + 1);
                }
            } else {
                // Desde la columna impar, "derecha" vuelve a la columna par de la MISMA fila:
                // es un ciclo de dos columnas, no un desplazamiento a otra fila.
                if (derecha != i - 1) {
                    okDerecha = false;
                    detalle = std::string("desde ") + TitleOf(i) + " (col 1) derecha dio " +
                              std::to_string(derecha) + ", esperado " + std::to_string(i - 1);
                }
            }
        }
        Check("DERECHA suma +1 desde columna par (y vuelve desde la impar)", okDerecha, detalle);

        // --- IZQUIERDA = -1 y SOLO desde columna impar ---
        bool okIzquierda = true;
        detalle.clear();
        for (int i = 0; i < kHomeCardCount; ++i) {
            const int columna = i % 2;
            const int izquierda = moveHorizontal(i, -1);
            const int esperado = (columna == 1) ? i - 1 : i + 1;
            if (izquierda != esperado) {
                okIzquierda = false;
                detalle = std::string("desde ") + TitleOf(i) + " (col " + std::to_string(columna) +
                          ") izquierda dio " + std::to_string(izquierda) +
                          ", esperado " + std::to_string(esperado);
            }
        }
        Check("IZQUIERDA resta -1 desde columna impar (y vuelve desde la par)", okIzquierda, detalle);

        // --- La COLUMNA no cambia NUNCA al subir o bajar (la esencia de la regla) ---
        bool okColumnaFija = true;
        detalle.clear();
        for (int i = 0; i < kHomeCardCount; ++i) {
            const int columna = i % 2;
            for (int d : {-1, 1}) {
                const int destino = navVertical(i, d);
                if (destino % 2 != columna) {
                    okColumnaFija = false;
                    detalle = std::string("desde ") + TitleOf(i) + " (col " + std::to_string(columna) +
                              ") dir " + std::to_string(d) + " se llego a " + TitleOf(destino) +
                              " (col " + std::to_string(destino % 2) + ")";
                }
            }
        }
        Check("la COLUMNA (i % 2) no cambia NUNCA al subir o bajar", okColumnaFija, detalle);

        // --- La FILA no cambia NUNCA al ir a izquierda o derecha ---
        bool okFilaFija = true;
        detalle.clear();
        for (int i = 0; i < kHomeCardCount; ++i) {
            const int fila = i / 2;
            for (int d : {-1, 1}) {
                const int destino = moveHorizontal(i, d);
                if (destino / 2 != fila) {
                    okFilaFija = false;
                    detalle = std::string("desde ") + TitleOf(i) + " (fila " + std::to_string(fila) +
                              ") dir " + std::to_string(d) + " se llego a " + TitleOf(destino) +
                              " (fila " + std::to_string(destino / 2) + ")";
                }
            }
        }
        Check("la FILA (i / 2) no cambia NUNCA al ir a izquierda o derecha", okFilaFija, detalle);
    }

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("  Todas las comprobaciones de la rejilla pasaron\n\n");
        return 0;
    }
    std::printf("  %d comprobacion(es) FALLARON\n\n", g_failures);
    return 1;
}
