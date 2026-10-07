#pragma once

#include <cstdint>
#include <cstddef>

namespace opennow::color {

// Flags for store modes
constexpr int STORE_UNALIGNED = 0;
constexpr int STORE_ALIGNED   = 1;
constexpr int STORE_STREAMING = 2; // Non-temporal store (_mm_stream_si128)

// Converts YUV420P (3 planar buffers) to BGRA32 using BT.709 matrix.
// full_range: true = 0-255 (AVCOL_RANGE_JPEG), false = 16-235 (AVCOL_RANGE_MPEG)
// y_plane: width x height
// u_plane: (width/2) x (height/2)
// v_plane: (width/2) x (height/2)
// dst_bgra: output buffer in BGRA format (4 bytes per pixel)
void ConvertYUV420PToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* u_plane, int u_pitch,
    const uint8_t* v_plane, int v_pitch,
    int store_mode = STORE_ALIGNED,
    bool full_range = false
);

// Converts NV12 (Y plane + interleaved UV plane) to BGRA32 using BT.709 matrix.
// y_plane: width x height
// uv_plane: interleaved UV, (width/2) x (height/2) pairs (stride typically >= width)
// dst_bgra: output buffer in BGRA format (4 bytes per pixel)
void ConvertNV12ToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    int store_mode = STORE_ALIGNED,
    bool full_range = false
);

// Reference scalar implementation for testing and unaligned tails
void ConvertYUV420PToBGRA_BT709_Scalar(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* u_plane, int u_pitch,
    const uint8_t* v_plane, int v_pitch,
    bool full_range = false
);

void ConvertNV12ToBGRA_BT709_Scalar(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    bool full_range = false
);

// Bilinear scaling + BT.709 conversion to target dimensions (e.g. 960x540 -> 1280x720)
// Nivel de realce de nitidez del escalado, en PORCENTAJE sobre el realce base (0 = desactivado).
//
// POR QUE ES AJUSTABLE: el escalado bilineal suaviza la imagen cuando el servidor entrega 540p y la
// salida es 720p. Un realce devuelve parte del contraste local, pero el nivel ideal depende del
// juego y del gusto: demasiado genera halos en los bordes, y muy poco deja la imagen blanda. En vez
// de fijar un valor arbitrario, se expone.
//
// Es un ajuste GLOBAL y no un parametro por llamada porque el escalado se reparte entre varios
// hilos del pool, y pasar el valor por parametro obligaria a tocar las cuatro funciones de escalado
// y sus llamadas. Se lee una vez por frame.
void SetLumaSharpenPercent(int percent);
void ScaleBilinearYUV420PToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int dst_w, int dst_h,
    int src_w, int src_h,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* u_plane, int u_pitch,
    const uint8_t* v_plane, int v_pitch,
    bool full_range = false
);

void ScaleBilinearNV12ToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int dst_w, int dst_h,
    int src_w, int src_h,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    bool full_range = false
);

} // namespace opennow::color
