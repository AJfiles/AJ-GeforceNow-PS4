#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace opennow
{

struct HttpTransferControl
{
    const std::atomic_bool* cancelled = nullptr;
    std::size_t max_body_bytes = 0;
    long timeout_ms = 15000;
};

struct HttpResponse
{
    long status_code = 0;
    std::string body;
};

class HttpClient
{
  public:
    HttpResponse Request(
        const std::string& method,
        const std::string& url,
        const std::string& user_agent,
        const std::vector<std::string>& headers = {},
        const std::string& body = {},
        const std::string& proxy_url = {},
        HttpTransferControl control = {}) const;

    HttpResponse Get(
        const std::string& url,
        const std::string& user_agent,
        const std::vector<std::string>& headers = {},
        const std::string& proxy_url = {},
        HttpTransferControl control = {}) const;

    HttpResponse Post(
        const std::string& url,
        const std::string& user_agent,
        const std::vector<std::string>& headers,
        const std::string& body,
        const std::string& proxy_url = {},
        HttpTransferControl control = {}) const;

    int MeasureConnectLatencyMs(
        const std::string& url,
        long timeout_ms = 3000) const noexcept;
};

// Releases the persistent Net/SSL/HTTP contexts created on first request.
// Must be called before sceNetTerm(): teardown runs in reverse creation order
// (HTTP, then SSL, then the network pool). Safe to call more than once and safe
// when nothing was ever initialised. After it returns, the next request would
// lazily recreate the pool, so only call it during shutdown.
void ShutdownHttpClient();

} // namespace opennow
