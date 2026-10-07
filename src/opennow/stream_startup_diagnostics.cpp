#include "stream_startup_diagnostics.hpp"

#include <cstdint>
#include <string>

#ifdef __ORBIS__
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <mutex>
#endif

namespace opennow
{
#ifdef __ORBIS__
namespace
{
std::mutex diagnosticsMutex;
std::mutex actionTraceMutex;
struct ActionTraceRecord { char text[192]; };
constexpr size_t kActionTraceCapacity = 512;
std::array<ActionTraceRecord, kActionTraceCapacity> actionTrace{};
size_t actionTraceRead = 0, actionTraceCount = 0;
uint64_t actionTraceDropped = 0;
std::chrono::steady_clock::time_point actionTraceStarted{};
volatile sig_atomic_t crashPage = -1;
volatile sig_atomic_t crashLaunchState = -1;
volatile sig_atomic_t crashStreamState = -1;

void appendSignalNumber(char* out, size_t& used, size_t capacity, int value)
{
    if (used >= capacity) return;
    char digits[16]; size_t count = 0;
    unsigned int n = value < 0 ? static_cast<unsigned int>(-value) : static_cast<unsigned int>(value);
    do { digits[count++] = static_cast<char>('0' + n % 10); n /= 10; } while (n && count < sizeof(digits));
    if (value < 0 && used < capacity) out[used++] = '-';
    while (count && used < capacity) out[used++] = digits[--count];
}

void appendSignalHex(char* out, size_t& used, size_t capacity, uintptr_t value)
{
    static const char hex[] = "0123456789abcdef";
    if (used + 2 >= capacity) return;
    out[used++] = '0'; out[used++] = 'x';
    bool started = false;
    for (int shift = static_cast<int>(sizeof(value) * 8) - 4; shift >= 0 && used < capacity; shift -= 4) {
        const unsigned digit = static_cast<unsigned>((value >> shift) & 15u);
        if (digit || started || shift == 0) { out[used++] = hex[digit]; started = true; }
    }
}

void appFatalSignalHandler(int signal_number, siginfo_t* info, void*)
{
    // Only async-signal-safe operations here. SA_RESETHAND lets the fault
    // reoccur with the default disposition after this record is written.
    //
    // CRASH_ENTER SE ESCRIBE PRIMERO Y SE SINCRONIZA. El motivo: en la sesion de la 2.94 la app
    // se cerro en pleno juego sin dejar NINGUN rastro, con este manejador instalado. Eso significa
    // que el proceso murio sin llegar aqui, o que llego y no le dio tiempo a escribir el detalle.
    // Con este marcador delante, si el manejador se ejecuta aunque sea un instante, queda
    // constancia en disco de la senal exacta. Si NO aparece, sabemos que la app no fallo: fue el
    // sistema quien la termino (falta de memoria del sistema, cierre forzado).
    {
        char enter[64]; size_t n = 0;
        const char pfx[] = "\n[CRASH_ENTER] signal=";
        for (size_t i = 0; i < sizeof(pfx) - 1; ++i) enter[n++] = pfx[i];
        appendSignalNumber(enter, n, sizeof(enter), signal_number);
        if (n < sizeof(enter)) enter[n++] = '\n';
        const int efd = open("/data/gfnps4/diagnostic.log", O_WRONLY | O_APPEND);
        if (efd >= 0) { (void)write(efd, enter, n); (void)fsync(efd); (void)close(efd); }
    }
    char line[192]; size_t used = 0;
    const char prefix[] = "\n[FATAL_SIGNAL] signal=";
    for (size_t i = 0; i < sizeof(prefix) - 1; ++i) line[used++] = prefix[i];
    appendSignalNumber(line, used, sizeof(line), signal_number);
    const char code[] = " code=";
    for (size_t i = 0; i < sizeof(code) - 1; ++i) line[used++] = code[i];
    appendSignalNumber(line, used, sizeof(line), info ? info->si_code : 0);
    const char addr[] = " address=";
    for (size_t i = 0; i < sizeof(addr) - 1; ++i) line[used++] = addr[i];
    appendSignalHex(line, used, sizeof(line), reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
    const char state[] = " page=";
    for (size_t i = 0; i < sizeof(state) - 1; ++i) line[used++] = state[i];
    appendSignalNumber(line, used, sizeof(line), crashPage);
    const char launch[] = " launch=";
    for (size_t i = 0; i < sizeof(launch) - 1; ++i) line[used++] = launch[i];
    appendSignalNumber(line, used, sizeof(line), crashLaunchState);
    const char stream[] = " stream=";
    for (size_t i = 0; i < sizeof(stream) - 1; ++i) line[used++] = stream[i];
    appendSignalNumber(line, used, sizeof(line), crashStreamState);
    if (used < sizeof(line)) line[used++] = '\n';
    const int fd = open("/data/gfnps4/diagnostic.log", O_WRONLY | O_APPEND);
    if (fd >= 0) { (void)write(fd, line, used); (void)fsync(fd); (void)close(fd); }
}

void appTerminateHandler()
{
    char line[512];
    size_t used = 0;
    const char prefix[] = "\n[CPP_TERMINATE] ";
    std::memcpy(line + used, prefix, sizeof(prefix) - 1);
    used += sizeof(prefix) - 1;
    try {
        const std::exception_ptr current = std::current_exception();
        if (current) {
            try { std::rethrow_exception(current); }
            catch (const std::exception& error) {
                const char* detail = error.what();
                const char label[] = "what=";
                std::memcpy(line + used, label, sizeof(label) - 1);
                used += sizeof(label) - 1;
                while (detail && *detail && used < sizeof(line) - 80) {
                    const char c = *detail++;
                    line[used++] = (c == '\n' || c == '\r') ? ' ' : c;
                }
            }
            catch (...) {
                const char unknown[] = "what=non_std_exception";
                std::memcpy(line + used, unknown, sizeof(unknown) - 1);
                used += sizeof(unknown) - 1;
            }
        } else {
            const char no_exception[] = "reason=terminate_without_active_exception";
            std::memcpy(line + used, no_exception, sizeof(no_exception) - 1);
            used += sizeof(no_exception) - 1;
        }
    } catch (...) {}
    const int n = std::snprintf(line + used, sizeof(line) - used,
                                " page=%d launch=%d stream=%d\n",
                                static_cast<int>(crashPage),
                                static_cast<int>(crashLaunchState),
                                static_cast<int>(crashStreamState));
    if (n > 0) {
        const size_t available = sizeof(line) - used;
        used += static_cast<size_t>(n) < available
            ? static_cast<size_t>(n) : available - 1;
    }
    const int fd = open("/data/gfnps4/diagnostic.log", O_WRONLY | O_APPEND);
    if (fd >= 0) { (void)write(fd, line, used); (void)fsync(fd); (void)close(fd); }
    std::abort();
}

bool IsDurableLifecycleEvent(const char* event)
{
    if (!event || !*event)
        return false;

    const std::string name(event);
    return name == "APP_START" || name == "APP_EXIT" ||
           name == "APP_INIT_FAILED" || name == "GAME_LAUNCH_REQUESTED" ||
           name == "GAME_LAUNCH_THREAD_CREATED" || name == "CLOUD_SESSION_CREATED" ||
           name == "CLOUD_SESSION_WAIT_BEGIN" || name == "CLOUD_SESSION_WAIT_COMPLETE" ||
           name == "CLOUD_SESSION_STATE_PUBLISHED" ||
           name == "UI_HEARTBEAT" || name == "UI_LAUNCH_LOOP_ENTERED" ||
           name == "UI_LAUNCH_PAD_POLL_BEGIN" || name == "UI_LAUNCH_PAD_POLL_COMPLETE" ||
           name == "UI_LAUNCH_DRAW_BEGIN" || name == "UI_LAUNCH_DRAW_COMPLETE" ||
           name == "UI_LAUNCH_DRAW_STAGE" ||
           name == "CLOUD_SESSION_READY" || name == "STREAM_CONNECT_PRESSED" ||
           name == "STREAM_WORKER_START" || name == "STREAM_UI_FIRST_DRAW_BEGIN" ||
           name == "STREAM_UI_FIRST_DRAW_COMPLETE" ||
           name == "STREAM_VIDEO_FIRST_FRAME_DECODED" ||
           name == "CATALOG_START" || name == "CATALOG_STAGE" ||
           name.find("_FAILED") != std::string::npos ||
           name.find("_ERROR") != std::string::npos ||
           name.find("_CRASH") != std::string::npos;
}
}
#endif

void BeginAppLifecycleLog()
{
#ifdef __ORBIS__
    {
        std::lock_guard<std::mutex> lock(actionTraceMutex);
        actionTraceRead = actionTraceCount = 0;
        actionTraceDropped = 0;
        actionTraceStarted = std::chrono::steady_clock::now();
    }
    mkdir("/data/gfnps4", 0777);
    FILE* file = std::fopen("/data/gfnps4/diagnostic.log", "w");
    if (!file) return;
    std::fputs("AJ GeForce NOW PS4 diagnostic log\n", file);
    std::fflush(file);
    fsync(fileno(file));
    std::fclose(file);
    const char* legacy[] = {"app_lifecycle.log", "stream_startup_stage.txt", "session_trace.log",
                            "stream_trace.log", "signaling.log", "input.log", "auth.log"};
    for (const char* name : legacy) {
        char path[192];
        std::snprintf(path, sizeof(path), "/data/gfnps4/%s", name);
        unlink(path);
    }
    unlink("/data/gfnps4/last_stage.txt");
#endif
}

// Persistent stage marker ("heartbeat").
//
// WHY THIS EXISTS: the 2.80 build crashed during a game session and the diagnostic log simply
// STOPPED - no fatal-signal line, no terminate_handler line, even though the handlers are
// installed for SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE and the boot log confirmed
// "failed_mask=0x00" (all registered). When the process dies without the handler running, the
// main log cannot say where it happened, and the only way to find out is to record the stage
// somewhere that survives.
//
// A background thread writes the current stage to its own small file and fsyncs it, so the
// last value on disk is the stage that was running when the process died. A dedicated file is
// used rather than the main log because the main log is buffered and opened for append by
// whichever thread logs, which is exactly what does not get flushed on a hard crash.
namespace {
std::atomic<int> g_currentStage{-1};
std::atomic<bool> g_heartbeatRun{false};
std::thread g_heartbeatThread;
} // namespace

void SetCurrentStage(int stage)
{
    g_currentStage.store(stage, std::memory_order_relaxed);
}

int GetCurrentStage()
{
    return g_currentStage.load(std::memory_order_relaxed);
}

void StartStageHeartbeat()
{
#ifdef __ORBIS__
    bool expected = false;
    if (!g_heartbeatRun.compare_exchange_strong(expected, true)) return; // already running
    g_heartbeatThread = std::thread([] {
        long long beat = 0;
        for (;;) {
            if (!g_heartbeatRun.load(std::memory_order_relaxed)) return;
            // Write the stage and force it to disk. Small file, one write per second.
            int fd = open("/data/gfnps4/last_stage.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                // El nombre se escribe junto al numero para que el fichero se lea sin tabla.
                static const char* const names[] = {
                    "IDLE", "CATALOG", "EVENTS", "INPUT", "STREAM_UI", "DRAW", "PRESENT",
                    "VIDEO_PICK", "VIDEO_CONVERT", "VIDEO_SCALE", "VIDEO_FLIP", "VIDEO_WAIT",
                    // =============================================================================
                    // SUB-ETAPAS DEL DIBUJADO (v3.50) — DEBEN IR EN EL MISMO ORDEN QUE EL ENUM
                    // =============================================================================
                    // El enum esta en `src/ps4/main.cpp` (ambito de fichero, valores 12 a 20).
                    //
                    // MOTIVO DE EXISTIR: el cierre inesperado reportado dejo `stage=5 name=DRAW`, y
                    // `DRAW` cubre TODA la funcion `draw()`. Con estas sub-etapas, la siguiente traza
                    // dira cual de las partes fue.
                    //
                    // OJO: **si se anade una sub-etapa al enum hay que anadirla aqui en la MISMA
                    // posicion**, o el fichero de traza mentira (mostraria el nombre de otra etapa).
                    // Es el precio de tener los nombres en una tabla plana; se documenta aqui para que
                    // no se rompa en silencio.
                    "DRAW_MENU_BEGIN",      // 12
                    "DRAW_STREAM_BEGIN",    // 13
                    "DRAW_STREAM_CLEAR",    // 14
                    "DRAW_STREAM_VIDEO",    // 15
                    "DRAW_STREAM_HUD",      // 16
                    "DRAW_MENU_HEADER",     // 17
                    "DRAW_MENU_CONTENT",    // 18
                    "DRAW_MENU_FOOTER",     // 19
                    "DRAW_PRESENT",         // 20
                    "VIDEO_RENDER_ENTER",   // 21
                    "VIDEO_RENDER_SCALE",   // 22
                    "VIDEO_RENDER_UPLOAD",  // 23
                    "VIDEO_RENDER_COPY",    // 24
                    "VIDEO_RENDER_FALLBACK" // 25
                };
                const int stage = g_currentStage.load(std::memory_order_relaxed);
                const char* name = (stage >= 0 && stage < static_cast<int>(sizeof(names)/sizeof(names[0])))
                                       ? names[stage] : "?";
                char buf[160];
                const int n = std::snprintf(buf, sizeof(buf), "stage=%d name=%s beat=%lld\n",
                                            stage, name, beat);
                if (n > 0) {
                    ssize_t written = write(fd, buf, static_cast<size_t>(n));
                    (void)written;
                }
                fsync(fd);
                close(fd);
            }
            ++beat;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
#endif
}

void StopStageHeartbeat()
{
    g_heartbeatRun.store(false, std::memory_order_relaxed);
    if (g_heartbeatThread.joinable()) g_heartbeatThread.join();
}

void TraceAppAction(const char* action, const char* detail)
{
#ifdef __ORBIS__
    if (!action || !*action) return;
    std::lock_guard<std::mutex> lock(actionTraceMutex);
    const size_t write = (actionTraceRead + actionTraceCount) % kActionTraceCapacity;
    if (actionTraceCount == kActionTraceCapacity) {
        actionTraceRead = (actionTraceRead + 1) % kActionTraceCapacity;
        ++actionTraceDropped;
    } else {
        ++actionTraceCount;
    }
    ActionTraceRecord& record = actionTrace[write];
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - actionTraceStarted).count();
    int used = std::snprintf(record.text, sizeof(record.text), "+%lldms %s",
                             static_cast<long long>(elapsed), action);
    if (detail && *detail && used > 0 && static_cast<size_t>(used) < sizeof(record.text) - 1) {
        record.text[used++] = ' ';
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(detail);
             *p && static_cast<size_t>(used) < sizeof(record.text) - 1; ++p)
            record.text[used++] = (*p == '\r' || *p == '\n') ? ' ' : static_cast<char>(*p);
        record.text[used] = '\0';
    }
#else
    (void)action; (void)detail;
#endif
}

void FlushAppActionTrace()
{
#ifdef __ORBIS__
    std::array<ActionTraceRecord, 64> batch{};
    size_t count = 0;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(actionTraceMutex);
        count = actionTraceCount < batch.size() ? actionTraceCount : batch.size();
        for (size_t i = 0; i < count; ++i) {
            batch[i] = actionTrace[actionTraceRead];
            actionTraceRead = (actionTraceRead + 1) % kActionTraceCapacity;
        }
        actionTraceCount -= count;
        dropped = actionTraceDropped;
        actionTraceDropped = 0;
    }
    if (!count && !dropped) return;
    char text[64 * 192 + 64];
    size_t used = 0;
    for (size_t i = 0; i < count && used < sizeof(text) - 2; ++i) {
        const size_t length = std::strlen(batch[i].text);
        if (length > sizeof(text) - used - 2) break;
        std::memcpy(text + used, batch[i].text, length);
        used += length; text[used++] = '\n';
    }
    if (dropped && used < sizeof(text) - 2) {
        const int n = std::snprintf(text + used, sizeof(text) - used,
                                    "TRACE_OVERFLOW dropped=%llu\n",
                                    static_cast<unsigned long long>(dropped));
        if (n > 0) used += static_cast<size_t>(n);
    }
    text[used] = '\0';
    AppendDiagnosticLogBlock("ACTION", text, false);
#endif
}

void InstallAppCrashDiagnostics()
{
#ifdef __ORBIS__
    const int signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    struct sigaction action{};
    action.__sa_handler.__sa_sigaction = appFatalSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    unsigned int installed = 0;
    unsigned int failed = 0;
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        if (sigaction(signals[i], &action, nullptr) == 0)
            installed |= (1u << i);
        else
            failed |= (1u << i);
    }
    std::set_terminate(appTerminateHandler);
    char detail[80];
    std::snprintf(detail, sizeof(detail), "signal_mask=0x%02x failed_mask=0x%02x terminate_handler=1",
                  installed, failed);
    LogAppLifecycleEvent("FATAL_HANDLERS_INSTALLED", detail);
#endif
}

void SetAppCrashContext(int page, int launch_state, int stream_state)
{
#ifdef __ORBIS__
    crashPage = page;
    crashLaunchState = launch_state;
    crashStreamState = stream_state;
#else
    (void)page; (void)launch_state; (void)stream_state;
#endif
}

void AppendDiagnosticLogBlock(const char* source, const char* text, bool sync_to_disk)
{
#ifdef __ORBIS__
    std::lock_guard<std::mutex> lock(diagnosticsMutex);
    mkdir("/data/gfnps4", 0777);
    FILE* file = std::fopen("/data/gfnps4/diagnostic.log", "a");
    if (!file)
        return;
    static const auto started = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::fprintf(file, "+%lldms [%s] ", static_cast<long long>(elapsed), source ? source : "APP");
    const size_t length = text ? std::strlen(text) : 0;
    if (length) std::fwrite(text, 1, length, file);
    if (!length || text[length - 1] != '\n') std::fputc('\n', file);
    std::fflush(file);
    if (sync_to_disk) fsync(fileno(file));
    std::fclose(file);
#else
    (void)source;
    (void)text;
    (void)sync_to_disk;
#endif
}

void LogAppLifecycleEvent(const char* event, const char* detail)
{
    std::string line = event ? event : "event";
    if (detail && *detail) {
        line.push_back(' ');
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(detail); *p; ++p)
            line.push_back((*p == '\r' || *p == '\n') ? ' ' : static_cast<char>(*p));
    }
    // UI events are frequent and run on the render/input thread. Flushing and
    // fsyncing each one caused storage stalls during navigation and stream
    // startup. Keep the unified log flushed by stdio, and force durable writes
    // only for lifecycle boundaries and failures.
    AppendDiagnosticLogBlock("APP", line.c_str(), IsDurableLifecycleEvent(event));
}

void WriteStreamStartupStage(const char* stage)
{
#ifdef __ORBIS__
    // These checkpoints are specifically used to identify the last completed
    // stage after a native crash, so keep them durable in the single log.
    const std::string detail = std::string("STREAM_STAGE ") +
                               (stage ? stage : "unknown");
    AppendDiagnosticLogBlock("APP", detail.c_str(), true);
#else
    LogAppLifecycleEvent("STREAM_STAGE", stage ? stage : "unknown");
#endif
}

} // namespace opennow

extern "C" void opennow_write_stream_startup_stage_from_c(const char* stage)
{
    opennow::WriteStreamStartupStage(stage);
}

extern "C" void opennow_log_app_lifecycle_from_c(const char* event, const char* detail)
{
    opennow::LogAppLifecycleEvent(event, detail);
}
