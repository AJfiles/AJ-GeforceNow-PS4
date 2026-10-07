#define _POSIX_C_SOURCE 200809L
#include "AudioPipeline.hpp"
#include "AudioRtpUtils.hpp"
#include "../../stream_startup_diagnostics.hpp"

#include "peer_connection.h"

#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr int kRate=48000;
constexpr int kChannels=2;
constexpr int kOutputFrames=256;
constexpr int kMaxPcmFrames=kRate/2;
uint64_t monotonic_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

struct AudioPipeline::Impl {
    struct Packet {
        std::vector<uint8_t> bytes;
        uint16_t sequence=0;
        uint32_t timestamp=0;
        uint32_t ssrc=0;
        uint8_t payload_type=0;
    };
    std::atomic<bool> running{false};
    std::thread worker;
    mutable std::mutex mutex;
    std::deque<Packet> packets;
    std::deque<int16_t> pcm;
    OpusDecoder* decoder=nullptr;
    int audio_handle=-1;
    std::atomic<int> module_rc{0},audio_init_rc{0},opus_rc{0},audio_open_rc{0},output_rc{0},user_rc{0},audio_user_id{ORBIS_USER_SERVICE_USER_ID_SYSTEM};
    int volume=100;
    int buffer_target_ms=60;
    bool have_expected=false;
    uint16_t expected=0;
    std::atomic<uint64_t> packets_received{0}, packets_decoded{0}, samples_output{0}, packet_drops{0};
    std::atomic<bool> worker_failed{false};
    uint64_t sr_ntp=0;
    uint32_t sr_rtp=0, audio_ssrc=0;
    uint64_t started_at=0;

    bool initialize() {
        module_rc.store(static_cast<int32_t>(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT)));
        audio_init_rc.store(sceAudioOutInit());
        int error=0;
        decoder=opus_decoder_create(kRate,kChannels,&error);
        opus_rc.store(error);
        if(!decoder || error!=OPUS_OK) return false;
        int32_t userId=ORBIS_USER_SERVICE_USER_ID_SYSTEM;
        (void)sceUserServiceInitialize(nullptr);
        user_rc.store(sceUserServiceGetInitialUser(&userId));
        if(user_rc.load()>=0) audio_user_id.store(userId);
        audio_handle=sceAudioOutOpen(audio_user_id.load(),
            ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,0,kOutputFrames,kRate,
            ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        audio_open_rc.store(audio_handle);
        if(audio_handle<0 && audio_user_id.load()!=ORBIS_USER_SERVICE_USER_ID_SYSTEM) {
            audio_handle=sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,
                ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,0,kOutputFrames,kRate,
                ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            audio_open_rc.store(audio_handle);
            if(audio_handle>=0) audio_user_id.store(ORBIS_USER_SERVICE_USER_ID_SYSTEM);
        }
        if(audio_handle<0) { opus_decoder_destroy(decoder); decoder=nullptr; return false; }
        started_at=monotonic_us();
        return true;
    }

    void decode_packet(const Packet& packet, bool lost) {
        std::array<int16_t,5760*kChannels> decoded{};
        const uint8_t* payload=packet.bytes.data();
        size_t payload_size=packet.bytes.size();
        if(!lost) {
            const auto parsed=opennow::audio::ParseRedPrimary(payload,payload_size,packet.payload_type);
            if(!parsed.data || parsed.size==0) { ++packet_drops; return; }
            payload=parsed.data; payload_size=parsed.size;
        }
        const int frames=opus_decode(decoder,lost?nullptr:payload,
            lost?0:static_cast<opus_int32>(payload_size),decoded.data(),5760,0);
        if(frames<0) { ++packet_drops; return; }
        {
            std::lock_guard<std::mutex> lock(mutex);
            const int gain=volume;
            for(int i=0;i<frames*kChannels;i++) {
                const int scaled=(static_cast<int>(decoded[i])*gain)/100;
                pcm.push_back(static_cast<int16_t>(std::max(-32768,std::min(32767,scaled))));
            }
            const size_t max_samples=static_cast<size_t>(kMaxPcmFrames*kChannels);
            while(pcm.size()>max_samples) pcm.pop_front();
        }
        ++packets_decoded;
    }

