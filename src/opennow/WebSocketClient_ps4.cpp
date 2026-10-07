#define _POSIX_C_SOURCE 200809L
#include "WebSocketClient.hpp"
#include "websocket_handshake.hpp"
#include "ps4_secure_random.h"
#include "stream_startup_diagnostics.hpp"

#include "config.h"
extern "C" {
#include "ports.h"
#include "socket.h"
}

#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <sys/select.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
constexpr size_t kMaximumHandshakeBytes = 64 * 1024;
constexpr size_t kMaximumSignalingPayloadBytes = 4 * 1024 * 1024;
constexpr size_t kMaximumPollReadBytes = 64 * 1024;
constexpr size_t kMaximumFramesPerPoll = 16;

bool ensure_ca_bundle(std::string& error) {
    static std::string ca_bundle;
    if (!ca_bundle.empty()) return true;
    std::ifstream file("/app0/assets/misc/mozilla-ca-bundle.pem", std::ios::binary);
    if (!file) {
        error = "Trusted CA bundle missing at /app0/assets/misc/mozilla-ca-bundle.pem";
        opennow::LogAppLifecycleEvent("TLS_CA_BUNDLE_FAILED", "reason=file_missing");
        return false;
    }
    ca_bundle.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (ca_bundle.empty() || ca_bundle.size() > 1024 * 1024) {
        ca_bundle.clear(); error = "Trusted CA bundle is empty or exceeds 1 MiB";
        opennow::LogAppLifecycleEvent("TLS_CA_BUNDLE_FAILED", "reason=size_invalid");
        return false;
    }
    if (ssl_transport_set_ca_bundle(ca_bundle.c_str()) != 0) {
        ca_bundle.clear(); error = "Could not configure TLS trust roots";
        opennow::LogAppLifecycleEvent("TLS_CA_BUNDLE_FAILED", "reason=configure_failed");
        return false;
    }
    opennow::LogAppLifecycleEvent("TLS_CA_BUNDLE_READY", "source=packaged_mozilla_bundle");
    return true;
}

std::vector<uint8_t> secure_random(size_t size) {
    std::vector<uint8_t> bytes(size);
    const int random_rc = ps4_secure_random(bytes.data(), size);
    if (random_rc != 0)
        throw std::runtime_error("PS4 libSceRandom secure entropy source failed (Orbis code " +
                                 std::to_string(random_rc) + ")");
    return bytes;
}

