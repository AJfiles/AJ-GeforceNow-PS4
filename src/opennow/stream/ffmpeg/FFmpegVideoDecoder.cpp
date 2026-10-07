#include "FFmpegVideoDecoder.hpp"
#include "AVFrameHolder.hpp"
#include "../../session_recorder.hpp"
#ifdef __ORBIS__
#include "../../ps4_logger.hpp"
#else
#include "borealis.hpp"
#endif
#include "../../stream_settings.hpp"
#include "../../stream_startup_diagnostics.hpp"
#include "../../video_quality_policy.hpp"
#include "../DecodeQueuePolicy.hpp"
#include <cstdio>
#include <chrono>
#include <new>
#include <vector>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/cpu.h>   // av_get_cpu_flags(): diagnostico SIMD (v3.80)
}
#ifdef PLATFORM_APPLE
extern "C" {
#include <libavcodec/videotoolbox.h>
}
#endif

// Disables the deblocking filter at the cost of image quality
#define DISABLE_LOOP_FILTER 0x1
// Uses the low latency decode flag (disables multithreading)
#define LOW_LATENCY_DECODE 0x2

#if defined(PLATFORM_ANDROID)
#include <jni.h>
#include <libavcodec/jni.h>
#include <libavutil/hwcontext_mediacodec.h>

//static JavaVM *mJavaVM = NULL;
//JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
//{
////    av_jni_set_java_vm(vm, NULL);
//    return JNI_VERSION_1_4;
//}
#endif

