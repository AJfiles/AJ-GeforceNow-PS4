#include "http_client.hpp"
#include "gfn_client.hpp"
#include "stream_startup_diagnostics.hpp"

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace opennow
{
namespace
{
std::mutex& HttpMutex()
{
    static std::mutex mutex;
    return mutex;
}

void CheckOrbis(int32_t result, const char* operation)
{
    if (result < 0)
        throw std::runtime_error(std::string(operation) + " failed (Orbis " +
                                 std::to_string(result) + ")");
}

// Persistent Net, SSL, and HTTP context across the application lifetime
static int32_t s_persistent_pool = -1;
static int32_t s_persistent_ssl = -1;
static int32_t s_persistent_http = -1;

void EnsureHttpInitialized(const std::function<void(const char*, long, std::size_t)>& log_stage)
{
    if (s_persistent_http >= 0) {
        LogAppLifecycleEvent("HTTP_POOL_REUSED", "status=reused");
        return;
    }

    log_stage("load_net_module", 0, 0);
    int rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    if (rc < 0 && rc != static_cast<int>(0x80960003)) CheckOrbis(rc, "load SceNet");

    log_stage("load_ssl_module", 0, 0);
    rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    if (rc < 0 && rc != static_cast<int>(0x80960003)) CheckOrbis(rc, "load SceSsl");

    log_stage("load_http_module", 0, 0);
    rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    if (rc < 0 && rc != static_cast<int>(0x80960003)) CheckOrbis(rc, "load SceHttp");

    log_stage("create_net_pool", 0, 0);
    s_persistent_pool = sceNetPoolCreate("aj-gfn-http", 64 * 1024, 0);
    CheckOrbis(s_persistent_pool, "sceNetPoolCreate");

    log_stage("init_ssl", 0, 0);
    s_persistent_ssl = sceSslInit(512 * 1024);
    CheckOrbis(s_persistent_ssl, "sceSslInit");

    log_stage("init_http", 0, 0);
    s_persistent_http = sceHttpInit(s_persistent_pool, s_persistent_ssl, 1024 * 1024);
    CheckOrbis(s_persistent_http, "sceHttpInit");

    LogAppLifecycleEvent("HTTP_POOL_INIT_OK", "pool=aj-gfn-http size=64KB ssl_size=512KB http_size=1MB");
}

} // namespace

HttpResponse HttpClient::Request(
    const std::string& method,
    const std::string& url,
    const std::string& user_agent,
    const std::vector<std::string>& headers,
    const std::string& body,
    const std::string& proxy_url,
    HttpTransferControl control) const
{
    if (url.rfind("https://", 0) != 0)
        throw std::runtime_error("PS4 HTTP client only permits HTTPS requests");
    if (!proxy_url.empty())
        throw std::runtime_error("HTTP proxy is not supported by the PS4 transport yet");

    const std::size_t host_begin = url.find("//") + 2;
    const std::size_t host_end = url.find('/', host_begin);
    const std::string host = url.substr(host_begin,
        host_end == std::string::npos ? std::string::npos : host_end - host_begin);
    std::string stage = "request_begin";
    const auto log_stage = [&](const char* next_stage, long status = 0, std::size_t bytes = 0) {
        stage = next_stage;
        // Keep the in-memory stage precise for failures, but only append the
        // request boundaries during normal operation. The previous trace
        // opened and flushed the unified log for every SceHttp setup call in
        // every queue poll, competing with the render thread for file I/O.
        if (stage != "request_begin" && stage != "request_complete") return;
        std::string detail = "method=" + method + " host=" + host + " stage=" + stage;
        if (status) detail += " http=" + std::to_string(status);
        if (bytes) detail += " bytes=" + std::to_string(bytes);
        LogAppLifecycleEvent("PS4_HTTP_STAGE", detail.c_str());
    };
    log_stage("request_begin");

    std::lock_guard<std::mutex> lock(HttpMutex());
    int32_t tmpl = -1, connection = -1, request = -1;
    const auto cleanup = [&]() {
        if (request >= 0) sceHttpDeleteRequest(request);
        if (connection >= 0) sceHttpDeleteConnection(connection);
        if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
    };

    try
    {
        EnsureHttpInitialized(log_stage);

        log_stage("create_template");
        tmpl = sceHttpCreateTemplate(s_persistent_http, user_agent.c_str(), ORBIS_HTTP_VERSION_1_1, 0);
        CheckOrbis(tmpl, "sceHttpCreateTemplate");
        const long timeout_ms = std::clamp(control.timeout_ms, 100L, 120'000L);
        const uint32_t timeout_us = static_cast<uint32_t>(timeout_ms * 1000L);
        CheckOrbis(sceHttpSetConnectTimeOut(tmpl, timeout_us), "set HTTP connect timeout");
        CheckOrbis(sceHttpSetResolveTimeOut(tmpl, timeout_us), "set HTTP DNS timeout");
        CheckOrbis(sceHttpSetSendTimeOut(tmpl, timeout_us), "set HTTP send timeout");
        CheckOrbis(sceHttpSetRecvTimeOut(tmpl, timeout_us), "set HTTP receive timeout");
        log_stage("create_connection");
        connection = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), false);
        CheckOrbis(connection, "sceHttpCreateConnectionWithURL");
        log_stage("create_request");
        request = sceHttpCreateRequestWithURL2(
            connection, method.c_str(), url.c_str(), static_cast<uint64_t>(body.size()));
        CheckOrbis(request, "sceHttpCreateRequestWithURL2");

        log_stage("add_headers");
        for (const std::string& header : headers)
        {
            const std::size_t colon = header.find(':');
            if (colon == std::string::npos || colon == 0)
                throw std::runtime_error("Invalid HTTP header from GFN client");
            std::size_t value = colon + 1;
            while (value < header.size() && header[value] == ' ') ++value;
            CheckOrbis(sceHttpAddRequestHeader(request, header.substr(0, colon).c_str(),
                                              header.substr(value).c_str(), 0),
                       "sceHttpAddRequestHeader");
        }

        if (control.cancelled && control.cancelled->load())
            throw std::runtime_error("HTTP request cancelled");
        log_stage("send_request");
        CheckOrbis(sceHttpSendRequest(request, body.empty() ? nullptr : body.data(), body.size()),
                   "sceHttpSendRequest");
        int32_t status = 0;
        log_stage("get_status");
        CheckOrbis(sceHttpGetStatusCode(request, &status), "sceHttpGetStatusCode");
        log_stage("read_response", status);

        // A HEAD response never carries a body. On PS4, calling
        // sceHttpReadData() for it returns an error instead of EOF; this made
        // every region latency probe look like a transport failure.
        if (method == "HEAD")
        {
            cleanup();
            log_stage("request_complete", status);
            return HttpResponse{status, {}};
        }

        const std::size_t max_bytes = control.max_body_bytes == 0
            ? 8 * 1024 * 1024 : control.max_body_bytes;
        std::string response;
        char buffer[16 * 1024];
        for (;;)
        {
            if (control.cancelled && control.cancelled->load())
            {
                sceHttpAbortRequest(request);
                throw std::runtime_error("HTTP request cancelled");
            }
            const int32_t count = sceHttpReadData(request, buffer, sizeof(buffer));
            if (count < 0)
                throw std::runtime_error("sceHttpReadData failed (Orbis " +
                                         std::to_string(count) + ")");
            if (count == 0) break;
            if (response.size() + static_cast<std::size_t>(count) > max_bytes)
                throw std::runtime_error("HTTP response exceeds the configured size limit");
            response.append(buffer, static_cast<std::size_t>(count));
        }

        cleanup();
        log_stage("request_complete", status, response.size());
        return HttpResponse{status, std::move(response)};
    }
    catch (const std::exception& error)
    {
        const std::string detail = "method=" + method + " host=" + host +
            " stage=" + stage + " error=" + error.what();
        LogAppLifecycleEvent("PS4_HTTP_FAILED", detail.c_str());
        cleanup();
        throw;
    }
    catch (...)
    {
        const std::string detail = "method=" + method + " host=" + host +
            " stage=" + stage + " error=unknown";
        LogAppLifecycleEvent("PS4_HTTP_FAILED", detail.c_str());
        cleanup();
        throw;
    }
}