std::string base64(const uint8_t* data, size_t size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    uint32_t value = 0;
    int bits = -6;
    for (size_t i = 0; i < size; ++i) {
        value = (value << 8) | data[i];
        bits += 8;
        while (bits >= 0) {
            result.push_back(alphabet[(value >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) result.push_back(alphabet[((value << 8) >> (bits + 8)) & 0x3f]);
    while ((result.size() & 3) != 0) result.push_back('=');
    return result;
}

int tls_send(void* context, const unsigned char* data, size_t size) {
    auto* socket = static_cast<TcpSocket*>(context);
    return tcp_socket_send(socket, data, static_cast<int>(size));
}

int tls_recv_nowait(void* context, unsigned char* data, size_t size, uint32_t) {
    auto* socket = static_cast<TcpSocket*>(context);
    fd_set read_fds;
    timeval timeout{};
    FD_ZERO(&read_fds);
    FD_SET(socket->fd, &read_fds);
    const int ready = select(socket->fd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (ready < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return tcp_socket_recv(socket, data, static_cast<int>(size));
}

std::string tls_error(int error) {
    char buffer[160]{};
    mbedtls_strerror(error, buffer, sizeof(buffer));
    return buffer;
}
}

WebSocketClient::WebSocketClient(const std::string& url) : url_(url) {}
WebSocketClient::~WebSocketClient() { disconnect(); close_transport(); }

bool WebSocketClient::connect() {
    disconnect();
    close_transport();
    last_error_.clear();
    if (url_.rfind("wss://", 0) != 0) {
        last_error_ = "PS4 signaling only accepts wss://";
        return false;
    }

    const size_t authority_start = 6;
    const size_t path_start = url_.find_first_of("/?#", authority_start);
    std::string authority = url_.substr(authority_start,
        path_start == std::string::npos ? std::string::npos : path_start - authority_start);
    std::string path = path_start == std::string::npos ? "/" : url_.substr(path_start);
    if (path.empty() || path[0] == '?' || path[0] == '#') path.insert(path.begin(), '/');
    if (path.find('#') != std::string::npos || authority.empty() || authority.find('@') != std::string::npos) {
        last_error_ = "Invalid or unsupported WebSocket URL";
        return false;
    }

    std::string hostname = authority;
    uint16_t port = 443;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        try {
            const unsigned long parsed = std::stoul(authority.substr(colon + 1));
            if (parsed == 0 || parsed > 65535) throw std::out_of_range("port");
            port = static_cast<uint16_t>(parsed);
            hostname = authority.substr(0, colon);
        } catch (...) {
            last_error_ = "Invalid WebSocket port";
            return false;
        }
    }
    if (hostname.empty()) { last_error_ = "WebSocket host is empty"; return false; }
    if (!ensure_ca_bundle(last_error_)) return false;

    tls_ = static_cast<NetworkContext_t*>(std::calloc(1, sizeof(NetworkContext_t)));
    if (!tls_) { last_error_ = "Could not allocate TLS context"; return false; }
    if (ssl_transport_connect(tls_, hostname.c_str(), port, nullptr) != 0) {
        const char* transport_error = ssl_transport_last_error();
        last_error_ = (transport_error && transport_error[0])
            ? std::string("Signaling transport failed: ") + transport_error
            : "TLS handshake or CA verification failed";
        close_transport();
        return false;
    }

    // Keep the UI/event loop responsive after the TLS handshake.
    mbedtls_ssl_conf_read_timeout(&tls_->conf, 1);
    mbedtls_ssl_set_bio(&tls_->ssl, &tls_->tcp_socket, tls_send, nullptr, tls_recv_nowait);

    try {
        const std::vector<uint8_t> nonce = secure_random(16);
        const std::string key = base64(nonce.data(), nonce.size());
        const auto expected = opennow::websocket::AcceptForKey(key);
        if (!expected) throw std::runtime_error("Could not calculate WebSocket accept hash");
        std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + authority +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
            "\r\nSec-WebSocket-Version: 13\r\n";
        for (const auto& header : custom_headers_) request += header + "\r\n";
        request += "\r\n";
        if (!send_handshake(reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
            close_transport();
            return false;
        }

        std::string response;
        uint8_t buffer[2048];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            const int count = ssl_transport_recv(tls_, buffer, sizeof(buffer));
            if (count > 0) {
                response.append(reinterpret_cast<const char*>(buffer), static_cast<size_t>(count));
                if (response.size() > kMaximumHandshakeBytes) throw std::runtime_error("WebSocket handshake response too large");
                if (response.find("\r\n\r\n") == std::string::npos) continue;
                const size_t end = response.find("\r\n\r\n") + 4;
                if (!opennow::websocket::ValidateUpgrade(response, *expected)) {
                    const size_t status_end=response.find("\r\n");
                    std::string status_line=status_end==std::string::npos
                        ? std::string("status line missing") : response.substr(0,status_end);
                    if(status_line.size()>120) status_line.resize(120);
                    const bool server_rejected=status_line.rfind("HTTP/1.1 101",0)!=0 &&
                                                status_line.rfind("HTTP/1.0 101",0)!=0;
                    if (server_rejected && status_line.find("404") != std::string::npos &&
                        path != "/nvst/") {
                        opennow::LogAppLifecycleEvent("WEBSOCKET_404_NVST_RETRY",
                            ("host=" + hostname + " old_path=" + path).c_str());
                        url_ = "wss://" + authority + "/nvst/";
                        close_transport();
                        return connect();
                    }
                    const std::string detail=server_rejected
                        ? "Server rejected WebSocket upgrade: " + status_line
                        : "Server returned invalid WebSocket upgrade headers";
                    opennow::LogAppLifecycleEvent(server_rejected?"WEBSOCKET_UPGRADE_REJECTED":"WEBSOCKET_UPGRADE_INVALID",status_line.c_str());
                    throw std::runtime_error(detail);
                }
                if (response.size() > end)
                    rx_buffer_.insert(rx_buffer_.end(), response.begin() + end, response.end());
                connected_ = true;
                return true;
            }
            if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_TIMEOUT) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            throw std::runtime_error("WebSocket handshake read failed: " + tls_error(count));
        }
        throw std::runtime_error("WebSocket upgrade timed out");
    } catch (const std::exception& error) {
        last_error_ = error.what();
        close_transport();
        return false;
    }
}

void WebSocketClient::disconnect() {
    if (connected_) {
        const uint8_t close_payload[] = {0x03, 0xe8};
        (void)send_frame(0x08, close_payload, sizeof(close_payload));
        connected_ = false;
        closing_ = true;
    }
    if (closing_) (void)drain_outgoing();
}

void WebSocketClient::close_transport() {
    connected_ = false;
    closing_ = false;
    tx_queue_.reset();
    rx_buffer_.clear();
    fragmented_message_.clear();
    fragmented_opcode_ = 0;
    if (tls_) {
        ssl_transport_disconnect(tls_);
        std::free(tls_);
        tls_ = nullptr;
    }
}

bool WebSocketClient::send_handshake(const uint8_t* data, size_t length) {
    size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + opennow::websocket::WriteQueue::SendTimeout;
    while (sent < length && std::chrono::steady_clock::now() < deadline) {
        const int count = ssl_transport_send(tls_, data + sent, length - sent);
        if (count > 0) sent += static_cast<size_t>(count);
        else if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else { last_error_ = "TLS handshake write failed: " + tls_error(count); return false; }
    }
    if (sent != length) { last_error_ = "WebSocket handshake send timed out"; return false; }
    return true;
}

bool WebSocketClient::send_frame(uint8_t opcode, const uint8_t* payload, size_t length) {
    if (!connected_ || !tls_) return false;
    const size_t header = length < 126 ? 6 : length <= 0xffff ? 8 : 14;
    if (length > kMaximumSignalingPayloadBytes || !tx_queue_.can_enqueue(length + header)) {
        last_error_ = "Outgoing WebSocket queue is full";
        close_transport();
        return false;
    }
    std::vector<uint8_t> frame;
    frame.reserve(length + header);
    frame.push_back(opcode | 0x80);
    if (length < 126) frame.push_back(static_cast<uint8_t>(0x80 | length));
    else if (length <= 0xffff) {
        frame.push_back(0xfe); frame.push_back(static_cast<uint8_t>(length >> 8)); frame.push_back(static_cast<uint8_t>(length));
    } else {
        frame.push_back(0xff);
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>(static_cast<uint64_t>(length) >> (i * 8)));
    }
    try {
        const std::vector<uint8_t> mask = secure_random(4);
        frame.insert(frame.end(), mask.begin(), mask.end());
        for (size_t i = 0; i < length; ++i) frame.push_back(payload[i] ^ mask[i & 3]);
    } catch (const std::exception& error) { last_error_ = error.what(); close_transport(); return false; }
    return tx_queue_.enqueue(std::move(frame), opennow::websocket::WriteQueue::Clock::now());
}

bool WebSocketClient::drain_outgoing() {
    using Queue = opennow::websocket::WriteQueue;
    const auto result = tx_queue_.drain([&](const uint8_t* data, size_t size) {
        const int count = ssl_transport_send(tls_, data, size);
        if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE)
            return Queue::WriteResult{Queue::WriteStatus::Again, 0};
        if (count < 0) return Queue::WriteResult{Queue::WriteStatus::Error, 0};
        return Queue::WriteResult{Queue::WriteStatus::Progress, static_cast<size_t>(count)};
    }, [] { return Queue::Clock::now(); });
    if (result == Queue::DrainResult::Complete || result == Queue::DrainResult::Pending) {
        if (closing_ && result == Queue::DrainResult::Complete) close_transport();
        return true;
    }
    last_error_ = result == Queue::DrainResult::TimedOut ? "WebSocket send timed out" : "WebSocket send failed";
    close_transport();
    return false;
}

void WebSocketClient::send_message(const std::string& message) {
    (void)send_frame(0x01, reinterpret_cast<const uint8_t*>(message.data()), message.size());
}

void WebSocketClient::poll() {
    if ((!connected_ && !closing_) || !tls_) return;
    if (closing_) { (void)drain_outgoing(); return; }
    size_t processed = 0;
    while (rx_buffer_.size() >= 2 && processed < kMaximumFramesPerPoll) {
        const uint8_t opcode = rx_buffer_[0] & 0x0f;
        const bool final = (rx_buffer_[0] & 0x80) != 0;
        const bool control = (opcode & 0x08) != 0;
        const uint8_t len7 = rx_buffer_[1] & 0x7f;
        if ((rx_buffer_[0] & 0x70) || (rx_buffer_[1] & 0x80) ||
            (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10) ||
            (control && (!final || len7 > 125)) || (!control && ((opcode == 0) != (fragmented_opcode_ != 0)))) {
            last_error_ = "Invalid incoming WebSocket frame"; close_transport(); return;
        }
        size_t header = 2, length = len7;
        if (len7 == 126) { header += 2; if (rx_buffer_.size() < header) break; length = (rx_buffer_[2] << 8) | rx_buffer_[3]; }
        else if (len7 == 127) {
            header += 8; if (rx_buffer_.size() < header) break; uint64_t n = 0;
            for (size_t i=0;i<8;i++) n=(n<<8)|rx_buffer_[2+i];
            if (n > kMaximumSignalingPayloadBytes) { last_error_="Incoming WebSocket frame too large"; close_transport(); return; }
            length=static_cast<size_t>(n);
        }
        if (length > kMaximumSignalingPayloadBytes || (!control && length > kMaximumSignalingPayloadBytes-fragmented_message_.size())) {
            last_error_="Incoming WebSocket frame too large"; close_transport(); return;
        }
        if (rx_buffer_.size() < header + length) break;
        std::string payload(reinterpret_cast<const char*>(rx_buffer_.data()+header),length);
        rx_buffer_.erase(rx_buffer_.begin(),rx_buffer_.begin()+header+length); ++processed;
        if (!control) {
            if (opcode) fragmented_opcode_=opcode;
            fragmented_message_+=payload;
            if (final) {
                const bool text=fragmented_opcode_==1;
                std::string message=std::move(fragmented_message_); fragmented_message_.clear(); fragmented_opcode_=0;
                if (text && on_message_) on_message_(message);
            }
        } else if (opcode==8) {
            (void)send_frame(8,reinterpret_cast<const uint8_t*>(payload.data()),payload.size());
            connected_=false; closing_=true;
        } else if (opcode==9) {
            (void)send_frame(10,reinterpret_cast<const uint8_t*>(payload.data()),payload.size());
        }
        if (!connected_) break;
    }
    if (!drain_outgoing() || !connected_ || processed>=kMaximumFramesPerPoll) return;
    uint8_t buffer[4096]; size_t read=0;
    while (read < kMaximumPollReadBytes) {
        const int count=ssl_transport_recv(tls_,buffer,sizeof(buffer));
        if (count==MBEDTLS_ERR_SSL_WANT_READ || count==MBEDTLS_ERR_SSL_TIMEOUT) break;
        if (count==0) { last_error_="Remote endpoint closed signaling"; close_transport(); return; }
        if (count<0) { last_error_="WebSocket TLS read failed: "+tls_error(count); close_transport(); return; }
        if (rx_buffer_.size() > kMaximumSignalingPayloadBytes+14-static_cast<size_t>(count)) {
            last_error_="Incoming WebSocket buffer too large"; close_transport(); return;
        }
        rx_buffer_.insert(rx_buffer_.end(),buffer,buffer+count); read+=static_cast<size_t>(count);
    }
}