FFmpegVideoDecoder::FFmpegVideoDecoder() {
//    AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
//    AVHWDeviceContext* ctx = (AVHWDeviceContext*)deviceRef->data;
//    AVMediaCodecDeviceContext* hwctx = (AVMediaCodecDeviceContext*)ctx->hwctx;
////    hwctx->surface = ;
//    av_hwdevice_ctx_init(deviceRef);
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;

void ffmpegLog(void* ptr, int level, const char* fmt, va_list vargs) {
    (void)ptr;
    (void)level;
    va_list ap_copy;
    va_copy(ap_copy, vargs);
    const int length = std::vsnprintf(nullptr, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (length <= 0)
        return;

    std::vector<char> buffer(static_cast<size_t>(length) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), fmt, vargs);
    brls::Logger::debug("FFmpeg [LOG]: {}", buffer.data());
}

int FFmpegVideoDecoder::setup(int video_format, int width, int height,
                              int redraw_rate, void* context, int dr_flags) {
    opennow::WriteStreamStartupStage("ffmpeg_setup_entered");
    (void)context;
    m_stream_fps = redraw_rate;
    m_uses_hardware_frames =
        (dr_flags & VIDEO_DECODER_PREFER_HARDWARE) != 0 &&
        (dr_flags & VIDEO_DECODER_FORCE_SOFTWARE) == 0;

    std::string format;
    switch (video_format) {
        case VIDEO_FORMAT_H264:
            format = "H264";
            break;
        case VIDEO_FORMAT_H265:
            format = "HEVC";
            break;
        case VIDEO_FORMAT_H265_MAIN10:
            format = "HEVC HDR";
            break;
        case VIDEO_FORMAT_AV1_MAIN8:
            format = "AV1";
            break;
        case VIDEO_FORMAT_AV1_MAIN10:
            format = "AV1 HDR";
            break;
        default:
            format = "UNKNOWN";
            break;
    }
    brls::Logger::debug("FFMpeg's AVCodec version: {}.{}.{}", AV_VERSION_MAJOR(avcodec_version()), AV_VERSION_MINOR(avcodec_version()), AV_VERSION_MICRO(avcodec_version()));
    brls::Logger::info(
        "FFmpeg: Setup with format: {}, width: {}, height: {}, fps: {}", format, width, height, redraw_rate);

    av_log_set_level(AV_LOG_WARNING);
    // av_log_set_callback(&ffmpegLog); // Uncomment to see FFMpeg logs
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
    avcodec_register_all();
#endif

    m_packet = av_packet_alloc();
    if (!m_packet) {
        brls::Logger::error("FFmpeg: Couldn't allocate packet");
        return AVERROR(ENOMEM);
    }
    opennow::WriteStreamStartupStage("ffmpeg_packet_allocated");

    // LOW_LATENCY_DECODE APAGADO A PROPOSITO (medido en una sesion de 2 h).
    //
    // QUE HACE: anade AV_CODEC_FLAG_LOW_DELAY, que impide que el decodificador retenga frames de
    // referencia para reordenar. El problema es que **el paralelismo de FF_THREAD_FRAME se basa en
    // tener varios frames en vuelo a la vez**: con low_delay el decodificador no puede solapar el
    // trabajo de varios frames y el rendimiento se hunde.
    //
    // MEDIDO en la sesion de 2 h con low_delay activo:
    //     decode P95 = 50.000 us, maximo = 89.046 us
    //     FPS de entrada/decoder = 30,3 / 30,2   (la mitad de 60)
    //     frameQueue dropTiming = 32.940 frames descartados
    //
    // Es decir: el decodificador iba a 30 FPS porque cada frame le costaba hasta 89 ms, y eso es
    // coherente con un decodificador serializado.
    //
    // El coste de apagarlo es unos pocos frames de latencia de reordenacion (el stream usa un
    // GOP corto), y a cambio el frame threading puede repartir el trabajo. El presupuesto a 60 FPS
    // es de 16.666 us por frame, asi que pasar de 50.000 a menos de 16.000 es exactamente lo que
    // hace falta para que el servidor deje de bajar el bitrate.
    int perf_lvl = 0;
// =================================================================================================
// ACTIVADO TAMBIEN EN PS4 (v3.60) — ver la explicacion larga en el bloque de `skip_loop_filter`
// =================================================================================================
// En la PS4 **no hay decodificador por hardware** (las dos APIs fallan: `viable=0`), asi que el
// decodificador por software marca el techo del sistema (~30 FPS medidos por el propio proyecto en
// `FFmpegVideoDecoder.cpp:201`). El filtro de desbloqueo es una de las partes mas caras de decodificar
// H.264 en software, y el codificador de NVIDIA **ya ha filtrado el stream antes de enviarlo**.
//
// Se salta solo en fotogramas DESECHABLES (los que no sirven de referencia), asi que los fotogramas
// que sostienen la cadena conservan el filtro completo.
#if defined(PLATFORM_SWITCH)
    // H.264 deblocking is one of the largest software decode costs on Tegra X1.
    // The cloud stream is already filtered by the encoder, so trading a small
    // amount of edge quality for stable frame time is preferable to frame loss.
    if (!m_uses_hardware_frames)
        perf_lvl |= DISABLE_LOOP_FILTER;
#endif
#if defined(__ORBIS__)
    // Mismo razonamiento, y aqui ademas es la unica palanca que toca al que marca el techo.
    if (!m_uses_hardware_frames)
        perf_lvl |= DISABLE_LOOP_FILTER;
#endif

#ifdef PLATFORM_ANDROID
    if (video_format & VIDEO_FORMAT_MASK_H264) {
        m_decoder = avcodec_find_decoder_by_name("h264_mediacodec");
    } else if (video_format & VIDEO_FORMAT_MASK_H265) {
        m_decoder = avcodec_find_decoder_by_name("hevc_mediacodec");
    } else {
        // Unsupported decoder type
    }
#else
    if (video_format & VIDEO_FORMAT_MASK_H264) {
#if defined(OPENNOW_ENABLE_NVDEC)
        m_decoder = m_uses_hardware_frames
            ? avcodec_find_decoder_by_name("h264_nvtegra")
            : avcodec_find_decoder(AV_CODEC_ID_H264);
#else
        m_uses_hardware_frames = false;
        m_decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
#endif
    } else if (video_format & VIDEO_FORMAT_MASK_H265) {
        m_decoder = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    } else {
        // Unsupported decoder type
    }
#endif

    if (m_decoder == nullptr) {
        brls::Logger::error("FFmpeg: Couldn't find decoder");
        return -1;
    }
    opennow::WriteStreamStartupStage("ffmpeg_decoder_found");

    m_decoder_context = avcodec_alloc_context3(m_decoder);
    if (m_decoder_context == nullptr) {
        brls::Logger::error("FFmpeg: Couldn't allocate context");
        return -1;
    }
    opennow::WriteStreamStartupStage("ffmpeg_context_allocated");

    // =============================================================================================
    // DESBLOQUEO EN FOTOGRAMAS DESECHABLES — ACTIVADO EN PS4 (v3.60)
    // =============================================================================================
    // QUE HACE: `skip_loop_filter = AVDISCARD_NONREF` le dice al decodificador que **no aplique el
    // filtro de desbloqueo (deblocking) en los fotogramas que no sirven de referencia** (B no
    // referenciados). Esos fotogramas no los usa nadie para predecir, asi que el filtro solo mejora
    // su aspecto, no la cadena.
    //
    // POR QUE AHORA: en la PS4 **NO HAY DECODIFICADOR POR HARDWARE** — las dos APIs fallan:
    //
    //     HWVDEC_PROBE_RESULT  viable=0 aborted=module_not_loaded fallback=software
    //     HWVDEC1_PROBE_RESULT viable=0 stopped_at=1_query_resource_info
    //
    // Y el decodificador por software es **el techo del sistema**. El propio proyecto lo tiene medido
    // (`FFmpegVideoDecoder.cpp:201`): *"El decodificador por software en 3 hilos no daba abasto
    // (30 FPS medidos)"*. **No hay arreglo de la interfaz que lleve a 60 FPS si el decodificador no
    // pasa de ~30.**
    //
    // El filtro de desbloqueo es **una de las partes mas caras** de decodificar H.264 en software: es
    // un filtro adaptativo por borde, con varias lecturas y escrituras por pixel, sobre TODA la
    // imagen. Y el propio proyecto ya lo documenta para la Switch:
    //
    //     "H.264 deblocking is one of the largest software decode costs on Tegra X1. The cloud stream
    //      is already filtered by the encoder, so trading a small amount of edge quality for stable
    //     frame time is preferable to frame loss."
    //
    // **El argumento de la calidad vale igual en PS4**: el codificador de NVIDIA ya ha filtrado el
    // stream antes de enviarlo, asi que el desbloqueo del decodificador es en gran parte redundante.
    //
    // QUE SE PIERDE: algo de suavidad en los bordes de los bloques **en los fotogramas desechables**,
    // que son los que menos se miran (escenas de mucho movimiento). Los fotogramas de REFERENCIA
    // conservan el filtro completo, asi que la calidad base de la imagen no cambia.
    //
    // MODOS: se respeta el ajuste de calidad del usuario.
    //   - "Original"  -> se salta el filtro en TODOS los fotogramas desechables (es el modo que pide
    //                    maxima fluidez y no aplica post-proceso).
    //   - "Clarity" y "Adaptive" -> igual, pero ademas estos modos ya aplican realce de nitidez por
    //                    sombreado, asi que la perdida de suavidad se compensa.
    //
    // MEDICION: el resultado se registra en `DECODE_PERF` (una linea por segundo) con el tiempo medio
    // de decodificacion, para poder comparar con los 30 FPS medidos antes de este cambio.
    if (perf_lvl & DISABLE_LOOP_FILTER) {
        const auto settings = opennow::LoadStreamSettings();
        const auto tuning = opennow::video::ResolveQualityTuning(settings.image_quality_mode);
        // Keep deblocking on reference pictures in enhanced modes. This removes
        // persistent block edges while still skipping work on disposable frames.
        m_decoder_context->skip_loop_filter = tuning.preserve_reference_deblocking
            ? AVDISCARD_NONREF : AVDISCARD_ALL;
        opennow::LogAppLifecycleEvent("DECODER_LOOP_FILTER_POLICY",
            (std::string("skip_loop_filter=") +
             (m_decoder_context->skip_loop_filter == AVDISCARD_ALL ? "ALL" : "NONREF") +
             " modo=" + settings.image_quality_mode +
             " motivo=cpu_sin_decoder_hardware").c_str());
    }

    if (perf_lvl & LOW_LATENCY_DECODE)
        // Use low delay single threaded encoding
        m_decoder_context->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Do not present corrupt or pre-keyframe pictures. RTP recovery now keeps
    // complete access units, so these permissive flags only expose reference
    // damage as visible macroblocks instead of holding the previous good frame.
    m_decoder_context->flags2 |= AV_CODEC_FLAG2_FAST;

    // =============================================================================================
    // OCULTACION DE ERRORES (v3.63) — CONTRA LOS MACROBLOQUES
    // =============================================================================================
    // SINTOMA REPORTADO: *"la imagen sufre de macrobloques horribles (artifacts) justo cuando GFN baja
    // la resolucion"*.
    //
    // POR QUE PASA: `AV_CODEC_FLAG2_FAST` (la linea de arriba) hace que el decodificador **no
    // compruebe si el flujo es valido** y **no oculte los errores**: cuando falta un macrobloque o un
    // bloque de referencia, en lugar de reconstruirlo de forma aproximada **lo deja como esta**, que es
    // lo que se ve como cuadros de basura. La bandera es intencionada (ahorra CPU, y el comentario de
    // arriba explica que se prefiere ver el dano antes que congelar el frame), pero el resultado es que
    // **no hay ninguna ocultacion**.
    //
    // `error_concealment` es la palanca que SI reconstruye: con `FF_EC_GUESS_MVS` estima los vectores de
    // movimiento que faltan a partir de los bloques vecinos, y con `FF_EC_DEBLOCK` suaviza los bordes
    // entre bloques reconstruidos y buenos. `FF_EC_FAVOR_INTER` hace que prefiera la estimacion entre
    // fotogramas (lo correcto para video, no para imagen fija).
    //
    // SON BANDERAS, no un modo: se pueden activar sin renunciar a `FAST`. El coste es bajo porque solo
    // trabaja en los bloques que fallan, no en la imagen entera.
    //
    // Los valores estan en `libavcodec/avcodec.h` de este FFmpeg:
    //     FF_EC_GUESS_MVS    = 1
    //     FF_EC_DEBLOCK      = 2
    //     FF_EC_FAVOR_INTER  = 256
    m_decoder_context->error_concealment =
        FF_EC_GUESS_MVS | FF_EC_DEBLOCK | FF_EC_FAVOR_INTER;
    opennow::LogAppLifecycleEvent("DECODER_ERROR_CONCEALMENT",
        ("error_concealment=" + std::to_string(m_decoder_context->error_concealment) +
         " flags=guess_mvs|deblock|favor_inter motivo=macrobloques_al_bajar_resolucion").c_str());

    // =============================================================================================
    // DIAGNOSTICO DE SIMD DEL DECODIFICADOR (v3.80). ES EL DATO QUE DECIDE SI EL TECHO SE PUEDE SUBIR.
    // =============================================================================================
    // HALLAZGO QUE MOTIVA ESTA LINEA: la configuracion con la que se construyo este FFmpeg tiene
    //
    //     FFMPEG_CONFIGURATION=... --arch=x86_64 ...     (pidio x86_64)
    //     ARCH=c                                         (**acabo en C**)
    //     HAVE_X86ASM 0   HAVE_SSE2 0   HAVE_SSE4 0   HAVE_AVX2 0   HAVE_MMX 0
    //
    // Es decir: **el decodificador H.264 de la PS4 puede estar corriendo en C puro**, sin las rutas
    // SIMD que FFmpeg tiene para compensacion de movimiento, IDCT, prediccion y qpel. La libreria de
    // PS4 pesa 2,3 MB y la de Switch (con NEON) 17,1 MB, lo que es coherente con esa sospecha.
    //
    // **Y si eso es cierto, es el techo de los 30 FPS**: no es que la Jaguar no pueda, es que se le
    // esta pidiendo decodificar con el codigo mas lento que existe.
    //
    // ESTA LINEA DISTINGUE LAS DOS POSIBILIDADES, que tienen arreglos totalmente distintos:
    //
    //   (a) `cpu=0` o solo flags basicos  -> **el BUILD no tiene las rutas**: hay que reconstruir
    //       FFmpeg con ensamblador (necesita `nasm`, que hoy NO esta en el entorno).
    //   (b) `cpu` con SSE2/SSE4/AVX2      -> las rutas EXISTEN y se estan detectando; entonces el
    //       techo es otro y hay que buscar en el reparto de hilos o en el bitrate.
    //
    // Se registran tambien los flags que el BUILD declara soportar (`av_get_cpu_flags` devuelve lo
    // detectado en tiempo de ejecucion, que no puede incluir nada que no se compilara).
    {
        const int cpuFlags = av_get_cpu_flags();
        char sd[224];
        std::snprintf(sd, sizeof(sd),
                      "cpu_flags=0x%08X sse2=%d sse4=%d avx2=%d mmx=%d avx=%d "
                      "nota=si_todo_es_0_el_build_es_C_puro",
                      (unsigned)cpuFlags,
                      (cpuFlags & AV_CPU_FLAG_SSE2) ? 1 : 0,
                      (cpuFlags & AV_CPU_FLAG_SSE4) ? 1 : 0,
                      (cpuFlags & AV_CPU_FLAG_AVX2) ? 1 : 0,
                      (cpuFlags & AV_CPU_FLAG_MMX) ? 1 : 0,
                      (cpuFlags & AV_CPU_FLAG_AVX) ? 1 : 0);
        opennow::LogAppLifecycleEvent("DECODER_SIMD", sd);
    }

    // NVDEC submits asynchronously and does not benefit from CPU frame
    // threads. The software fallback keeps one Tegra X1 core for UI/network.
    // La PS4 (Jaguar) tiene 8 nucleos. El decodificador por software en 3 hilos no daba abasto
    // (30 FPS medidos). Con 5 se deja margen al hilo de red y al de presentacion.
    const int decoder_threads = m_uses_hardware_frames ? 1 : 5;
    m_decoder_context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    m_decoder_context->thread_count = decoder_threads;

    m_decoder_context->width = width;
    m_decoder_context->height = height;

#if defined(PLATFORM_SWITCH) && defined(BOREALIS_USE_DEKO3D)
        AVHWDeviceType hwType = m_uses_hardware_frames
            ? AV_HWDEVICE_TYPE_NVTEGRA : AV_HWDEVICE_TYPE_NONE;
#elif defined(PLATFORM_ANDROID)
        AVHWDeviceType hwType = AV_HWDEVICE_TYPE_MEDIACODEC;
#elif defined(PLATFORM_APPLE)
        AVHWDeviceType hwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
        AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;
#endif

    int err = 0;
    if (hwType != AV_HWDEVICE_TYPE_NONE) {
        if ((err = av_hwdevice_ctx_create(&hw_device_ctx, hwType, nullptr, nullptr, 0)) < 0) {
            char error[512];
            av_strerror(err, error, sizeof(error));
            brls::Logger::error("FFmpeg: Error initializing hardware decoder - {}", error);
            return -1;
        }
        m_decoder_context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    } else {
        brls::Logger::info("FFmpeg: using software frames for OpenGL renderer");
    }

#if defined(OPENNOW_ENABLE_NVDEC)
    if (m_uses_hardware_frames)
        m_decoder_context->pix_fmt = AV_PIX_FMT_NVTEGRA;
#endif

    opennow::WriteStreamStartupStage("ffmpeg_avcodec_open_begin");
    err = avcodec_open2(m_decoder_context, m_decoder, nullptr);
    if (err < 0) {
        char error[512];
        av_strerror(err, error, sizeof(error));
        brls::Logger::error("FFmpeg: Couldn't open codec - {}", error);
        return err;
    }
    opennow::WriteStreamStartupStage("ffmpeg_avcodec_open_complete");

    // Cola amplia: absorbe la rafaga de catch-up del arranque del stream. Con 4 se perdian 231
    // frames en los primeros 470 ms, que es el congelon de 1-3 s al entrar en la partida.
    AVFrameHolder::instance().prepare(m_uses_hardware_frames ? 8 : 12);

    // Telemetria de la configuracion EFECTIVA del decodificador. Sin esto no se puede saber si
    // FFmpeg acepto los hilos y el tipo de threading que se pidieron, ni si low_delay sigue activo:
    // era imposible diagnosticar por que el decoder iba a 30 FPS.
    {
        char cfg[256];
        std::snprintf(cfg, sizeof(cfg),
                      "threads=%d thread_type=%d low_delay=%d fast=%d skip_loop_filter=%d "
                      "flags=0x%08X",
                      m_decoder_context->thread_count,
                      m_decoder_context->thread_type,
                      (m_decoder_context->flags & AV_CODEC_FLAG_LOW_DELAY) ? 1 : 0,
                      (m_decoder_context->flags2 & AV_CODEC_FLAG2_FAST) ? 1 : 0,
                      static_cast<int>(m_decoder_context->skip_loop_filter),
                      static_cast<unsigned>(m_decoder_context->flags));
        opennow::LogAppLifecycleEvent("DECODER_CONFIG", cfg);
    }

    // One extra frame for decoder output while the frame holder owns deep copies.
    m_frames_size = 5 + 1;
    m_frames = new (std::nothrow) AVFrame*[m_frames_size] {};
    if (!m_frames) {
        brls::Logger::error("FFmpeg: Couldn't allocate frame slots");
        return AVERROR(ENOMEM);
    }

    tmp_frame = av_frame_alloc();
    if (!tmp_frame) {
        brls::Logger::error("FFmpeg: Couldn't allocate temp frame");
        return -1;
    }

    for (int i = 0; i < m_frames_size; i++) {
        auto& frame = m_frames[i];
        frame = av_frame_alloc();
        if (frame == nullptr) {
            brls::Logger::error("FFmpeg: Couldn't allocate frame");
            return -1;
        }
    }

    opennow::WriteStreamStartupStage("ffmpeg_frame_pool_ready");
    brls::Logger::info("FFmpeg: Setup done with {} frame threads", decoder_threads);
    return 0;
}

void FFmpegVideoDecoder::cleanup() {
    brls::Logger::info("FFmpeg: Cleanup...");

    av_packet_free(&m_packet);

    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }

    if (m_decoder_context) {
        avcodec_free_context(&m_decoder_context);
    }

    if (m_frames) {
        for (int i = 0; i < m_frames_size; i++) {
            av_frame_free(&m_frames[i]);
        }

        delete[] m_frames;
        m_frames = nullptr;
        m_frames_size = 0;
    }

    if (tmp_frame) {
        av_frame_free(&tmp_frame);
    }

    AVFrameHolder::instance().cleanup();

    brls::Logger::info("FFmpeg: Cleanup done!");
}

