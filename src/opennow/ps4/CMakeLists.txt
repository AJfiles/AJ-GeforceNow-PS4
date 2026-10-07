cmake_minimum_required(VERSION 3.20)
project(gfn_client_ps4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED PS4_PROJECT_ROOT)
    message(FATAL_ERROR "PS4_PROJECT_ROOT must point to the app repository root")
endif()
file(TO_CMAKE_PATH "${PS4_PROJECT_ROOT}" PS4_PROJECT_ROOT)
set(OPENNOW_DIR "${PS4_PROJECT_ROOT}/src/opennow")
set(NATIVE_DEPS "${PS4_PROJECT_ROOT}/build/libpeer-ps4/dist")

add_library(gfn_client_ps4 STATIC
    "${OPENNOW_DIR}/gfn/authentication.cpp"
    "${OPENNOW_DIR}/gfn/catalog.cpp"
    "${OPENNOW_DIR}/gfn/cloud_session.cpp"
    "${OPENNOW_DIR}/gfn/cloud_session_protocol.cpp"
    "${OPENNOW_DIR}/gfn/cloud_session_state.cpp"
    "${OPENNOW_DIR}/gfn/community_proxy.cpp"
    "${OPENNOW_DIR}/gfn/persistence.cpp"
    "${OPENNOW_DIR}/gfn/regions.cpp"
    "${OPENNOW_DIR}/gfn/shared.cpp"
    "${OPENNOW_DIR}/ps4_secure_random.cpp"
    "${OPENNOW_DIR}/stream_settings.cpp"
    "${OPENNOW_DIR}/localization.cpp"
    "${OPENNOW_DIR}/app_paths.cpp"
    "${OPENNOW_DIR}/play_history.cpp"
    "${OPENNOW_DIR}/stream_diagnostics.cpp"
    "${OPENNOW_DIR}/stream_startup_diagnostics.cpp"
    # TRAZA DETALLADA (v4.25): un hito por paso del arranque y una fila por frame presentado. Existe
    # porque el log periodico no deja ver QUE frame falla ni POR QUE; ver src/opennow/trace_detail.hpp.
    "${OPENNOW_DIR}/trace_detail.cpp"
    # Registrador de sesion: diagnostico granular por frame. La telemetria periodica no permite
    # correlacionar eventos (si la imagen cambia en el frame 1200, no se sabe que mas paso entonces).
    # Ver src/opennow/session_recorder.hpp.
    "${OPENNOW_DIR}/session_recorder.cpp"
    # Arbitracion del decodificador de video: el paso que YouTube hace antes de usar
    # libSceVideodec2 y que a nosotros nos faltaba.
    "${OPENNOW_DIR}/stream/videodec_arbitration.cpp"
    # Probe REAL de la API v2 del decodificador por hardware (secuencia con cola de computo).
    # Nunca se habia llamado a estas funciones; solo se probaba la v1 con `codecType`.
    "${OPENNOW_DIR}/stream/videodec2_probe.cpp"
    # Carga del modulo del decodificador CON GUARDIA. Sin esto, llamar a la API sin modulo cargado
    # provoca CE-34878-0 (salto a direccion nula por el stub sin resolver).
    "${OPENNOW_DIR}/stream/videodec2_load.cpp"
    # Probe de la API v1: su sysmodule SI carga (VIDEODEC rc=0x00000000) y no necesita cola de
    # computo, a diferencia de la v2 cuyo modulo no esta disponible (0x805A1000).
    "${OPENNOW_DIR}/stream/videodec1_probe.cpp"
    "${OPENNOW_DIR}/mbedtls_timing_compat.c"
    "${OPENNOW_DIR}/ps4_http_client.cpp"
    "${OPENNOW_DIR}/WebSocketClient_ps4.cpp"
    "${OPENNOW_DIR}/signaling_client.cpp"
    "${OPENNOW_DIR}/stream/audio/AudioPipeline_ps4.cpp"
    "${OPENNOW_DIR}/stream/ffmpeg/FFmpegVideoDecoder.cpp"
    "${OPENNOW_DIR}/stream/ffmpeg/AVFrameHolder.cpp"
    "${OPENNOW_DIR}/stream/SDLVideoRenderer.cpp"
    "${OPENNOW_DIR}/stream/PS4PigletVideoRenderer.cpp"
    "${OPENNOW_DIR}/stream/color_simd.cpp"
    "${OPENNOW_DIR}/stream/PS4VideoOutRenderer.cpp"
    "${OPENNOW_DIR}/webrtc/session.cpp"
    "${OPENNOW_DIR}/webrtc/shared.cpp"
    "${OPENNOW_DIR}/webrtc/nvst_sdp.cpp"
    "${OPENNOW_DIR}/webrtc/diagnostic_writer.cpp"
    "${OPENNOW_DIR}/webrtc/negotiation.cpp"
    "${OPENNOW_DIR}/webrtc/input.cpp"
    "${OPENNOW_DIR}/webrtc/media.cpp"
    "${OPENNOW_DIR}/webrtc/diagnostics.cpp"
)
set_source_files_properties("${OPENNOW_DIR}/stream/PS4PigletVideoRenderer.cpp"
    PROPERTIES COMPILE_DEFINITIONS "__PIGLET__=1")

target_include_directories(gfn_client_ps4 PUBLIC
    "${OPENNOW_DIR}"
    "${OPENNOW_DIR}/gfn"
    "${OPENNOW_DIR}/stream"
    "${PS4_PROJECT_ROOT}/src/third_party/jansson/src"
    "${PS4_PROJECT_ROOT}/src/third_party/libpeer-orbis/src"
    "${PS4_PROJECT_ROOT}/src/third_party/libpeer-orbis/third_party/coreHTTP/source/interface"
    "${NATIVE_DEPS}/include"
)
target_compile_definitions(gfn_client_ps4 PRIVATE __ORBIS__=1)
