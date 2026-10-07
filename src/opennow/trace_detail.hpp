#pragma once

// `uint64_t` en la interfaz: hace falta el include explicito, porque este .hpp no arrastra <cstdint> por
// si mismo y el cliente PS4 lo compila en unidades de traduccion que no lo traen.
#include <cstdint>

// =====================================================================================================
// TRAZA DETALLADA DE ARRANQUE Y DE SESION DE JUEGO (v4.25)
// =====================================================================================================
// QUE ES Y POR QUE EXISTE
// -----------------------
// El log unificado (`diagnostic.log`) es **periodico**: escribe una linea por segundo. Con eso se ve
// "20 fps" o "imagen congelada", pero **no se ve QUE frame fallo ni POR QUE**. Esta serie de versiones
// lo ha demostrado: los fallos que costaron mas caro (la textura recreada 3.573 veces, la presentacion
// encerrada en una guarda) **no se podian ver en un resumen por segundo**; hubo que deducirlos.
//
// Este modulo escribe **una linea por frame** y **un hito por cada paso del arranque y del inicio de
// sesion**, para que la respuesta a "por que no van los 60 fps" salga de los datos y no de una
// deduccion.
//
// DOS SALIDAS, CON PROPOSITOS DISTINTOS
// -------------------------------------
//   `/data/gfnps4/trace_boot.txt`    Hitos del arranque: cada subsistema, con su tiempo y su resultado.
//                                    Sirve para responder "cuanto tarda en estar lista la app".
//
//   `/data/gfnps4/trace_stream.csv`  **Una fila por frame presentado**, y una fila de resumen cada
//                                    segundo. Sirve para responder "donde se va el presupuesto de
//                                    16.666 us", que es LA pregunta de los 60 fps.
//
// LAS COLUMNAS DEL CSV, Y QUE PREGUNTA RESPONDE CADA UNA
// -----------------------------------------------------
//   t_ms              tiempo desde el arranque
//   frame             indice de frame
//   gen               generacion del frame publicado (la que gobierna la puerta de presentacion)
//   reused            1 = la cola estaba vacia; 0 = habia frame nuevo
//   src_w,src_h       lo que entrego el servidor
//   dst_w,dst_h       a lo que se convirtio (el framebuffer)
//   scaled            1 si hubo escalado en CPU  -> si esto es 1, los 60 fps estan en peligro
//   q                 profundidad de la cola del decodificador
//   dec_fps           fps que produce el decodificador
//   pres_us           lo que costo `Present()`  -> el coste a batir
//   budget_us         16.666
//   over              1 si `pres_us` paso del presupuesto
//
// **Con esas columnas, "por que no van a 60" tiene respuesta directa:** se mira cuantas filas tienen
// `over=1`, y si `scaled=1` o `pres_us` alto explica el resto.
//
// COSTE, QUE IMPORTA
// ------------------
// Escribir por frame en un fichero y hacer `fsync` **estropearia la medida**. Por eso:
//   - el CSV se escribe **en un bufer en memoria** y se vuelca a disco **una vez por segundo**;
//   - `FileTrace` escribe con `O_APPEND` y **sin fsync por linea** (solo en los hitos marcados);
//   - el tope de frames guardados es acotado, asi que un stream largo no llena el disco.
//
// COMO LEERLO
// -----------
// El CSV se puede abrir directamente en una hoja de calculo. En el log de texto, cada linea empieza por
// `+<ms>` para poder ordenar por tiempo.

namespace opennow
{
namespace trace
{

// Arranca la traza. Debe llamarse **lo antes posible** en `main`, para que el primer hito sea el
// primero de verdad. Crea/trunca los ficheros.
void StartTrace();

// ---------------------------------------------------------------------------------------------------
// HITOS DE ARRANQUE (`trace_boot.txt`)
// ---------------------------------------------------------------------------------------------------
// Cada llamada escribe una linea con el tiempo transcurrido desde `StartTrace()`, el nombre del paso y
// el detalle. **Pensado para envolver cada paso del arranque**, incluidos los que ya existen: asi se
// ve cual tarda.
void BootStep(const char* step, const char* detail = nullptr);

// Igual que `BootStep`, pero ademas fuerza el volcado a disco (para los hitos que preceden a algo que
// puede colgar).
void BootStepSync(const char* step, const char* detail = nullptr);

// Tiempo desde `StartTrace()` en milisegundos. Util para medir tramos.
long long TraceNowMs();

// ---------------------------------------------------------------------------------------------------
// HITOS DE LA SESION DE JUEGO (`trace_stream.txt` y `trace_stream.csv`)
// ---------------------------------------------------------------------------------------------------
// Texto: un hito de sesion (conexion, traspaso, resolucion, fallo...). Va a `trace_stream.txt`.
void StreamEvent(const char* event, const char* detail = nullptr);

// CSV: **una fila por frame presentado.** Se llama desde el camino de presentacion.
//
// Los parametros son exactamente las columnas descritas arriba. `present_us` puede ser 0 si el frame no
// llego a presentarse; en ese caso `reused` deberia valer 1.
void FrameRow(uint64_t frameIndex, uint64_t generation, int reused,
              int srcW, int srcH, int dstW, int dstH, int scaled,
              int queueDepth, int decodeFps, uint64_t presentUs);

// Resumen por segundo (una fila de texto). Se llama desde el bucle principal.
void StreamSecond(int loopFps, int drawMs, int iterMs, uint64_t framesThisSecond,
                  int queueDepth, int decodeFps, int presentedTotal);

// Vuelca el bufer del CSV a disco. Lo llama `StreamSecond()`; no hace falta llamarlo a mano.
void FlushTrace();

// Cuantas filas de frame lleva acumuladas la traza.
//
// POR QUE EXISTE, en lugar de reutilizar `diag::SessionRecorderCount()`: esa funcion **coge el mismo
// candado que `RecordFrame`**, y se llamaba **una vez por frame** desde el camino de presentacion. No
// habia interbloqueo (el candado se suelta antes de volver), pero **era contencion y una dependencia
// innecesarias en el camino que se esta midiendo**. Este contador es del propio modulo de traza y no
// toca ningun candado ajeno.
uint64_t FrameRowsRecorded();

// Cierra los ficheros (final ordenado de la aplicacion).
void StopTrace();

} // namespace trace
} // namespace opennow
