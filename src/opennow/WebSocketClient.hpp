#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#ifdef __ORBIS__
extern "C" {
#include "ssl_transport.h"
}
#else
#include <curl/curl.h>
#endif
#include "websocket_write_queue.hpp"

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&)>;

    WebSocketClient(const std::string& url);
    ~WebSocketClient();

    bool connect();
    void disconnect();
    void poll();
    void send_message(const std::string& msg);

    void set_on_message(MessageCallback cb) { on_message_ = cb; }
    void set_custom_headers(const std::vector<std::string>& headers) { custom_headers_ = headers; }
    bool is_connected() const { return connected_; }
    std::string get_last_error() const { return last_error_; }

private:
    std::string url_;
    std::vector<std::string> custom_headers_;
#ifdef __ORBIS__
    NetworkContext_t* tls_ = nullptr;
#else
    CURL* curl_ = nullptr;
#endif
    MessageCallback on_message_;
    bool connected_ = false;
    bool closing_ = false;
    std::string last_error_;
    std::vector<uint8_t> rx_buffer_;
    std::string fragmented_message_;
    uint8_t fragmented_opcode_ = 0;
    opennow::websocket::WriteQueue tx_queue_;

    void close_transport();
    bool has_transport() const {
#ifdef __ORBIS__
        return tls_ != nullptr;
#else
        return curl_ != nullptr;
#endif
    }
    bool drain_outgoing();
    bool send_handshake(const uint8_t* data, size_t length);
    bool send_frame(uint8_t opcode, const uint8_t* payload, size_t length);
};
