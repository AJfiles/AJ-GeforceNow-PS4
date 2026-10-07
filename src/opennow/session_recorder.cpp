// Implementacion del registrador de sesion. Ver session_recorder.hpp para el porque.
#include "session_recorder.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "stream_startup_diagnostics.hpp"

namespace opennow::diag {

namespace {

// Anillo de 4096 frames: a 60 fps son unos 68 segundos de historial. Es suficiente para ver la
// secuencia completa alrededor de un evento sin ocupar memoria apreciable (4096 x 64 B = 256 KB).
constexpr int kRingCapacity = 4096;

struct Slot {
    FrameRecord rec;
    // Etiqueta del marcador mas reciente ANTERIOR a este frame. Se copia aqui para no tener que
    // cruzar dos estructuras al analizar.
    char mark[32];
};

std::mutex g_mutex;
std::vector<Slot> g_ring(kRingCapacity);
int g_count = 0;        // frames registrados en total
int g_head = 0;         // siguiente posicion a escribir
std::atomic<bool> g_enabled{false};
char g_lastMark[32] = {0};

// Estadisticas acumuladas para el resumen.
struct Stats {
    uint64_t frames = 0;      // frames PRESENTADOS (RecordFrame)
    uint64_t packets = 0;     // PAQUETES comprimidos (RecordPacket)
    uint64_t keyframes = 0;
    uint64_t resolution_changes = 0;
    uint64_t path_changes = 0;
    uint64_t au_min = 0xFFFFFFFFull;
    uint64_t au_max = 0;
    uint64_t au_sum = 0;
    // Histograma de tamanos de AU en 8 tramos logaritmicos: sirve para ver si el servidor esta
    // comprimiendo de forma estable o si oscila.
    uint64_t au_buckets[8] = {0};
    int last_src_w = -1, last_src_h = -1;
    int last_path = -1;
};
Stats g_stats;

int au_bucket(int bytes) {
    if (bytes <= 0) return 0;
    int b = 0;
    int v = bytes;
    while (v > 1 && b < 7) { v >>= 1; ++b; }
    return b;
}

}  // namespace

void SetSessionRecorderEnabled(bool enabled) { g_enabled.store(enabled); }
bool IsSessionRecorderEnabled() { return g_enabled.load(); }

void ResetSessionRecorder() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_count = 0;
    g_head = 0;
    g_lastMark[0] = '\0';
    g_stats = Stats{};
    LogAppLifecycleEvent("SESSION_RECORDER_RESET", "ring=empty");
}

void RecordFrame(const FrameRecord& record) {
    // TELEMETRIA DE DIAGNOSTICO.
    //
    // POR QUE: en la 3.06 el volcado corria 23 veces pero siempre con frames=0. Los simbolos estaban
    // enlazados, asi que hacia falta saber si RecordFrame no se llamaba, o si se llamaba con el flag
    // apagado. Estos dos contadores lo distinguen sin necesidad de otra ronda de pruebas.
    static uint64_t s_calls = 0;
    static uint64_t s_skipped_disabled = 0;
    ++s_calls;
    if (!g_enabled.load()) {
        ++s_skipped_disabled;
        if (s_calls <= 3) {
            char d[96];
            std::snprintf(d, sizeof(d), "call=%llu enabled=0 skipped=%llu",
                          static_cast<unsigned long long>(s_calls),
                          static_cast<unsigned long long>(s_skipped_disabled));
            LogAppLifecycleEvent("SESSION_RECORD_SKIPPED", d);
        }
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);

    Slot& slot = g_ring[g_head];
    slot.rec = record;
    std::strncpy(slot.mark, g_lastMark, sizeof(slot.mark) - 1);
    slot.mark[sizeof(slot.mark) - 1] = '\0';

    g_head = (g_head + 1) % kRingCapacity;
    ++g_count;

    ++g_stats.frames;
    if (record.is_keyframe) ++g_stats.keyframes;
    if (record.src_w != g_stats.last_src_w || record.src_h != g_stats.last_src_h) {
        if (g_stats.last_src_w >= 0) ++g_stats.resolution_changes;
        g_stats.last_src_w = record.src_w;
        g_stats.last_src_h = record.src_h;
    }
    if (record.path_scaled != g_stats.last_path) {
        if (g_stats.last_path >= 0) ++g_stats.path_changes;
        g_stats.last_path = record.path_scaled;
    }
    // Las estadisticas de AU ahora se alimentan desde RecordPacket, donde SI se ve el tamano
    // comprimido. Aqui solo se cuenta el frame presentado.
}

