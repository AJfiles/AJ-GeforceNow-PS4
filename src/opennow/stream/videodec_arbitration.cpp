// Implementacion de la arbitracion del decodificador de video. Ver videodec_arbitration.hpp.
#include "videodec_arbitration.hpp"

#include <cstdio>

#include "../stream_startup_diagnostics.hpp"

namespace opennow::videodec_arb {

ArbitrationResult EnableVideoDecoderArbitration() {
    ArbitrationResult result;

#ifdef __ORBIS__
    // ORDEN IMPORTANTE: primero Initialize (pedir el recurso al sistema), despues Enable.
    // Si Initialize falla, NO se llama a Enable: hacerlo sobre un subsistema sin inicializar
    // podria devolver un error distinto y enmascarar la causa real.
    result.init_rc = sceVideoDecoderArbitrationInitialize();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "step=initialize rc=0x%08X",
                  static_cast<unsigned>(result.init_rc));
    LogAppLifecycleEvent("VDEC_ARB_INIT", detail);

    if (result.init_rc == 0) {
        result.enable_rc = sceVideoDecoderArbitrationEnable();
        std::snprintf(detail, sizeof(detail), "step=enable rc=0x%08X",
                      static_cast<unsigned>(result.enable_rc));
        LogAppLifecycleEvent("VDEC_ARB_ENABLE", detail);

        if (result.enable_rc == 0) {
            // El modo de suspension se pide DESHABILITADO: queremos el decodificador de forma
            // continua durante el stream. Si el sistema necesita retirarlo (grabacion del usuario),
            // devolvera un evento que se puede atender con AcceptEvent.
            result.suspend_rc = sceVideoDecoderArbitrationEnableSuspendMode(0);
            std::snprintf(detail, sizeof(detail), "step=suspend_mode_off rc=0x%08X",
                          static_cast<unsigned>(result.suspend_rc));
            LogAppLifecycleEvent("VDEC_ARB_SUSPEND", detail);
            result.usable = true;
        }
    }
#else
    // En host no hay modulo de arbitracion: se declara no usable sin fingir exito.
    LogAppLifecycleEvent("VDEC_ARB_INIT", "step=skipped reason=not_orbis");
#endif

    LogAppLifecycleEvent("VDEC_ARB_RESULT",
                         result.usable ? "usable=1 video_decoder_resource_acquired"
                                       : "usable=0 falling_back_to_software");
    return result;
}

}  // namespace opennow::videodec_arb
