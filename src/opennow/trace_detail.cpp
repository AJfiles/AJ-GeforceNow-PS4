// =====================================================================================================
// IMPLEMENTACION DE LA TRAZA DETALLADA. Ver `trace_detail.hpp` para el porque y para las columnas.
// =====================================================================================================
#include "trace_detail.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef __ORBIS__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace opennow
{
namespace trace
{
namespace
{

std::mutex traceMutex;
bool traceStarted = false;
std::chrono::steady_clock::time_point traceStart{};

// ---------------------------------------------------------------------------------------------------
// TOPES. Un stream largo no puede llenar el disco ni comerse la memoria.
// ---------------------------------------------------------------------------------------------------
// El CSV es lo mas voluminoso: una fila por frame a 60 fps son 216.000 filas por hora. Con ~120 bytes
// por fila serian ~26 MB/hora. Por eso el bufer en memoria esta acotado y, al llegar al tope, **se
// deja de acumular y se anota una vez**. Es preferible perder el final de una sesion larguisima que
// llenar el disco del usuario.
constexpr size_t kCsvBufferLimitBytes = 4u * 1024u * 1024u;   // 4 MB en memoria
constexpr size_t kCsvLineCapacity    = 256u;

std::string csvBuffer;
bool csvOverflowed = false;
uint64_t csvRowsWritten = 0;

void appendLineToFile(const char* path, const char* text, bool sync)
{
#ifdef __ORBIS__
    const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    const size_t length = std::strlen(text);
    if (length) (void)write(fd, text, length);
    if (sync) (void)fsync(fd);
    (void)close(fd);
#else
    (void)path; (void)text; (void)sync;
#endif
}

// Construye "+<ms> [TRAZA] texto\n" y lo escribe. `sync` solo en los hitos criticos.
void writeTimedLine(const char* path, const char* text, bool sync)
{
    char line[512];
    const long long ms = TraceNowMs();
    std::snprintf(line, sizeof(line), "+%lldms [TRAZA] %s\n", ms, text ? text : "");
    appendLineToFile(path, line, sync);
}

} // namespace

long long TraceNowMs()
{
    if (!traceStarted) return 0;
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - traceStart).count());
}

void StartTrace()
{
#ifdef __ORBIS__
    std::lock_guard<std::mutex> lock(traceMutex);
    if (traceStarted) return;
    traceStart = std::chrono::steady_clock::now();
    traceStarted = true;
    mkdir("/data/gfnps4", 0777);

    // Se TRUNCAN los dos ficheros: esta traza es de la ejecucion actual, no historica. El log unificado
    // (`diagnostic.log`) es el que conserva el historial.
    {
        FILE* f = std::fopen("/data/gfnps4/trace_boot.txt", "w");
        if (f) {
            std::fputs("TRAZA DETALLADA DE ARRANQUE - AJ GeForce NOW PS4\n", f);
            std::fputs("Cada linea: +<ms desde el inicio de la traza> [PASO] detalle\n", f);
            std::fflush(f);
            fsync(fileno(f));
            std::fclose(f);
        }
    }
    {
        FILE* f = std::fopen("/data/gfnps4/trace_stream.csv", "w");
        if (f) {
            // Cabecera explicita: cada columna responde a una pregunta concreta (ver el .hpp).
            std::fputs("t_ms,frame,gen,reused,src_w,src_h,dst_w,dst_h,scaled,q,dec_fps,pres_us,budget_us,over\n", f);
            std::fflush(f);
            std::fclose(f);
        }
    }
    {
        FILE* f = std::fopen("/data/gfnps4/trace_stream.txt", "w");
        if (f) {
            std::fputs("TRAZA DE LA SESION DE JUEGO - hitos\n", f);
            std::fflush(f);
            std::fclose(f);
        }
    }
    writeTimedLine("/data/gfnps4/trace_boot.txt", "TRAZA_INICIADA", true);
#else
    traceStart = std::chrono::steady_clock::now();
    traceStarted = true;
#endif
}

void BootStep(const char* step, const char* detail)
{
    if (!traceStarted) return;
    std::lock_guard<std::mutex> lock(traceMutex);
    char text[384];
    if (detail && *detail) {
        std::snprintf(text, sizeof(text), "%-38s %s", step ? step : "?", detail);
    } else {
        std::snprintf(text, sizeof(text), "%s", step ? step : "?");
    }
    writeTimedLine("/data/gfnps4/trace_boot.txt", text, false);
}

