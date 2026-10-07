// Registrador de sesion de juego — diagnostico granular del stream.
//
// POR QUE EXISTE
// --------------
// Hay dos sintomas sin explicacion confirmada que solo aparecen en sesion real:
//
//   1. La imagen se degrada poco despues de empezar: al entrar se ve nitida y despues aparece
//      pixelacion. Los logs actuales muestran el cambio de resolucion (720p -> 540p) pero no
//      permiten ver CON QUE FRECUENCIA cambia el servidor, ni si hay mas ajustes que no se registran.
//
//   2. Una "actualizacion de imagen" que se repite cada pocos milisegundos durante toda la sesion.
//      Los datos actuales la descartan como cambio de resolucion (solo hubo uno) y como cambio de
//      camino de presentacion (solo dos). Queda por saber si es del codificador del servidor.
//
// POR QUE UN REGISTRADOR Y NO MAS TELEMETRIA SUELTA
// ------------------------------------------------
// La telemetria actual emite UN dato cada N segundos. Eso sirve para tendencias, pero no para
// correlacionar eventos: si la imagen cambia en el frame 1200, no se puede saber que mas paso
// exactamente entonces.
//
// Este registrador guarda una LINEA POR FRAME en un anillo en memoria, y solo la vuelca a disco
// cuando hace falta (a peticion, o al cerrar). Asi:
//   - No penaliza la sesion escribiendo en disco 60 veces por segundo.
//   - Permite ver la SECUENCIA completa alrededor de cualquier evento.
//   - El volcado se puede activar desde la interfaz en el momento en que se nota el problema.
//
// QUE REGISTRA POR FRAME
//   - Numero de frame y marca de tiempo
//   - Resolucion de entrada del decodificador y de salida de presentacion
//   - Camino de presentacion (1:1 o escalado)
//   - Tamano en bytes de la unidad de acceso (indica cuanto esta comprimiendo el servidor)
//   - Si el frame fue IDR/I-frame (un IDR grande sugiere refresco completo)
//   - Tiempo de escalado y de presentacion
//
// ESOS CINCO DATOS JUNTOS SON LOS QUE PERMITEN DISTINGUIR LAS HIPOTESIS:
//   - Si el tamano de AU oscila periodicamente -> es el codificador cambiando la cuantizacion.
//   - Si aparecen IDR cada poco -> el servidor esta refrescando la imagen entera a menudo.
//   - Si el camino de presentacion parpadea -> es el cliente.
//   - Si la resolucion cambia -> es la adaptacion de red.
#pragma once

#include <cstdint>
#include <string>

namespace opennow::diag {

// Activa o desactiva el registro por frame. Desactivado por defecto: no se quiere pagar el coste
// (una escritura en el anillo por frame) en sesiones normales.
void SetSessionRecorderEnabled(bool enabled);
bool IsSessionRecorderEnabled();

// Datos de un frame. Se llama una vez por frame presentado.
struct FrameRecord {
    uint64_t frame_index = 0;
    uint64_t time_ms = 0;
    int src_w = 0;              // resolucion de entrada del decodificador
    int src_h = 0;
    int dst_w = 0;              // resolucion de salida de presentacion
    int dst_h = 0;
    int access_unit_bytes = 0;  // tamano de la unidad de acceso de este frame
    int is_keyframe = 0;        // 1 si fue IDR/I-frame
    uint32_t scale_us = 0;      // tiempo de escalado de este frame
    uint32_t present_us = 0;    // tiempo de presentacion
    int path_scaled = 0;        // 1 si se uso el camino escalado, 0 si fue 1:1
};

void RecordFrame(const FrameRecord& record);

// Registra el PAQUETE COMPRIMIDO que llega al decodificador. A diferencia de RecordFrame (que ve el
// frame decodificado, con tamano constante), aqui SI se ve cuanto comprime el servidor: un IDR
// (keyframe) ocupa mucho mas que un P, y si el tamano de los P oscila, el codificador esta cambiando
// la cuantizacion, que es una de las causas posibles del parpadeo de bloques.
void RecordPacket(int compressed_bytes, int is_keyframe);

// Vuelca el anillo a un fichero. Devuelve el numero de lineas escritas.
// Se llama a peticion del usuario, o automaticamente al terminar la sesion si esta activo.
int DumpSessionRecorder(const std::string& path);

// Limpia el anillo (por ejemplo al empezar una sesion nueva).
void ResetSessionRecorder();

// Numero de frames guardados actualmente en el anillo.
int SessionRecorderCount();

// ---------------------------------------------------------------------------------------------
// RESUMEN DE SESION
//
// Un volcado por frame son miles de lineas. Para el analisis se quiere ademas un resumen compacto
// con la estadistica que importa: cuantas veces cambio la resolucion, cuantas veces cambio el
// tamano de AU de forma brusca, y la distribucion de tamanos de AU.
// ---------------------------------------------------------------------------------------------

// Escribe un resumen estadistico de lo registrado. Es lo primero que hay que mirar.
int DumpSessionSummary(const std::string& path);

// ---------------------------------------------------------------------------------------------
// MARCADOR DE EVENTO
//
// Permite anotar un instante concreto con una etiqueta, para luego buscar en el volcado que paso
// justo entonces. Ejemplo: el usuario nota la "actualizacion de imagen" y pulsa un boton; el
// marcador queda en el registro y se puede ver la secuencia de frames de alrededor.
// ---------------------------------------------------------------------------------------------
void MarkSessionEvent(const char* label);

}  // namespace opennow::diag
