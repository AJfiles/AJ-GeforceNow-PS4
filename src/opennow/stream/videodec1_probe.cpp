// Implementacion del probe de la API v1 (libSceVideodec). Ver videodec1_probe.hpp.
#include "videodec1_probe.hpp"

#include "videodec_abi.hpp"
#include "../stream_startup_diagnostics.hpp"

#include <cstdio>

#ifdef __ORBIS__
#include <orbis/Sysmodule.h>
#endif

namespace opennow::videodec1_probe {

using namespace opennow::videodec;

namespace {

void LogStep(const char* step, int32_t rc) {
    char d[128];
    std::snprintf(d, sizeof(d), "api=v1 step=%s rc=0x%08X", step, static_cast<unsigned>(rc));
    LogAppLifecycleEvent("HWVDEC1_PROBE", d);
}

}  // namespace

VideoDecodeV1ProbeResult RunVideoDecodeV1Probe() {
    VideoDecodeV1ProbeResult r;

#ifdef __ORBIS__
    // =====================================================================
    // GUARDIA: cargar el modulo ANTES de tocar la API.
    //
    // Es la leccion de la 3.17: llamar a un stub sin el modulo cargado salta a una direccion nula
    // y termina el proceso con CE-34878-0. Para la v1 la carga SI funciona (medido: rc=0x00000000
    // en todas las versiones), pero la comprobacion se hace igual porque es la unica forma de que
    // el peor caso sea "no disponible" y nunca un cierre.
    // =====================================================================
    r.load_rc = static_cast<int32_t>(sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC));
    LogStep("0_load_module", r.load_rc);

    const bool already_loaded = (static_cast<uint32_t>(r.load_rc) == 0x800200CBu);
    if (r.load_rc != 0 && !already_loaded) {
        LogAppLifecycleEvent("HWVDEC1_PROBE_RESULT",
                             "viable=0 aborted=module_not_loaded guard=held app_stays_alive");
        return r;
    }
    r.guard_passed = true;
    LogAppLifecycleEvent("HWVDEC1_GUARD_PASSED", "module=VIDEODEC api_callable=1");

    // ---------------------------------------------------------------------
    // PASO 1: cuanto necesita el decodificador
    //
    // La configuracion usa los valores de un stream real de GeForce NOW: H.264 (codecType 1),
    // perfil High (100) y nivel 4.1 (41), 1920x1088 (la altura se alinea a macrobloque).
    // ---------------------------------------------------------------------
    OrbisVideodecConfigInfo cfg{};
    cfg.thisSize = sizeof(cfg);
    cfg.codecType = 1;              // AVC / H.264
    cfg.profile = 100;              // High
    cfg.maxLevel = 41;              // 4.1
    cfg.maxFrameWidth = 1920;
    cfg.maxFrameHeight = 1088;
    cfg.maxDpbFrameCount = -1;      // automatico
    cfg.videodecFlags = 0;

    OrbisVideodecResourceInfo res{};
    res.thisSize = sizeof(res);
    r.query_resource_rc = sceVideodecQueryResourceInfo(&cfg, &res);
    LogStep("1_query_resource_info", r.query_resource_rc);

    if (r.query_resource_rc != 0) {
        LogAppLifecycleEvent("HWVDEC1_PROBE_RESULT",
                             "viable=0 stopped_at=1_query_resource_info");
        return r;
    }
    {
        char d[200];
        std::snprintf(d, sizeof(d),
                      "cpu_mem=%llu cpu_gpu_mem=%llu max_frame=%llu align=%u",
                      static_cast<unsigned long long>(res.cpuMemorySize),
                      static_cast<unsigned long long>(res.cpuGpuMemorySize),
                      static_cast<unsigned long long>(res.maxFrameBufferSize),
                      static_cast<unsigned>(res.frameBufferAlignment));
        LogAppLifecycleEvent("HWVDEC1_RESOURCE_INFO", d);
    }

    // ---------------------------------------------------------------------
    // PASO 2: crear el decodificador (SIN cola de computo: esa es la ventaja de la v1)
    // ---------------------------------------------------------------------
    OrbisVideodecCtrl ctrl{};
    ctrl.thisSize = sizeof(ctrl);
    r.create_decoder_rc = sceVideodecCreateDecoder(&cfg, &res, &ctrl);
    LogStep("2_create_decoder", r.create_decoder_rc);

    r.decoder_created = (r.create_decoder_rc == 0) && (ctrl.handle != nullptr);
    r.viable = r.decoder_created;

    if (r.decoder_created) {
        // Se libera de inmediato: este probe comprueba VIABILIDAD, no deja el decodificador
        // abierto consumiendo un recurso compartido con el sistema.
        r.delete_decoder_rc = sceVideodecDeleteDecoder(&ctrl);
        LogStep("3_delete_decoder", r.delete_decoder_rc);
    }

    LogAppLifecycleEvent("HWVDEC1_PROBE_RESULT",
                         r.viable
                             ? "viable=1 v1_hardware_decoder_created_and_released"
                             : "viable=0 stopped_at=2_create_decoder");
#else
    LogAppLifecycleEvent("HWVDEC1_PROBE_RESULT", "viable=0 reason=not_orbis_host_build");
#endif

    return r;
}

}  // namespace opennow::videodec1_probe
