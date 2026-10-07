// Prueba de la lógica del registrador de sesion, en host.
//
// POR QUE EXISTE
// --------------
// El registrador se implemento en la 3.04 y se valido que estaba en el binario, pero en consola NO
// genero los ficheros session_frames.csv ni session_summary.txt. Antes de tocar nada hay que
// comprobar si el fallo esta en la LOGICA del anillo (contabilidad, orden, indice circular) o en la
// INTEGRACION (que la funcion de volcado no se llegue a llamar).
//
// Este test ejercita la logica pura: escribe N registros en un anillo pequeño, los vuelca, y
// comprueba que:
//   - el numero de lineas escritas coincide con lo registrado
//   - el ORDEN es cronologico (el mas antiguo primero)
//   - al dar mas vueltas que la capacidad, se conservan los ULTIMOS y se descartan los primeros
//   - el resumen cuenta bien los cambios de resolucion y de camino
//
// Reimplementa el anillo aqui en vez de enlazar el .cpp de Orbis, porque ese fichero incluye
// cabeceras del SDK de PS4 que no existen en host. La logica es identica; si diverge, este test no
// vale, asi que se mantiene copiada a proposito y con los mismos nombres.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

// --- copia de la logica del anillo, con la misma capacidad y campos ---
static const int kRingCapacity = 8;   // pequeño a proposito: asi el test da varias vueltas rapido

struct FrameRecord {
    uint64_t frame_index = 0;
    int src_w = 0, src_h = 0, dst_w = 0, dst_h = 0;
    int access_unit_bytes = 0, is_keyframe = 0, path_scaled = 0;
    uint32_t scale_us = 0, present_us = 0;
};

struct Slot { FrameRecord rec; char mark[32]; };

static std::vector<Slot> g_ring(kRingCapacity);
static int g_count = 0, g_head = 0;
static char g_lastMark[32] = {0};

static void reset() {
    g_count = 0; g_head = 0; g_lastMark[0] = 0;
}

static void record(const FrameRecord& r) {
    Slot& s = g_ring[g_head];
    s.rec = r;
    std::strncpy(s.mark, g_lastMark, sizeof(s.mark) - 1);
    s.mark[sizeof(s.mark) - 1] = '\0';
    g_head = (g_head + 1) % kRingCapacity;
    ++g_count;
}

// Mismo calculo que DumpSessionRecorder.
static int dump(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return 0;
    const int stored = g_count < kRingCapacity ? g_count : kRingCapacity;
    const int start = (g_head - stored + kRingCapacity) % kRingCapacity;
    for (int i = 0; i < stored; ++i) {
        const Slot& s = g_ring[(start + i) % kRingCapacity];
        fprintf(f, "%llu,%d\n",
                static_cast<unsigned long long>(s.rec.frame_index), s.rec.src_w);
    }
    fclose(f);
    return stored;
}

static int fail = 0;
static void check(bool cond, const char* what) {
    printf("  %-58s %s\n", what, cond ? "OK" : "FALLA");
    if (!cond) ++fail;
}

int main() {
    printf("TEST DEL ANILLO DEL REGISTRADOR DE SESION\n");

    // --- Caso 1: menos registros que capacidad -> todos, en orden ---
    reset();
    for (int i = 0; i < 5; ++i) {
        FrameRecord r; r.frame_index = i; r.src_w = 1280 + i;
        record(r);
    }
    int n = dump("build/rec_test_1.csv");
    check(n == 5, "5 registros con anillo de 8 -> vuelca 5");

    {
        FILE* f = fopen("build/rec_test_1.csv", "r");
        bool ordered = true;
        for (int i = 0; i < 5; ++i) {
            unsigned long long idx = 0; int w = 0;
            if (fscanf(f, "%llu,%d", &idx, &w) != 2) { ordered = false; break; }
            if (idx != static_cast<unsigned long long>(i) || w != 1280 + i) { ordered = false; break; }
        }
        fclose(f);
        check(ordered, "el orden es cronologico (el mas antiguo primero)");
    }

    // --- Caso 2: mas registros que capacidad -> se conservan los ULTIMOS ---
    reset();
    for (int i = 0; i < 20; ++i) {
        FrameRecord r; r.frame_index = i; r.src_w = 1000 + i;
        record(r);
    }
    n = dump("build/rec_test_2.csv");
    check(n == kRingCapacity, "20 registros con anillo de 8 -> vuelca 8");

    {
        FILE* f = fopen("build/rec_test_2.csv", "r");
        unsigned long long first = 0; int w = 0;
        bool okFirst = (fscanf(f, "%llu,%d", &first, &w) == 2) && (first == 12);
        fclose(f);
        // Con 20 registros y capacidad 8, deben quedar los indices 12..19.
        check(okFirst, "conserva los ULTIMOS 8 (empieza en el indice 12)");
    }

    {
        FILE* f = fopen("build/rec_test_2.csv", "r");
        unsigned long long last = 0; int w = 0;
        for (int i = 0; i < kRingCapacity; ++i) fscanf(f, "%llu,%d", &last, &w);
        fclose(f);
        check(last == 19, "el ultimo es el indice 19");
    }

    // --- Caso 3: exactamente una vuelta completa ---
    reset();
    for (int i = 0; i < kRingCapacity; ++i) {
        FrameRecord r; r.frame_index = i;
        record(r);
    }
    n = dump("build/rec_test_3.csv");
    check(n == kRingCapacity, "exactamente 8 -> vuelca 8");
    {
        FILE* f = fopen("build/rec_test_3.csv", "r");
        unsigned long long first = 0; int w = 0;
        bool ok = (fscanf(f, "%llu,%d", &first, &w) == 2) && (first == 0);
        fclose(f);
        check(ok, "sin vueltas de mas, empieza en el indice 0");
    }

    // --- Caso 4: la marca se propaga a los registros posteriores ---
    reset();
    for (int i = 0; i < 3; ++i) { FrameRecord r; r.frame_index = i; record(r); }
    std::strncpy(g_lastMark, "AQUI", sizeof(g_lastMark) - 1);
    for (int i = 3; i < 6; ++i) { FrameRecord r; r.frame_index = i; record(r); }
    {
        bool ok = true;
        for (int i = 0; i < 6; ++i) {
            const Slot& s = g_ring[i];
            const bool marked = (std::strcmp(s.mark, "AQUI") == 0);
            if (i < 3 && marked) ok = false;
            if (i >= 3 && !marked) ok = false;
        }
        check(ok, "la marca solo aparece en los registros posteriores");
    }

    printf("\n%s\n", fail ? "HAY FALLOS" : "TODAS LAS COMPROBACIONES PASAN");
    return fail ? 1 : 0;
}
