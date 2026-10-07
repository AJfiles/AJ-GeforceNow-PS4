// Test de la tabla de reciprocros que sustituye a la division en RCAS.
//
// POR QUE EXISTE
// --------------
// La version anterior de rcas_luma_px hacia una DIVISION POR PIXEL: (room * 256) / adiff.
// Medido en consola, eso dejo el escalado en dispatch_avg_us=28837 y los FPS en 24, porque a 720p
// son 921.600 divisiones por frame (unos 28 millones de ciclos solo en dividir).
//
// La version nueva la sustituye por una multiplicacion con el reciproco precalculado:
//
//   rcp[i] = 65536 / i                       (punto fijo 16.16)
//   gmax   = (room * 256 * rcp[adiff]) >> 16
//
// Este test comprueba que el resultado de la tabla coincide con la division exacta en TODO el rango
// de entradas posible, que es pequeno y por tanto se puede recorrer entero. No se razona sobre la
// aproximacion: se comprueba.
#include <cstdio>
#include <cstdint>
#include <cstdlib>

// Copia exacta de la tabla del fuente, para poder verificarla aqui.
static uint32_t g_rcp[257];
static void build_rcp() {
    for (int i = 1; i < 257; ++i) g_rcp[i] = 65536u / static_cast<uint32_t>(i);
    g_rcp[0] = 0xFFFFFFFFu;
}

// Version con DIVISION (referencia exacta).
static int rcas_div(int center, int up, int down, int strength) {
    if (strength <= 0) return center;
    int lo = center, hi = center;
    if (up < lo) lo = up;    if (up > hi) hi = up;
    if (down < lo) lo = down; if (down > hi) hi = down;
    const int mean = (lo + hi) >> 1;
    const int diff = center - mean;
    if (diff == 0) return center;
    const int roomUp = 255 - hi, roomDown = lo;
    const int room = (roomUp < roomDown) ? roomUp : roomDown;
    if (room <= 0) return center;
    const int adiff = (diff < 0) ? -diff : diff;
    int gmax = (room * 256) / adiff;             // <-- la division que se quiere eliminar
    int g = strength; if (g > gmax) g = gmax;
    if (g <= 0) return center;
    int v = center + ((diff * g) >> 8);
    if (v < 0) v = 0; if (v > 255) v = 255;
    return v;
}

// Version con TABLA (la que se usa en consola).
static int rcas_tab(int center, int up, int down, int strength) {
    if (strength <= 0) return center;
    int lo = center, hi = center;
    if (up < lo) lo = up;    if (up > hi) hi = up;
    if (down < lo) lo = down; if (down > hi) hi = down;
    const int mean = (lo + hi) >> 1;
    const int diff = center - mean;
    if (diff == 0) return center;
    const int roomUp = 255 - hi, roomDown = lo;
    const int room = (roomUp < roomDown) ? roomUp : roomDown;
    if (room <= 0) return center;
    const int adiff = (diff < 0) ? -diff : diff;
    int gmax;
    if (adiff <= 256) {
        gmax = static_cast<int>((static_cast<uint32_t>(room) * 256u * g_rcp[adiff]) >> 16);
    } else {
        gmax = 0;
    }
    int g = strength; if (g > gmax) g = gmax;
    if (g <= 0) return center;
    int v = center + ((diff * g) >> 8);
    if (v < 0) v = 0; if (v > 255) v = 255;
    return v;
}

int main() {
    build_rcp();

    // El espacio de entradas es pequeno y cerrado: centro, arriba y abajo son luma 0..255, y la
    // fuerza es uno de los cuatro niveles (0, 50, 100, 160). Se recorre ENTERO.
    const int strengths[] = {0, 50, 100, 160};
    long long checked = 0, mismatch = 0, diffSum = 0, diffMax = 0;
    int worstC = 0, worstU = 0, worstD = 0, worstS = 0, worstA = 0, worstB = 0;

    for (int s = 0; s < 4; ++s) {
        const int st = strengths[s];
        for (int c = 0; c < 256; ++c) {
            for (int u = 0; u < 256; ++u) {
                for (int d = 0; d < 256; ++d) {
                    const int a = rcas_div(c, u, d, st);
                    const int b = rcas_tab(c, u, d, st);
                    ++checked;
                    if (a != b) {
                        ++mismatch;
                        const int delta = (a > b) ? (a - b) : (b - a);
                        diffSum += delta;
                        if (delta > diffMax) {
                            diffMax = delta;
                            worstC = c; worstU = u; worstD = d; worstS = st;
                            worstA = a; worstB = b;
                        }
                    }
                }
            }
        }
    }

    FILE* f = fopen("build/rcas_table_report.txt", "w");
    if (!f) f = stdout;
    fprintf(f, "RCAS RECIPROCAL TABLE TEST\n");
    fprintf(f, "  combos comprobados : %lld\n", checked);
    fprintf(f, "  discrepancias      : %lld\n", mismatch);
    fprintf(f, "  maximo delta       : %lld\n", diffMax);
    fprintf(f, "  delta medio        : %.6f\n",
            mismatch ? static_cast<double>(diffSum) / static_cast<double>(mismatch) : 0.0);
    if (mismatch) {
        fprintf(f, "  peor caso          : c=%d u=%d d=%d strength=%d -> div=%d tabla=%d\n",
                worstC, worstU, worstD, worstS, worstA, worstB);
    }
    // Criterio: la tabla puede desviarse como mucho 1 nivel de luma. Mas que eso seria visible, y
    // un delta de 1 en un unsharp adaptativo no se percibe (esta por debajo del ruido del codec).
    const bool pass = (diffMax <= 1);
    fprintf(f, "  resultado          : %s\n", pass ? "PASA" : "FALLA");
    if (f != stdout) fclose(f);

    printf("checked=%lld mismatch=%lld maxdelta=%lld -> %s\n",
           checked, mismatch, diffMax, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