    void run() {
        try {
        std::array<int16_t,kOutputFrames*kChannels> output{};
        uint64_t last_packet_us=monotonic_us();
        while(running.load(std::memory_order_acquire)) {
            Packet packet;
            bool have_packet=false, lost=false;
            {
                // Orbis libc++ can surface a normal pthread-condvar timeout as
                // std::system_error ("condition_variable timed_wait failed").
                // On a worker thread that can invoke std::terminate. Poll with
                // a short sleep instead; the stream already targets 30/60 Hz.
                std::unique_lock<std::mutex> lock(mutex);
                if(!running.load()) break;
                if(!packets.empty()) {
                    auto it=packets.begin();
                    if(!have_expected) { expected=it->sequence; have_expected=true; }
                    const int16_t distance=static_cast<int16_t>(it->sequence-expected);
                    if(distance<=0 || packets.size()>=4 || monotonic_us()-last_packet_us>40000) {
                        if(distance>0) lost=true;
                        else { packet=std::move(*it); packets.erase(it); }
                        expected=static_cast<uint16_t>(expected+1);
                        have_packet=true;
                        last_packet_us=monotonic_us();
                    }
                }
            }
            if(have_packet) decode_packet(packet,lost);

            bool output_ready=false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(pcm.size()>=output.size()) {
                    for(auto& sample:output) { sample=pcm.front(); pcm.pop_front(); }
                    output_ready=true;
                }
            }
            if(!output_ready) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            const int rc=sceAudioOutOutput(audio_handle,output.data());
            output_rc.store(rc);
            if(rc>=0)
                samples_output+=kOutputFrames;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        } catch (const std::exception& error) {
            worker_failed.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            try { opennow::LogAppLifecycleEvent("AUDIO_WORKER_EXCEPTION", error.what()); }
            catch (...) {}
        } catch (...) {
            worker_failed.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            try { opennow::LogAppLifecycleEvent("AUDIO_WORKER_EXCEPTION", "unknown"); }
            catch (...) {}
        }
    }

    void stop() {
        running.store(false, std::memory_order_release);
        if(worker.joinable()) worker.join();
        if(audio_handle>=0) { sceAudioOutClose(audio_handle); audio_handle=-1; }
        if(decoder) { opus_decoder_destroy(decoder); decoder=nullptr; }
        std::lock_guard<std::mutex> lock(mutex);
        packets.clear(); pcm.clear(); have_expected=false;
    }
};

AudioPipeline::AudioPipeline():impl_(new Impl()){}
AudioPipeline::~AudioPipeline(){stop();}

bool AudioPipeline::start() {
    if(!impl_ || impl_->running.load()) return impl_ && impl_->running.load();
    if(!impl_->initialize()) return false;
    impl_->running.store(true,std::memory_order_release);
    impl_->worker=std::thread(&Impl::run,impl_.get());
    return true;
}

void AudioPipeline::configure(int volume_percent,int target_buffer_ms) {
    if(!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->volume=std::max(0,std::min(200,volume_percent));
    impl_->buffer_target_ms=std::max(20,std::min(250,target_buffer_ms));
}

void AudioPipeline::stop(){if(impl_) impl_->stop();}

void AudioPipeline::submit(const PeerAudioPacket& packet) {
    if(!impl_ || !impl_->running.load() || !packet.data || packet.size==0) return;
    Impl::Packet item;
    item.bytes.assign(packet.data,packet.data+packet.size);
    item.sequence=packet.sequence; item.timestamp=packet.timestamp;
    item.ssrc=packet.ssrc; item.payload_type=packet.payload_type;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->packets_received;
        if(impl_->packets.size()>=128) { impl_->packets.pop_front(); ++impl_->packet_drops; }
        impl_->packets.push_back(std::move(item));
        impl_->audio_ssrc=packet.ssrc;
    }
}

void AudioPipeline::set_sender_report(uint32_t ssrc,uint64_t ntp_us,uint32_t rtp_timestamp) {
    if(!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->audio_ssrc=ssrc; impl_->sr_ntp=ntp_us; impl_->sr_rtp=rtp_timestamp;
}

std::string AudioPipeline::debug_info() const {
    if(!impl_) return "PS4 audio unavailable";
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return "PS4 SceAudioOut/Opus rx="+std::to_string(impl_->packets_received.load())+
        " decoded="+std::to_string(impl_->packets_decoded.load())+
        " samples="+std::to_string(impl_->samples_output.load())+
        " drops="+std::to_string(impl_->packet_drops.load())+
        " queue="+std::to_string(impl_->packets.size())+
        " module_rc="+std::to_string(impl_->module_rc.load())+
        " init_rc="+std::to_string(impl_->audio_init_rc.load())+
        " opus_rc="+std::to_string(impl_->opus_rc.load())+
        " user_rc="+std::to_string(impl_->user_rc.load())+
        " user="+std::to_string(impl_->audio_user_id.load())+
        " open_rc="+std::to_string(impl_->audio_open_rc.load())+
        " output_rc="+std::to_string(impl_->output_rc.load())+
        " worker_failed="+std::to_string(impl_->worker_failed.load() ? 1 : 0);
}

int64_t AudioPipeline::playback_ntp_us() const {
    if(!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->sr_ntp==0) return 0;
    const uint64_t elapsed=impl_->samples_output.load()*1000000ULL/kRate;
    return static_cast<int64_t>(impl_->sr_ntp+elapsed);
}
