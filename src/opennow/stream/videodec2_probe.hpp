// Probe REAL del decodificador por hardware de PS4 (libSceVideodec2, API v2).
//
// POR QUE EXISTE
// --------------
// Hasta ahora NUNCA se habia llamado a las funciones de la v2: solo se probaba `codecType` contra la
// API v1 (`libSceVideodec`), que sigue un esquema distinto (un solo campo de codec) y por eso
// devolvia 0x80C10001 con todos los valores. La v2 usa una estructura de configuracion completa con
// una COLA DE COMPUTO, que es lo que realmente selecciona el decodificador por hardware.
//
// La ABI de la v2 esta verificada: los `static_assert` de `videodec2_abi.hpp` (medidos en PS4)
// coinciden EXACTAMENTE con los de `prosper` (PS5) para las cinco estructuras:
//     DecoderConfigInfo 0x48   DecoderMemoryInfo 0x48   InputData 0x30
//     OutputInfo        0x38   FrameBuffer       0x20
// Eso descarta que el problema sea un tamano equivocado (el error 0x80C1000D nunca aparece).
//
// LA SECUENCIA (documentada por prosper, que reproduce el uso real de un titulo)
// -----------------------------------------------------------------------------
//     1. sceVideodec2QueryComputeMemoryInfo()     cuanto necesita la COLA
//     2. sceVideodec2AllocateComputeQueue()       crear la COLA
//     3. sceVideodec2QueryDecoderMemoryInfo()     AHORA si: cuanto necesita el DECODIFICADOR
//     4. sceVideodec2CreateDecoder()              crear el decodificador con la cola
//
// El paso 2 es el que faltaba: sin cola, `compute_queue` va a 0 y el decodificador no tiene sobre
// que construirse. prosper documenta su propio fallo con esas palabras:
//     "all 7 rejected on compute_queue = 0"
//
// SEGURIDAD
// ---------
// Este probe NO se llama durante el arranque (la leccion de la 3.11: una llamada de sistema con
// firmas no verificadas dejo la aplicacion sin arrancar). Se ejecuta desde una accion explicita del
// usuario, y cada paso registra su resultado ANTES de continuar, de modo que si algo no regresa ya
// se sabe en que paso fue.
//
// Las FIRMAS de `sceVideodec2*` SI estan verificadas (coinciden con prosper); las que no lo estaban
// eran las de `libSceVideoDecoderArbitration`, que es otro modulo.
#pragma once

#include <cstdint>

namespace opennow::videodec2_probe {

// Resultado completo del intento, para poder volcarlo al log como una sola linea legible.
struct HardwareDecodeProbeResult {
    int32_t compute_memory_rc = -1;   // paso 1
    int32_t compute_config_rc = -1;   // paso 1b (config de la cola)
    int32_t allocate_queue_rc = -1;   // paso 2
    int32_t decoder_memory_rc = -1;   // paso 3
    int32_t create_decoder_rc = -1;   // paso 4
    bool decoder_created = false;
    bool viable = false;
    uint64_t compute_queue_handle = 0;
};

// Ejecuta la secuencia completa y devuelve los codigos de cada paso.
// No lanza: si un paso falla, los siguientes se omiten y queda registrado donde se detuvo.
HardwareDecodeProbeResult RunHardwareDecodeProbe();

}  // namespace opennow::videodec2_probe
