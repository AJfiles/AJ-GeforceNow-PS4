#include "internal.hpp"

#include "../server_location_policy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace opennow
{
using namespace gfn::detail;

namespace
{
constexpr const char* kLcarsClientId = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
constexpr const char* kClientVersion = "2.0.80.173";

std::vector<std::string> BuildRegionHeaders(const std::string& token)
{
    std::vector<std::string> headers = {
        "Accept: application/json",
        "nv-client-id: " + std::string(kLcarsClientId),
        "nv-client-type: BROWSER",
        "nv-client-version: " + std::string(kClientVersion),
        "nv-client-streamer: WEBRTC",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "User-Agent: " + std::string(GfnClient::kUserAgent),
    };
    if (!token.empty())
        headers.push_back("Authorization: GFNJWT " + token);
    return headers;
}

int MeasureAverageLatency(const HttpClient& http_client, const std::string& url)
{
    // One bounded HTTPS HEAD per region is enough to rank options. Keep this
    // aggressive on PS4: a silent/unreachable region must not hold the manual
    // region picker (or the automatic launch preflight) for tens of seconds.
    return http_client.MeasureConnectLatencyMs(url, 350);
}

} // namespace

std::vector<StreamRegion> GfnClient::FetchStreamRegions(AuthSession& session) const
{
    session = RecoverSavedSession(session);
    const std::string base_url =
        server_location::ResolveStreamingBaseUrl("Auto", session.provider.streaming_service_url);
    if (base_url.empty())
        throw std::runtime_error("The GeForce NOW provider has no valid streaming endpoint.");

    const HttpResponse response = http_client_.Get(
        base_url + "v2/serverInfo",
        kUserAgent,
        BuildRegionHeaders(ResolveSessionJwt(session)));
    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Could not load GeForce NOW server locations (HTTP " +
            std::to_string(response.status_code) + ").");
    }

    JsonPtr root = LoadJson(response.body);
    json_t* metadata = json_object_get(root.get(), "metaData");
    if (!json_is_array(metadata))
        return {};

    std::vector<StreamRegion> regions;
    std::unordered_set<std::string> seen_urls;
    size_t index = 0;
    json_t* entry = nullptr;
    json_array_foreach(metadata, index, entry)
    {
        const std::string name = Trim(GetString(entry, "key"));
        const std::string raw_url = Trim(GetString(entry, "value"));
        if (name.empty() || name == "gfn-regions" || name.rfind("gfn-", 0) == 0)
            continue;

        const std::string url = server_location::NormalizeStreamingBaseUrl(raw_url);
        if (url.empty() || !seen_urls.insert(url).second)
            continue;

        regions.push_back({name, url, -1});
    }

    std::sort(regions.begin(), regions.end(), [](const StreamRegion& left, const StreamRegion& right) {
        return left.name < right.name;
    });
    return regions;
}

std::vector<StreamRegion> GfnClient::MeasureStreamRegionLatencies(
    std::vector<StreamRegion> regions) const
{
    if (regions.empty())
        return regions;

    // The PS4 HTTP implementation serializes requests around SceHttp/SceSsl
    // global state. Spawning workers only queues them behind that lock and
    // adds needless thread churn on the console.
    for (StreamRegion& region : regions)
        region.ping_ms = MeasureAverageLatency(http_client_, region.url);
    return regions;
}

} // namespace opennow