int FFmpegVideoDecoder::submit_decode_unit(uint8_t* indata, int inlen, int64_t pts) {
    if (!indata || inlen <= 0)
        return 0;

    if (static_cast<size_t>(inlen) > opennow::video::MaximumDecodeUnitBytes()) {
        brls::Logger::error("FFmpeg: access unit exceeds 2 MiB limit");
        return -1;
    }

    // =============================================================================================
    // MEDICION DEL DECODIFICADOR (v3.60)
    // =============================================================================================
    // POR QUE SE MIDE: el decodificador por software **es el techo del sistema** en PS4 (no hay
    // decodificador por hardware: las dos APIs dan `viable=0`). El proyecto tiene medidos **30 FPS**
    // con 3 hilos, y 5 hilos es lo que hay ahora.
    //
    // Se acaba de activar `skip_loop_filter` en fotogramas desechables, que deberia **ahorrar CPU al
    // decodificador**. Esta marca dice cuanto, con un numero en el log (`DECODE_PERF`), en lugar de
    // dejarlo en "deberia ir mejor".
    //
    // **Esta es la unica palanca que toca al que marca el techo**, asi que su efecto es el que mas
    // importa de todo el trabajo de rendimiento.
    const uint64_t t_decode0 = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    m_frames_in++;

    int decoded_frames = 0;
    bool corrupt_frame_dropped = false;
    auto drain_frames = [&]() {
        int decode_error = 0;
        while ((m_frame = get_frame(true, decode_error)) != nullptr) {
            if ((m_frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 ||
                m_frame->decode_error_flags != 0) {
                m_corrupt_frames_dropped++;
                corrupt_frame_dropped = true;
                if (m_corrupt_frames_dropped <= 5 || m_corrupt_frames_dropped % 60 == 0) {
                    brls::Logger::warning(
                        "FFmpeg: dropping corrupt frame flags={} decodeErrors={} total={}",
                        m_frame->flags, m_frame->decode_error_flags,
                        m_corrupt_frames_dropped);
                }
                av_frame_unref(m_frame);
                continue;
            }
            m_frames_out++;
            decoded_frames++;
            AVFrameHolder::instance().push(m_frame);
        }
        return decode_error;
    };

    int decode_result = decode(reinterpret_cast<char*>(indata), inlen, pts);
    if (decode_result == AVERROR(EAGAIN)) {
        const int drain_result = drain_frames();
        if (drain_result < 0)
            return drain_result;
        decode_result = decode(reinterpret_cast<char*>(indata), inlen, pts);
    }

    if (decode_result < 0)
        return decode_result;

    const int drain_result = drain_frames();
    if (drain_result < 0)
        return drain_result;

    if (decoded_frames == 0 && corrupt_frame_dropped)
        return AVERROR_INVALIDDATA;

    // Informe del decodificador: tiempo medio por unidad de acceso, una linea por segundo.
    // Es el numero que dice si `skip_loop_filter` ha servido de algo.
    {
        const uint64_t t_decode1 = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        static uint64_t s_decodeAccumUs = 0;
        static uint64_t s_decodeUnits  = 0;
        static uint64_t s_decodeLastLog = 0;
        s_decodeAccumUs += (t_decode1 > t_decode0) ? (t_decode1 - t_decode0) : 0;
        ++s_decodeUnits;
        if (s_decodeLastLog == 0 || (t_decode1 - s_decodeLastLog) >= 1000000ULL) {
            s_decodeLastLog = t_decode1;
            const uint64_t n = s_decodeUnits ? s_decodeUnits : 1;
            const uint64_t avg = s_decodeAccumUs / n;
            char dp[224];
            std::snprintf(dp, sizeof(dp),
                          "decode_us=%llu units=%llu fps_decoder=%.1f frames_out=%llu "
                          "corrupt_dropped=%llu tipo=%d hilos=%d",
                          (unsigned long long)avg, (unsigned long long)s_decodeUnits,
                          avg ? (1000000.0 / (double)avg) : 0.0,
                          (unsigned long long)m_frames_out,
                          (unsigned long long)m_corrupt_frames_dropped,
                          (int)m_decoder_context->skip_loop_filter,
                          (int)m_decoder_context->thread_count);
            opennow::LogAppLifecycleEvent("DECODE_PERF", dp);
            s_decodeAccumUs = 0;
            s_decodeUnits = 0;
        }
    }

    return decoded_frames;
}

void FFmpegVideoDecoder::reset_stream() {
    if (m_decoder_context)
        avcodec_flush_buffers(m_decoder_context);
}

int FFmpegVideoDecoder::capabilities() const {
    return 0;
}

int FFmpegVideoDecoder::decode(char* indata, int inlen, int64_t pts) {
    m_packet->data = (uint8_t*)indata;
    m_packet->size = inlen;
    m_packet->pts = pts;
    m_packet->dts = pts;

#if !defined(PLATFORM_SWITCH)
    int policy;
    sched_param params{};
    pthread_getschedparam(pthread_self(), &policy, &params);
    params.sched_priority = sched_get_priority_max(policy);
    pthread_setschedparam(pthread_self(), policy, &params);
#endif

//    m_decoder_context->skip_frame = AVDISCARD_ALL;

    // Registrar el PAQUETE COMPRIMIDO: su tamano y si es keyframe. Es el dato que permite
    // ver si el codificador del servidor esta pulsando la cuantizacion (causa posible del parpadeo
    // de bloques) o si los keyframes son demasiado frecuentes.
    opennow::diag::RecordPacket(inlen, (m_packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0);

    int err = avcodec_send_packet(m_decoder_context, m_packet);
    if (err == AVERROR(EAGAIN)) {
        brls::Logger::debug("FFmpeg: decoder wants frame drain before accepting more data");
        return err;
    }

    if (err != 0) {
        char error[512];
        av_strerror(err, error, sizeof(error));
        brls::Logger::error("FFmpeg: Decode failed - {}", error);
        return err;
    }

    return 0;
}

AVFrame* FFmpegVideoDecoder::get_frame(bool native_frame, int& decode_error) {
    decode_error = 0;
    if (!m_decoder_context || !m_frames || m_frames_size <= 0)
        return nullptr;

    int err;
    AVFrame* resultFrame = m_frames[m_next_frame];
    AVFrame* decodeFrame = resultFrame;
    av_frame_unref(resultFrame);

#if !defined(PLATFORM_ANDROID) && !defined(BOREALIS_USE_DEKO3D) && !defined(USE_METAL_RENDERER)
    if (hw_device_ctx) {
        // For HW->SW transfer path we decode into a temporary hardware frame.
        av_frame_unref(tmp_frame);
        decodeFrame = tmp_frame;
    }
#endif

    if ((err = avcodec_receive_frame(m_decoder_context, decodeFrame)) < 0) {
        if (err == AVERROR(EAGAIN)) {
            return nullptr;
        }

        if (err == AVERROR_EOF) {
            return nullptr;
        }

        char a[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        brls::Logger::error("FFmpeg: Error receiving frame with error {}",  av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, err));
        decode_error = err;
        return nullptr;
    }

    if (hw_device_ctx) {
#if defined(BOREALIS_USE_DEKO3D) || defined(PLATFORM_ANDROID) || defined(USE_METAL_RENDERER)
        // Keep hardware-backed frame references per queue slot.
        resultFrame = decodeFrame;
#else

#if defined(PLATFORM_SWITCH) && !defined(BOREALIS_USE_DEKO3D)
        for (int i = 0; i < 2; ++i) {
            if (((uintptr_t)resultFrame->data[i] & 0xff) || (resultFrame->linesize[i] & 0xff)) {
                brls::Logger::error("Frame address/pitch not aligned to 256, falling back to cpu transfer");
                break;
            }
        }
#endif

        // Copy hardware frame into software frame
        av_frame_unref(resultFrame);
        if ((err = av_hwframe_transfer_data(resultFrame, decodeFrame, 0)) < 0) {
            char a[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            brls::Logger::error("FFmpeg: Error transferring the data to system memory with error {}",  av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, err));
            decode_error = err;
            return nullptr;
        }

        av_frame_copy_props(resultFrame, decodeFrame);
#endif
    } else {
        resultFrame = decodeFrame;
    }

    m_current_frame = m_next_frame;
    m_next_frame = (m_current_frame + 1) % m_frames_size;
    if (native_frame)
        return resultFrame;

    return nullptr;
}

VideoDecodeStats* FFmpegVideoDecoder::video_decode_stats() {
    return (VideoDecodeStats*)&m_video_decode_stats_cache;
}
