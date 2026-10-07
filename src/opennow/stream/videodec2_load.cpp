// Implementacion de la carga con guardia del modulo del decodificador. Ver videodec2_load.hpp.
#include "videodec2_load.hpp"

#include "../stream_startup_diagnostics.hpp"

#include <cstdio>

#ifdef __ORBIS__
#include <orbis/Sysmodule.h>
#endif

namespace opennow::videodec2_load {

// ID interno del nucleo del decodificador de video.
//
// El SDK solo lo declara como enumerado (`ORBIS_SYSMODULE_INTERNAL_VDECCORE`), asi que se usa el
// valor numerico directamente para que la constante quede visible junto al comentario y no dependa
// de que la cabecera este incluida en este punto.
constexpr uint32_t kVdecCoreInternalId = 0x80000015u;

ModuleLoadState LoadVideoDecoderModule() {
    ModuleLoadState state;

#ifdef __ORBIS__
    // ------------------------------------------------------------------
    // VIA 1: carga publica del modulo del decodificador
    // ------------------------------------------------------------------
    // Medido en consola en repetidas versiones: devuelve 0x805A1000
    // (SCE_SYSMODULE_ERROR_UNKNOWN). Se intenta igualmente porque es la via documentada y podria
    // funcionar en otro firmware.
    state.public_load_rc =
        static_cast<int32_t>(sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC2));

    if (state.public_load_rc == 0) {
        state.api_callable = true;
        state.path = "public_sysmodule_load";
        LogModuleLoadState(state);
        return state;
    }

    // ------------------------------------------------------------------
    // VIA 2: carga INTERNA del nucleo del decodificador
    // ------------------------------------------------------------------
    // Los IDs con el prefijo 0x80000000 pertenecen a la API interna
    // (`sceSysmoduleLoadModuleInternal`), no a la publica. Cargar el nucleo (libSceVdecCore) es el
    // requisito previo mas probable para que la capa de videodec2 quede resuelta.
    //
    // Un valor ya cargado no es un fallo: se acepta como exito para no abortar una ruta valida.
    state.internal_load_rc =
        static_cast<int32_t>(sceSysmoduleLoadModuleInternal(
            static_cast<OrbisSysModuleInternal>(kVdecCoreInternalId)));

    const bool internal_ok =
        (state.internal_load_rc == 0) ||
        (static_cast<uint32_t>(state.internal_load_rc) == 0x800200CBu);  // ya cargado

    if (internal_ok) {
        // Con el nucleo cargado, se REINTENTA la via publica: es la comprobacion de si el nucleo
        // era el requisito que faltaba.
        state.verify_rc =
            static_cast<int32_t>(sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC2));
        if (state.verify_rc == 0 ||
            static_cast<uint32_t>(state.verify_rc) == 0x800200CBu) {
            state.api_callable = true;
            state.path = "internal_vdeccore_then_public";
        } else {
            // El nucleo cargo pero la capa de videodec2 sigue sin resolverse. NO se toca la API.
            state.api_callable = false;
            state.path = "internal_vdeccore_only";
        }
    } else {
        state.api_callable = false;
        state.path = "none";
    }
#else
    state.public_load_rc = 0;
    state.api_callable = false;
    state.path = "not_orbis";
#endif

    LogModuleLoadState(state);
    return state;
}

void LogModuleLoadState(const ModuleLoadState& state) {
    char d[224];
    std::snprintf(d, sizeof(d),
                  "public_rc=0x%08X internal_rc=0x%08X verify_rc=0x%08X api_callable=%d path=%s",
                  static_cast<unsigned>(state.public_load_rc),
                  static_cast<unsigned>(state.internal_load_rc),
                  static_cast<unsigned>(state.verify_rc),
                  state.api_callable ? 1 : 0,
                  state.path);
    LogAppLifecycleEvent("HWVDEC_MODULE_LOAD", d);
}

}  // namespace opennow::videodec2_load
