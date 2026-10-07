#include "nvst_sdp.hpp"

#include "../video_quality_policy.hpp"
#include "../stream_startup_diagnostics.hpp"

#include <cstdio>

#include <algorithm>
#include <string>
#include <vector>

namespace opennow::webrtc
{
namespace
{

std::string ExtractSdpValue(const std::string& sdp, const std::string& prefix)
{
    size_t start = 0;
    while (start < sdp.size()) {
        size_t end = sdp.find('\n', start);
        const size_t length = end == std::string::npos ? std::string::npos : end - start;
        std::string line = sdp.substr(start, length);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(prefix, 0) == 0)
            return line.substr(prefix.size());
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return "";
}

} // namespace

std::string BuildNvstSdp(
    const std::string& answer_sdp,
    const StreamSettings& settings,
    const RiInputCapabilities& ri_caps)
{
    constexpr int kOfficialMinimumBitrateKbps = 4000;
      // Capacidad REAL de presentacion de la consola (panel 1080p). Se declara al servidor para que
      // no elija 540p como tamano de referencia. Ver el comentario en la linea del viewport.
      constexpr int kClientViewportWidth = 1920;
      constexpr int kClientViewportHeight = 1080;
    const std::string ice_ufrag = ExtractSdpValue(answer_sdp, "a=ice-ufrag:");
    const std::string ice_pwd = ExtractSdpValue(answer_sdp, "a=ice-pwd:");
    const std::string fingerprint = ExtractSdpValue(answer_sdp, "a=fingerprint:sha-256 ");
    const auto tuning = video::ResolveQualityTuning(settings.image_quality_mode);
    // VENTANA DE BITRATE: endurecida segun el comportamiento medido.
    //
    // POR QUE: declarabamos initialBitrateKbps=12500 (max/4) mientras el servidor entregaba
    // ~5,8 Mbps reales (medido: media de 12.138 bytes por paquete a 60 fps). Esa distancia entre lo
    // DECLARADO y lo ENTREGADO es justo el margen en el que el control de congestion del servidor
    // puede oscilar, y esa oscilacion es lo que dispara la bajada de resolucion.
    //
    // Ahora el techo por defecto es 35 Mbps y el arranque baja a max/4 de ESE valor (8,75 Mbps),
    // mucho mas cerca de la realidad observada. El pico medido fue de 14,3 Mbps, asi que sigue
    // habiendo margen de sobra para escenas complejas.
    constexpr int kDefaultMaxBitrateKbps = 35000;
    const int requested_bitrate =
        settings.bitrate_kbps > 0 ? settings.bitrate_kbps : kDefaultMaxBitrateKbps;
    const int max_bitrate = std::max(kOfficialMinimumBitrateKbps, requested_bitrate);
    const int initial_bitrate = std::max(kOfficialMinimumBitrateKbps, max_bitrate / 4);

    // Registro de los parametros de CALIDAD que se envian al servidor. Sin esto no se puede saber
    // despues que se pidio ni correlacionarlo con la calidad observada en la sesion.
    {
        char q[256];
        std::snprintf(q, sizeof(q),
                      "viewport=%dx%d requested=%dx%d fps=%d max_kbps=%d initial_kbps=%d "
                      "prefilter_mode=1 sharpness=1 denoise=0 scaling_feature=1 quality=%s",
                      kClientViewportWidth, kClientViewportHeight,
                      settings.width, settings.height, settings.fps,
                      max_bitrate, initial_bitrate, settings.image_quality_mode.c_str());
        opennow::LogAppLifecycleEvent("STREAM_QUALITY_PARAMS", q);
    }

    const std::vector<std::string> lines = {
        "v=0",
        "o=SdpTest test_id_13 14 IN IPv4 127.0.0.1",
        "s=-",
        "t=0 0",
        "a=general.icePassword:" + ice_pwd,
        "a=general.iceUserNameFragment:" + ice_ufrag,
        "a=general.dtlsFingerprint:" + fingerprint,
        "m=video 0 RTP/AVP",
        "a=msid:fbc-video-0",
        "a=vqos.fec.rateDropWindow:10",
        "a=vqos.fec.minRequiredFecPackets:2",
        "a=vqos.drc.minRequiredBitrateCheckEnabled:1",
        "a=vqos.fec.repairMinPercent:" + std::to_string(tuning.fec_repair_min_percent),
        "a=vqos.fec.repairPercent:" + std::to_string(tuning.fec_repair_percent),
        "a=vqos.fec.repairMaxPercent:" + std::to_string(tuning.fec_repair_max_percent),
        "a=vqos.dynamicStreamingMode:3",
        "a=vqos.bllFec.enable:0",
        "a=vqos.drc.enable:1",
        "a=video.dx9EnableNv12:1",
        "a=video.dx9EnableHdr:1",
        "a=vqos.qpg.enable:1",
        "a=vqos.resControl.qp.qpg.featureSetting:7",
        "a=bwe.useOwdCongestionControl:1",
        "a=video.enableRtpNack:1",
        "a=vqos.bw.txRxLag.minFeedbackTxDeltaMs:200",
        "a=vqos.drc.bitrateIirFilterFactor:18",
        "a=video.packetSize:1140",
        "a=packetPacing.minNumPacketsPerGroup:" +
            std::to_string(tuning.pacing_min_packets_per_group),
        "a=vqos.adjustStreamingFpsDuringOutOfFocus:1",
        "a=vqos.resControl.cpmRtc.ignoreOutOfFocusWindowState:1",
        "a=vqos.resControl.perfHistory.rtcIgnoreOutOfFocusWindowState:1",
        "a=vqos.resControl.cpmRtc.featureMask:3",
        "a=packetPacing.numGroups:" + std::to_string(tuning.pacing_groups),
        "a=packetPacing.maxDelayUs:" + std::to_string(tuning.pacing_max_delay_us),
        "a=packetPacing.minNumPacketsFrame:10",
        "a=video.rtpNackQueueLength:1024",
        "a=video.rtpNackQueueMaxPackets:512",
        "a=video.rtpNackMaxPacketCount:25",
        // VIEWPORT DECLARADO AL SERVIDOR, a la capacidad REAL de la consola (panel 1080p) y no a la
          // resolucion que pedimos (720p). El servidor elige la resolucion que entrega en funcion
          // de este valor: declarando 720p bajaba a 960x540, y ese 540p es el origen de la
          // pixelacion y de la perdida de nitidez en textos y texturas (el escalado bilineal
          // recupera el tamano, no el detalle que nunca llego). Declarar 1080p, que es lo que la
          // consola presenta de verdad, deja al servidor con 720p como escalon intermedio antes de
          // bajar a 540p. No obliga a enviar 1080p: es solo una declaracion de capacidad.
          "a=video.clientViewportWd:" + std::to_string(kClientViewportWidth),
        "a=video.clientViewportHt:" + std::to_string(kClientViewportHeight),
        "a=video.maxFPS:" + std::to_string(settings.fps),
        "a=video.initialBitrateKbps:" + std::to_string(initial_bitrate),
        "a=video.initialPeakBitrateKbps:" + std::to_string(initial_bitrate),
        "a=vqos.bw.maximumBitrateKbps:" + std::to_string(max_bitrate),
        "a=vqos.bw.minimumBitrateKbps:" + std::to_string(kOfficialMinimumBitrateKbps),
        "a=video.maxNumReferenceFrames:4",
        "a=video.mapRtpTimestampsToFrames:1",
        "a=video.encoderCscMode:3",
        "a=video.encoderHdrCscMode:4",
        "a=video.dynamicRangeMode:0",
        "a=video.bitDepth:8",
        "a=video.scalingFeature1:1",
        // AFILADO EN EL SERVIDOR (prefiltro de NVIDIA).
          //
          // POR QUE: la queja principal es que los textos y las texturas no se ven nitidos. El
          // servidor entrega 960x540 y el escalado a 1280x720 en la PS4 es bilineal: recupera el
          // tamano pero no el detalle perdido. El prefiltro de NVIDIA actua ANTES de codificar,
          // sobre la imagen a la resolucion completa del juego, asi que realza el detalle ALLI
          // donde se pierde, y NO cuesta ni un ciclo de CPU en la consola.
          //
          // Referencia: OpenNOW envia prefilterMode=0 y sharpnessLevel=0 (todo apagado). Aqui se
          // enciende porque el objetivo es la nitidez percibida.
          //
          // denoiseLevel se deja en 0 a proposito: el suavizado de ruido emborrona el detalle fino
          // (textos, texturas), justo lo contrario de lo que buscamos.
          //
          // Si el servidor no reconoce el parametro, lo ignora sin cortar la sesion.
          "a=video.prefilterParams.prefilterMode:1",
        "a=video.prefilterParams.prefilterModel:0",
        "a=video.prefilterParams.denoiseLevel:0",
        "a=video.prefilterParams.sharpnessLevel:1",
        "m=audio 0 RTP/AVP",
        "a=msid:audio",
        "m=mic 0 RTP/AVP",
        "a=msid:mic",
        "a=rtpmap:0 PCMU/8000",
        "m=application 0 RTP/AVP",
        "a=msid:input_1",
        "a=ri.partialReliableThresholdMs:" + std::to_string(ri_caps.partial_reliable_threshold_ms),
        "a=ri.hidDeviceMask:" + std::to_string(ri_caps.hid_device_mask),
        "a=ri.enablePartiallyReliableTransferGamepad:" +
            std::to_string(ri_caps.partial_reliable_gamepad_mask),
        "a=ri.enablePartiallyReliableTransferHid:" +
            std::to_string(ri_caps.partial_reliable_hid_mask),
        "",
    };

    std::string result;
    for (const auto& line : lines) {
        result += line;
        result += "\n";
    }
    return result;
}

} // namespace opennow::webrtc
