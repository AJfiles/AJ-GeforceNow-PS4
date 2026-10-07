// Implementacion del probe del decodificador por hardware. Ver videodec2_probe.hpp.
#include "videodec2_probe.hpp"

#include "videodec2_abi.hpp"
#include "videodec2_load.hpp"
#include "../stream_startup_diagnostics.hpp"

#include <cstdio>
#include <cstring>

namespace opennow::videodec2_probe {

using namespace opennow::videodec2;

namespace {

const char* RcText(int32_t rc) {
    if (rc == 0) return "OK";
    // Comparaciones con if en vez de switch: los codigos son `static_cast<s32>` de constantes
    // hexadecimales, y un switch con case no constantes no compila.
    if (rc == ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER) return "ARGUMENT_POINTER";
    if (rc == ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE)      return "STRUCT_SIZE";
    if (rc == ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE) return "DECODER_INSTANCE";
    if (rc == ORBIS_VIDEODEC2_ERROR_COMPUTE_PIPE_ID)  return "COMPUTE_PIPE_ID";
    if (rc == ORBIS_VIDEODEC2_ERROR_COMPUTE_QUEUE_ID) return "COMPUTE_QUEUE_ID";
    if (rc == ORBIS_VIDEODEC2_ERROR_MEMORY_POINTER)   return "MEMORY_POINTER";
    if (rc == ORBIS_VIDEODEC2_ERROR_CONFIG_INFO)      return "CONFIG_INFO";
    // Los codigos de "modulo no disponible" del cargador llegan con el bit alto del rango de
    // bibliotecas (0x805A....), que no esta en la tabla de errores de la API. Se etiquetan aparte
    // para que quede claro que el fallo es de CARGA del modulo, no de la ABI.
    // Codigos de CARGA DE MODULO (no de la API). Se distinguen del resto porque cambian por
    // completo la interpretacion del resultado: si el fallo es de carga, ningun ajuste de la ABI lo
    // arregla y hay que cambiar COMO se obtiene el modulo.
    if ((static_cast<uint32_t>(rc) & 0xFFFF0000u) == 0x805A0000u) return "SYSMODULE_ERROR(load)";
    // 0x800200D9 / 0x80020002: la libreria no esta resuelta en el proceso (el cargador dinamico no
    // encontro el modulo con el que se enlazo el cliente). Distinto de "no existe el ID".
    if (static_cast<uint32_t>(rc) == 0x800200D9u) return "LIBRARY_NOT_RESOLVED";
    if (static_cast<uint32_t>(rc) == 0x80020002u) return "LIBRARY_NOT_LOADED";
    // Errores genericos del cargador de modulos.
    if (static_cast<uint32_t>(rc) == 0x800200CBu) return "MODULE_ALREADY_LOADED";
    return "?";
}

void LogStep(const char* step, int32_t rc) {
    char d[128];
    std::snprintf(d, sizeof(d), "step=%s rc=0x%08X (%s)", step,
                  static_cast<unsigned>(rc), RcText(rc));
    LogAppLifecycleEvent("HWVDEC_PROBE", d);
}

}  // namespace

HardwareDecodeProbeResult RunHardwareDecodeProbe() {
    HardwareDecodeProbeResult r;

    LogAppLifecycleEvent("HWVDEC_PROBE_BEGIN",
                         "sequence=query_compute_alloc_queue_query_decoder_create_decoder");

    // =====================================================================
    // GUARDIA OBLIGATORIA. NO LLAMAR A LA API SIN MODULO CARGADO.
    //
    // En la 3.17 el probe llamo directamente a `sceVideodec2QueryComputeMemoryInfo()` y la consola
    // devolvio CE-34878-0 (segmentation fault) al instante. La razon: los stubs de OpenOrbis los
    // rellena el cargador dinamico con la direccion real del modulo; si el modulo NO esta cargado,
    // esa direccion no se resuelve y la llamada SALTA A UNA DIRECCION NULA. No devuelve un error:
    // termina el proceso.
    //
    // Por eso la carga se comprueba primero, y si no hay exito se ABORTA sin tocar la API. Asi el
    // peor caso es "el hardware no esta disponible", nunca un cierre de la aplicacion.
    // =====================================================================
    const videodec2_load::ModuleLoadState load = videodec2_load::LoadVideoDecoderModule();
    if (!load.api_callable) {
        LogAppLifecycleEvent(
            "HWVDEC_PROBE_RESULT",
            "viable=0 aborted=module_not_loaded guard=held app_stays_alive fallback=software");
        return r;   // <- se sale ANTES de cualquier llamada a sceVideodec2*
    }

    LogAppLifecycleEvent("HWVDEC_GUARD_PASSED", "module_loaded_api_callable=1");

#ifdef __ORBIS__
    // ---------------------------------------------------------------------
    // PASO 1: cuanto necesita la COLA de computo
    // ---------------------------------------------------------------------
    OrbisVideodec2ComputeMemoryInfo compute_mem{};
    compute_mem.this_size = sizeof(compute_mem);
    r.compute_memory_rc = sceVideodec2QueryComputeMemoryInfo(&compute_mem);
    LogStep("1_query_compute_memory", r.compute_memory_rc);

    if (r.compute_memory_rc != 0) {
        // Fallo en el primer paso: si el codigo es LIBRARY_NOT_FOUND, el problema esta en la carga
        // del modulo y ninguna ABI lo arreglaria.
        LogAppLifecycleEvent("HWVDEC_PROBE_RESULT", "viable=0 stopped_at=1_query_compute_memory");
        return r;
    }

    {
        char d[160];
        std::snprintf(d, sizeof(d), "cpu_gpu_memory_size=%llu bytes",
                      static_cast<unsigned long long>(compute_mem.cpu_gpu_memory_size));
        LogAppLifecycleEvent("HWVDEC_COMPUTE_MEMORY", d);
    }

    // ---------------------------------------------------------------------
    // PASO 2: crear la COLA (el paso que faltaba)
    // ---------------------------------------------------------------------
    OrbisVideodec2ComputeConfigInfo compute_cfg{};
    compute_cfg.this_size = sizeof(compute_cfg);
    compute_cfg.compute_pipe_id = 0;
    compute_cfg.compute_queue_id = 0;
    compute_cfg.check_memory_type = false;

    OrbisVideodec2ComputeQueue queue = nullptr;
    r.compute_config_rc = 0;  // la config se valida dentro de AllocateComputeQueue
    r.allocate_queue_rc = sceVideodec2AllocateComputeQueue(&compute_cfg, &compute_mem, &queue);
    LogStep("2_allocate_compute_queue", r.allocate_queue_rc);

    if (r.allocate_queue_rc != 0) {
        LogAppLifecycleEvent("HWVDEC_PROBE_RESULT", "viable=0 stopped_at=2_allocate_compute_queue");
        return r;
    }
    r.compute_queue_handle = reinterpret_cast<uint64_t>(queue);
    {
        char d[96];
        std::snprintf(d, sizeof(d), "compute_queue=0x%llX",
                      static_cast<unsigned long long>(r.compute_queue_handle));
        LogAppLifecycleEvent("HWVDEC_QUEUE_ALLOCATED", d);
    }

    // ---------------------------------------------------------------------
    // PASO 3: ahora SI, cuanto necesita el DECODIFICADOR
    //
    // La configuracion usa los valores reales de un titulo de PS4 medidos con prosper:
    // codec AVC, perfil High (100), nivel 4.1 (41), 1920x1088 y profundidad de pipeline 4.
    // ---------------------------------------------------------------------
    OrbisVideodec2DecoderConfigInfo cfg{};
    cfg.this_size = sizeof(cfg);
    cfg.resource_type = 1;
    cfg.codec_type = OrbisVideodec2CodecType::Avc;
    cfg.profile = 100;              // High
    cfg.max_level = 41;             // 4.1
    cfg.max_frame_width = 1920;
    cfg.max_frame_height = 1088;
    cfg.max_dpb_frame_count = -1;   // automatico
    cfg.decode_pipeline_depth = 4;
    cfg.compute_queue = queue;      // <- la cola recien creada
    cfg.cpu_affinity_mask = 0x1fff;
    cfg.cpu_thread_priority = 700;
    cfg.optimize_progressive_video = true;
    cfg.check_memory_type = false;
    cfg.extra_config_info = nullptr;

    OrbisVideodec2DecoderMemoryInfo decoder_mem{};
    decoder_mem.this_size = sizeof(decoder_mem);
    r.decoder_memory_rc = sceVideodec2QueryDecoderMemoryInfo(&cfg, &decoder_mem);
    LogStep("3_query_decoder_memory", r.decoder_memory_rc);

    if (r.decoder_memory_rc != 0) {
        LogAppLifecycleEvent("HWVDEC_PROBE_RESULT", "viable=0 stopped_at=3_query_decoder_memory");
        return r;
    }
    {
        char d[200];
        std::snprintf(d, sizeof(d),
                      "cpu=%llu gpu=%llu cpu_gpu=%llu max_frame=%llu align=%u",
                      static_cast<unsigned long long>(decoder_mem.cpu_memory_size),
                      static_cast<unsigned long long>(decoder_mem.gpu_memory_size),
                      static_cast<unsigned long long>(decoder_mem.cpu_gpu_memory_size),
                      static_cast<unsigned long long>(decoder_mem.max_frame_buffer_size),
                      static_cast<unsigned>(decoder_mem.frame_buffer_alignment));
        LogAppLifecycleEvent("HWVDEC_DECODER_MEMORY", d);
    }

    // ---------------------------------------------------------------------
    // PASO 4: crear el decodificador
    // ---------------------------------------------------------------------
    OrbisVideodec2Decoder decoder = nullptr;
    r.create_decoder_rc = sceVideodec2CreateDecoder(&cfg, &decoder_mem, &decoder);
    LogStep("4_create_decoder", r.create_decoder_rc);

    r.decoder_created = (r.create_decoder_rc == 0) && (decoder != nullptr);
    r.viable = r.decoder_created;

    if (r.decoder_created) {
        // Se libera inmediatamente: este probe comprueba VIABILIDAD, no deja el decodificador
        // abierto. Dejarlo vivo consumiria un recurso compartido con el sistema sin usarlo.
        const int32_t del_rc = sceVideodec2DeleteDecoder(decoder);
        LogStep("5_delete_decoder", del_rc);
    }

    LogAppLifecycleEvent("HWVDEC_PROBE_RESULT",
                         r.viable ? "viable=1 hardware_decoder_created_and_released"
                                  : "viable=0 stopped_at=4_create_decoder");
#else
    LogAppLifecycleEvent("HWVDEC_PROBE", "step=skipped rc=0x00000000 reason=not_orbis_host_build");
    LogAppLifecycleEvent("HWVDEC_PROBE_RESULT", "viable=0 reason=not_orbis");
#endif

    return r;
}

}  // namespace opennow::videodec2_probe