void RecordPacket(int compressed_bytes, int is_keyframe) {
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_mutex);

    ++g_stats.packets;   // contador PROPIO: mezclarlo con frames corrompia los porcentajes
    if (is_keyframe) ++g_stats.keyframes;

    const uint64_t au = static_cast<uint64_t>(compressed_bytes > 0 ? compressed_bytes : 0);
    if (au > 0) {
        if (au < g_stats.au_min) g_stats.au_min = au;
        if (au > g_stats.au_max) g_stats.au_max = au;
        g_stats.au_sum += au;
        ++g_stats.au_buckets[au_bucket(compressed_bytes)];
    }
}
void MarkSessionEvent(const char* label) {
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::strncpy(g_lastMark, label ? label : "?", sizeof(g_lastMark) - 1);
    g_lastMark[sizeof(g_lastMark) - 1] = '\0';
    LogAppLifecycleEvent("SESSION_MARK", g_lastMark);
}

int SessionRecorderCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_count < kRingCapacity ? g_count : kRingCapacity;
}

int DumpSessionRecorder(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        LogAppLifecycleEvent("SESSION_DUMP_FAIL", path.c_str());
        return 0;
    }
    fprintf(f, "# frame,t_ms,src_w,src_h,dst_w,dst_h,au_bytes,keyframe,scaled,scale_us,present_us,mark\n");

    const int stored = g_count < kRingCapacity ? g_count : kRingCapacity;
    // Se vuelca en orden cronologico: el mas antiguo esta en (g_head - stored) del anillo.
    const int start = (g_head - stored + kRingCapacity) % kRingCapacity;
    for (int i = 0; i < stored; ++i) {
        const Slot& s = g_ring[(start + i) % kRingCapacity];
        const FrameRecord& r = s.rec;
        fprintf(f, "%llu,%llu,%d,%d,%d,%d,%d,%d,%d,%u,%u,%s\n",
                static_cast<unsigned long long>(r.frame_index),
                static_cast<unsigned long long>(r.time_ms),
                r.src_w, r.src_h, r.dst_w, r.dst_h,
                r.access_unit_bytes, r.is_keyframe, r.path_scaled,
                r.scale_us, r.present_us, s.mark);
    }
    fclose(f);
    char detail[200];
    std::snprintf(detail, sizeof(detail), "path=%s stored=%d total_calls=%d enabled=%d",
                  path.c_str(), stored, g_count, g_enabled.load() ? 1 : 0);
    LogAppLifecycleEvent("SESSION_DUMP_OK", detail);
    return stored;
}