void BootStepSync(const char* step, const char* detail)
{
    if (!traceStarted) return;
    std::lock_guard<std::mutex> lock(traceMutex);
    char text[384];
    if (detail && *detail) {
        std::snprintf(text, sizeof(text), "%-38s %s", step ? step : "?", detail);
    } else {
        std::snprintf(text, sizeof(text), "%s", step ? step : "?");
    }
    writeTimedLine("/data/gfnps4/trace_boot.txt", text, true);
}

void StreamEvent(const char* event, const char* detail)
{
    if (!traceStarted) return;
    std::lock_guard<std::mutex> lock(traceMutex);
    char text[384];
    if (detail && *detail) {
        std::snprintf(text, sizeof(text), "%-34s %s", event ? event : "?", detail);
    } else {
        std::snprintf(text, sizeof(text), "%s", event ? event : "?");
    }
    writeTimedLine("/data/gfnps4/trace_stream.txt", text, false);
}

void FrameRow(uint64_t frameIndex, uint64_t generation, int reused,
              int srcW, int srcH, int dstW, int dstH, int scaled,
              int queueDepth, int decodeFps, uint64_t presentUs)
{
    if (!traceStarted) return;
    std::lock_guard<std::mutex> lock(traceMutex);

    if (csvBuffer.size() >= kCsvBufferLimitBytes) {
        if (!csvOverflowed) {
            csvOverflowed = true;
            writeTimedLine("/data/gfnps4/trace_stream.txt",
                           "AVISO: bufer del CSV lleno (4 MB); se deja de acumular por frame", true);
        }
        return;
    }

    char row[kCsvLineCapacity];
    constexpr uint64_t kBudgetUs = 16666;
    const int over = (presentUs > kBudgetUs) ? 1 : 0;
    const int n = std::snprintf(row, sizeof(row), "%lld,%llu,%llu,%d,%d,%d,%d,%d,%d,%d,%d,%llu,%llu,%d\n",
                                 TraceNowMs(),
                                 static_cast<unsigned long long>(frameIndex),
                                 static_cast<unsigned long long>(generation),
                                 reused, srcW, srcH, dstW, dstH, scaled,
                                 queueDepth, decodeFps,
                                 static_cast<unsigned long long>(presentUs),
                                 static_cast<unsigned long long>(kBudgetUs), over);
    if (n > 0) {
        csvBuffer.append(row, static_cast<size_t>(n));
        ++csvRowsWritten;
    }
}

void FlushTrace()
{
    if (!traceStarted) return;
    std::lock_guard<std::mutex> lock(traceMutex);
    if (csvBuffer.empty()) return;
    appendLineToFile("/data/gfnps4/trace_stream.csv", csvBuffer.c_str(), false);
    csvBuffer.clear();
}

void StreamSecond(int loopFps, int drawMs, int iterMs, uint64_t framesThisSecond,
                  int queueDepth, int decodeFps, int presentedTotal)
{
    if (!traceStarted) return;
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        char text[320];
        std::snprintf(text, sizeof(text),
                      "loop_fps=%-3d draw_ms=%-3d iter_ms=%-3d frames=%llu q=%d dec_fps=%d presentados=%d csv_filas=%llu",
                      loopFps, drawMs, iterMs,
                      static_cast<unsigned long long>(framesThisSecond),
                      queueDepth, decodeFps, presentedTotal,
                      static_cast<unsigned long long>(csvRowsWritten));
        writeTimedLine("/data/gfnps4/trace_stream.txt", text, false);
    }
    // El volcado va FUERA del candado de arriba para no bloquear la toma de datos mientras se escribe.
    FlushTrace();
}

uint64_t FrameRowsRecorded()
{
    // Sin candado a proposito: solo lo escribe el hilo de presentacion y solo lo lee ese mismo hilo.
    // Poner un candado aqui seria contencion gratuita en el camino que se esta midiendo.
    return csvRowsWritten;
}

void StopTrace()
{
    if (!traceStarted) return;
    FlushTrace();
    std::lock_guard<std::mutex> lock(traceMutex);
    writeTimedLine("/data/gfnps4/trace_boot.txt", "TRAZA_CERRADA_ORDENADAMENTE", true);
}

} // namespace trace
} // namespace opennow
