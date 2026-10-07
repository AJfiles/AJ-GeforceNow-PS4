// Probe REAL del decodificador por hardware de PS4 usando la API v1 (libSceVideodec).
//
// POR QUE LA V1 Y NO LA V2
// ------------------------
// Medido en consola, en decenas de versiones:
//
//     BOOT_CODEC_MODULE module=VIDEODEC   rc=0x00000000   <- libSceVideodec CARGA
//     BOOT_CODEC_MODULE module=VIDEODEC2  rc=0x805A1000   <- libSceVideodec2 NO
//
// La v2 es la API "moderna" que usan los proyectos de referencia, pero **su sysmodule no esta
// disponible en este firmware** (0x805A1000 = SCE_SYSMODULE_ERROR_UNKNOWN). La v1 si carga.
//
// Y la v1 tiene dos ventajas decisivas segun la propia cabecera del proyecto:
//     "libSceVideodec (v1) loads cleanly, is simpler (no compute-queue step)"
//
// Es decir: **no necesita cola de computo**, que era justo el paso que complicaba la v2. La secuencia
// completa es:
//
//     1. sceVideodecQueryResourceInfo(&cfg, &res)      cuanto necesita
//     2. sceVideodecCreateDecoder(&cfg, &res, &ctrl)   crear el decodificador
//     3. sceVideodecDecode(&ctrl, &in, &fb, &pic)      decodificar
//     4. sceVideodecDeleteDecoder(&ctrl)               liberar
//
// LA MISMA GUARDIA QUE LA v2
// --------------------------
// En la 3.17 llamar a `sceVideodec2*` sin el modulo cargado provoco CE-34878-0 (salto a direccion
// nula por el stub sin resolver). Aqui se aplica la misma regla: **no se toca la API hasta que la
// carga del modulo haya devuelto exito**. La diferencia es que para la v1 la carga SI funciona, asi
// que la guardia deberia dejar pasar.
#pragma once

#include <cstdint>

namespace opennow::videodec1_probe {

// Resultado del intento con la API v1, paso a paso.
struct VideoDecodeV1ProbeResult {
    int32_t load_rc = -1;           // carga del sysmodule (VIA 1)
    int32_t query_resource_rc = -1; // paso 1
    int32_t create_decoder_rc = -1; // paso 2
    int32_t delete_decoder_rc = -1; // limpieza
    bool guard_passed = false;      // la guardia dejo pasar
    bool decoder_created = false;
    bool viable = false;
};

// Ejecuta la secuencia de la v1. Nunca llama a la API si la carga falla.
VideoDecodeV1ProbeResult RunVideoDecodeV1Probe();

}  // namespace opennow::videodec1_probe