int DumpSessionSummary(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return 0;

    const uint64_t packets = g_stats.packets;
    const uint64_t frames = g_stats.frames;
    fprintf(f, "RESUMEN DE SESION\n");
    fprintf(f, "  frames presentados      : %llu\n", static_cast<unsigned long long>(frames));
    fprintf(f, "  paquetes comprimidos    : %llu\n", static_cast<unsigned long long>(packets));
    fprintf(f, "  keyframes (IDR)         : %llu", static_cast<unsigned long long>(g_stats.keyframes));
    if (packets) {
        fprintf(f, "  (cada %.1f paquetes, %.2f por segundo a 60fps)\n",
                static_cast<double>(packets) / static_cast<double>(g_stats.keyframes ? g_stats.keyframes : 1),
                (static_cast<double>(g_stats.keyframes) * 60.0) / static_cast<double>(packets));
    } else {
        fprintf(f, "\n");
    }
    fprintf(f, "  cambios de resolucion   : %llu\n", static_cast<unsigned long long>(g_stats.resolution_changes));
    fprintf(f, "  cambios de camino       : %llu\n", static_cast<unsigned long long>(g_stats.path_changes));
    fprintf(f, "\nTAMANO DE UNIDAD DE ACCESO (indica cuanto comprime el servidor)\n");
    if (frames) {
        fprintf(f, "  minimo  : %llu bytes\n", static_cast<unsigned long long>(g_stats.au_min));
        fprintf(f, "  maximo  : %llu bytes\n", static_cast<unsigned long long>(g_stats.au_max));
        fprintf(f, "  media   : %llu bytes\n", static_cast<unsigned long long>(g_stats.au_sum / (packets ? packets : 1)));
        // La relacion max/min es el indicador de oscilacion: si es alta, el servidor esta cambiando
        // mucho su nivel de compresion durante la sesion.
        if (g_stats.au_min > 0) {
            fprintf(f, "  max/min : %.1fx\n",
                    static_cast<double>(g_stats.au_max) / static_cast<double>(g_stats.au_min));
        }
    }
    fprintf(f, "\nDISTRIBUCION DE TAMANOS DE AU (tramos logaritmicos)\n");
    // Las etiquetas se CALCULAN a partir de los mismos limites que usa au_bucket(), en vez de ser
    // una tabla de texto escrita a mano. Asi es imposible que la etiqueta y el calculo se
    // desincronicen: au_bucket() devuelve b para un valor v cuando (v>>b)==1, es decir el tramo
    // [2^b, 2^(b+1)-1]; el tramo 0 es <=1 byte y el ultimo (7) recoge todo lo de 128 bytes o mas.
    for (int i = 0; i < 8; ++i) {
        if (!g_stats.au_buckets[i]) continue;
        char label[24];
        if (i == 0) {
            std::snprintf(label, sizeof(label), "<=1 B");
        } else {
            const unsigned long long hi = (i == 7) ? 0ULL : ((1ULL << (i + 1)) - 1ULL);
            if (i == 7) {
                std::snprintf(label, sizeof(label), ">=128 B");
            } else if (hi < 1024) {
                std::snprintf(label, sizeof(label), "%llu-%llu B",
                              (1ULL << i), hi);
            } else {
                std::snprintf(label, sizeof(label), "%llu-%llu KB",
                              (1ULL << i) / 1024ULL, hi / 1024ULL);
            }
        }
        const double pct = packets ? (100.0 * static_cast<double>(g_stats.au_buckets[i]) / static_cast<double>(packets)) : 0.0;
        fprintf(f, "  %-14s : %8llu  (%.1f%%)\n", label,
                static_cast<unsigned long long>(g_stats.au_buckets[i]), pct);
    }
    fprintf(f, "\nCOMO LEER ESTO\n");
    fprintf(f, "  - 'cambios de resolucion' alto (>5) significa que el servidor esta subiendo y bajando\n");
    fprintf(f, "    la resolucion durante la sesion. Eso produce el salto de nitidez que se percibe.\n");
    fprintf(f, "  - 'max/min' alto (>10x) significa que el nivel de compresion oscila mucho.\n");
    fprintf(f, "  - Si los keyframes son frecuentes (<60 frames de media), el servidor refresca la\n");
    fprintf(f, "    imagen entera a menudo, y eso se ve como una actualizacion periodica.\n");
    fclose(f);
    LogAppLifecycleEvent("SESSION_SUMMARY_OK", path.c_str());
    return 1;
}

}  // namespace opennow::diag