HttpResponse HttpClient::Get(const std::string& url, const std::string& user_agent,
                             const std::vector<std::string>& headers,
                             const std::string& proxy_url, HttpTransferControl control) const
{
    return Request("GET", url, user_agent, headers, {}, proxy_url, control);
}

HttpResponse HttpClient::Post(const std::string& url, const std::string& user_agent,
                              const std::vector<std::string>& headers, const std::string& body,
                              const std::string& proxy_url, HttpTransferControl control) const
{
    return Request("POST", url, user_agent, headers, body, proxy_url, control);
}

int HttpClient::MeasureConnectLatencyMs(const std::string& url, long timeout_ms) const noexcept
{
    const auto start = std::chrono::steady_clock::now();
    try
    {
        // SceHttp on PS4 rejects HEAD at sceHttpSendRequest (0x80410116).
        // A ranged GET works on the console; 403/404 still prove reachability.
        (void)Request("GET", url, GfnClient::kUserAgent,
                      {"Range: bytes=0-0"}, {}, {},
                      HttpTransferControl{nullptr, 4096, timeout_ms});
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start).count());
    }
    catch (...)
    {
        return -1;
    }
}

void ShutdownHttpClient()
{
    std::lock_guard<std::mutex> lock(HttpMutex());

    // Reverse creation order. Each destroy call must only run on a live handle;
    // a failure is logged rather than thrown because this runs during shutdown,
    // where an escaping exception would abort the teardown halfway.
    auto report = [](const char* stage, int32_t rc) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "stage=%s rc=0x%08X", stage, static_cast<unsigned>(rc));
        LogAppLifecycleEvent("HTTP_POOL_SHUTDOWN", detail);
    };

    if (s_persistent_http >= 0)
    {
        report("http_term", sceHttpTerm(s_persistent_http));
        s_persistent_http = -1;
    }
    if (s_persistent_ssl >= 0)
    {
        // sceSslTerm() takes no context id and returns void.
        sceSslTerm();
        report("ssl_term", 0);
        s_persistent_ssl = -1;
    }
    if (s_persistent_pool >= 0)
    {
        // sceNetPoolDestroy() returns void.
        sceNetPoolDestroy(s_persistent_pool);
        report("net_pool_destroy", 0);
        s_persistent_pool = -1;
    }

    LogAppLifecycleEvent("HTTP_POOL_SHUTDOWN_DONE",
        (std::string("http=") + (s_persistent_http < 0 ? "closed" : "open") +
         " ssl=" + (s_persistent_ssl < 0 ? "closed" : "open") +
         " pool=" + (s_persistent_pool < 0 ? "closed" : "open")).c_str());
}

} // namespace opennow
