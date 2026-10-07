// Carga del modulo del decodificador de video por hardware, CON GUARDIA.
//
// POR QUE EXISTE ESTE FICHERO
// ---------------------------
// En la 3.17 el probe llamo directamente a `sceVideodec2QueryComputeMemoryInfo()` sin comprobar
// antes que el modulo estuviera cargado. **Resultado: CE-34878-0 (segmentation fault) inmediato.**
//
// El motivo es el que explico el analisis: los stubs de OpenOrbis son "stub vacios" que el cargador
// dinamico rellena con la direccion real. Si el modulo NO esta cargado, esa direccion nunca se
// resuelve y la llamada **salta a una direccion nula**. No es que devuelva un error: **crashea**.
//
// Por tanto la regla es tajante:
//
//     NO SE LLAMA A NINGUNA FUNCION DE libSceVideodec2 HASTA QUE LA CARGA DEL MODULO
//     HAYA DEVUELTO EXITO. SI NO HAY EXITO, SE ABORTA SIN TOCAR LA API.
//
// LAS DOS VIAS DE CARGA
// ---------------------
//   Via 1 (usuario):  sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC2 = 0x00CF)
//                     -> medido en consola: 0x805A1000 = SCE_SYSMODULE_ERROR_UNKNOWN
//   Via 2 (interna):  sceSysmoduleLoadModuleInternal(0x80000015 = VDECCORE)
//                     -> es la API que corresponde a los IDs con el prefijo 0x80000000
//
// La 3.17 demostro que la Via 1 sola no basta. Aqui se intenta la Via 1 y, si falla, la Via 2, y
// SOLO si alguna tiene exito se permite llamar a la API.
//
// El modulo VDECCORE es el nucleo del decodificador (libSceVdecCore), del que dependen las capas de
// mas alto nivel. Cargar el nucleo es el requisito previo mas probable para que la libreria de
// videodec2 quede resuelta.
#pragma once

#include <cstdint>

namespace opennow::videodec2_load {

// Estado de la carga del modulo. `api_callable` es LA guardia: ninguna llamada a `sceVideodec2*`
// puede hacerse si vale false.
struct ModuleLoadState {
    int32_t public_load_rc = -1;      // sceSysmoduleLoadModule(VIDEODEC2)
    int32_t internal_load_rc = -1;    // sceSysmoduleLoadModuleInternal(VDECCORE)
    int32_t verify_rc = -1;           // reintento publico tras cargar el nucleo
    bool api_callable = false;        // <- GUARDIA: solo true si algo cargo bien
    const char* path = "none";        // que via funciono
};

// Intenta cargar el modulo del decodificador por las dos vias conocidas.
//
// SEGURIDAD: esta funcion NUNCA llama a la API del decodificador. Solo llama a las funciones de
// carga de sysmodules, que devuelven un codigo de error en vez de crashear. Es, por tanto, seguro
// ejecutarla incluso cuando no sabemos si el modulo existe.
ModuleLoadState LoadVideoDecoderModule();

// Registra el estado en el log. Se separa de la carga para poder registrar el resultado ANTES de
// decidir nada.
void LogModuleLoadState(const ModuleLoadState& state);

}  // namespace opennow::videodec2_load
