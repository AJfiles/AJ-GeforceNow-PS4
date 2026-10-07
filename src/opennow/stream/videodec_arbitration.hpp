// Declaraciones de libSceVideoDecoderArbitration.
//
// POR QUE ESTE FICHERO EXISTE
// ---------------------------
// El SDK de OpenOrbis trae el stub `libSceVideoDecoderArbitration.so` con cuatro simbolos
// exportados, pero **ninguna cabecera los declara**. Es el mismo caso que `libSceVideodec2`, cuya
// cabecera oficial (`orbis/Videodec2.h`) declara las funciones como `void nombre();` sin firmas
// utiles. Aqui se declaran con firmas deducidas de la semantica del nombre y del uso observado.
//
// POR QUE HACE FALTA
// ------------------
// YouTube para PS4 carga `libSceVideoDecoderArbitration` JUNTO a `libSceVideodec2`. En PS4 el
// decodificador de video es un RECURSO COMPARTIDO entre la aplicacion y el sistema (grabacion,
// transmision, captura), asi que el sistema exige que la aplicacion lo solicite por el modulo de
// arbitracion antes de usarlo.
//
// Nuestro cliente intentaba cargar `VIDEODEC2` directamente (rc=0x805A1000) sin pasar por
// arbitracion. Ese es el paso que faltaba en la secuencia.
//
// SECUENCIA COMPLETA (segun la evidencia de YouTube + el orden de los simbolos del SDK):
//
//   1. sceVideoDecoderArbitrationInitialize()      <- pedir el recurso al sistema
//   2. sceVideoDecoderArbitrationEnable()          <- habilitar el uso
//   3. sceVideodec2QueryComputeMemoryInfo()        <- cuanto necesita la COLA
//   4. sceVideodec2AllocateComputeQueue()          <- crear la COLA
//   5. sceVideodec2QueryDecoderMemoryInfo()        <- AHORA si: cuanto necesita el DECODIFICADOR
//   6. sceVideodec2CreateDecoder(compute_queue)    <- crear el decodificador
//   7. sceVideodec2Decode()                        <- decodificar
//   8. sceVideoDecoderArbitrationAcceptEvent()     <- (eventos del sistema durante la sesion)
//
// RIESGO Y HONESTIDAD
// -------------------
// Las FIRMAS de arbitracion estan deducidas, no confirmadas contra firmware. Por eso cada llamada
// se hace con el minimo de argumentos (vacio) y el resultado se REGISTRA antes de decidir si
// continuar. Si una firma fuera incorrecta, el peor caso es un codigo de error, no corrupcion de
// memoria: no se le pasa ningun puntero a estructura.
//
// El modulo NO CARGA con el ID de sysmodule conocido (0x805A1000), asi que el primer intento real
// debe ser comprobar si al enlazar la libreria y llamarla se resuelve. Si tampoco, el problema esta
// en la carga del modulo y no en la ABI.
#pragma once

#include <cstdint>

namespace opennow::videodec_arb {

using s32 = int32_t;
using u32 = uint32_t;

extern "C" {
// Inicializa el subsistema de arbitracion del decodificador de video.
// Devuelve 0 (SCE_OK) si el recurso queda disponible para la aplicacion.
s32 sceVideoDecoderArbitrationInitialize();

// Habilita el uso del decodificador por parte de la aplicacion.
s32 sceVideoDecoderArbitrationEnable();

// Habilita el modo de suspension: la aplicacion acepta que el sistema le retire el decodificador
// temporalmente (por ejemplo si el usuario abre la grabacion de video del sistema).
s32 sceVideoDecoderArbitrationEnableSuspendMode(s32 enable);

// Procesa un evento del sistema relacionado con el decodificador.
// `event` es un puntero opaco; se deja como void* a proposito porque su estructura no esta
// confirmada y no hace falta interpretarla para el arranque.
s32 sceVideoDecoderArbitrationAcceptEvent(void* event);
}

// Resultado del intento de habilitar la arbitracion. Se registra en el log para que quede claro
// que paso en cada intento, en vez de fallar en silencio.
struct ArbitrationResult {
    s32 init_rc = -1;
    s32 enable_rc = -1;
    s32 suspend_rc = -1;
    bool usable = false;
};

// Intenta habilitar la arbitracion del decodificador. No lanza ni asume exito: devuelve los codigos
// de cada llamada para que el llamante decida.
ArbitrationResult EnableVideoDecoderArbitration();

}  // namespace opennow::videodec_arb
