#pragma once

#include "peer.h"
#include "../stream_startup_diagnostics.hpp"

#include <stdexcept>

namespace opennow::webrtc
{

class PeerRuntime
{
  public:
    PeerRuntime()
    {
        WriteStreamStartupStage("peer_runtime_initializing");
        if (peer_init() != 0) {
            WriteStreamStartupStage("peer_runtime_init_failed");
            throw std::runtime_error("Could not initialize the streaming security runtime");
        }
        WriteStreamStartupStage("peer_runtime_ready");
    }

    ~PeerRuntime()
    {
        peer_deinit();
    }

    PeerRuntime(const PeerRuntime&) = delete;
    PeerRuntime& operator=(const PeerRuntime&) = delete;
};

}
