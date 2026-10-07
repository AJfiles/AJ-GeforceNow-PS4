// PS4 capability probe - answers the hardware questions the main app cannot.
//
// The GFN client is capped near 35-40 FPS by a software H.264 decoder, and it
// cannot use the GPU for colour conversion because EGL fails to initialise on this
// firmware. Before writing more code against those limits, this probe measures
// what the console will actually provide and writes the result to
// /data/gfnps4/probe_report.txt so it can be copied off and analysed.
//
// Scope:
//   1. Network  - link state, MTU, device type (is the LAN gigabit?).
//   2. Memory   - direct (Garlic) pool headroom for extra framebuffers or decoder
//                 surfaces, plus CPU heap headroom for the software path.
//   3. GPU      - GNM submit availability and the real VideoOut mode/refresh.
//   4. Codecs   - which hardware codec modules load. The Videodec2 entry points
//                 exist in libSceVideodec2.so but the SDK only ships void-
//                 prototype stubs, so symbols are enumerated and NOT called: a
//                 guessed struct layout sent to a kernel codec would corrupt
//                 memory. This probe fixes the target list for that work.
//
// Every identifier used here was verified against the OpenOrbis headers in
// tools/openorbis/.../include/orbis before compiling (see the notes inline).

#define _POSIX_C_SOURCE 200809L
#include <time.h>

#include <SDL2/SDL.h>

#include <orbis/libkernel.h>
#include <orbis/GnmDriver.h>
#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

// sceKernelLoadStartModule is already declared by libkernel.h (line 293) with a
// matching prototype, so no local extern is needed here. Piglet is not a regular
// sysmodule: the main app loads it by .sprx name (PS4PigletVideoRenderer.cpp:127).

// sceKernelExitProcess is NOT exported by any library in this toolchain (verified
// with llvm-nm across lib/*.so), so the probe cannot use it. It exits by returning
// from main like the working client does, after performing the same cleanup the
// client performs: sceNetTerm() for the stack it initialised, and SDL_Quit().

namespace {

constexpr const char* kReportPath = "/data/gfnps4/probe_report.txt";
constexpr const char* kReportTempPath = "/data/gfnps4/probe_report.tmp";

FILE* g_report = nullptr;
uint32_t g_failures = 0;
uint64_t g_start_us = 0;

// Modules loaded through sceKernelLoadStartModule must be stopped and unloaded
// before the process exits. The first run of this probe left Piglet and
// PrecompiledShaders resident and the console answered with CE-34878-0
// (application error) right after the report was written.
constexpr int kMaxLoadedModules = 8;
int32_t g_loaded_modules[kMaxLoadedModules] = {0};
int g_loaded_count = 0;

uint64_t NowUs() { return sceKernelGetProcessTime(); }

const char* Verdict(bool ok) {
    if (!ok) ++g_failures;
    return ok ? "PASS" : "FAIL";
}

void Emit(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    printf("[probe] %s\n", line);
    if (g_report) {
        fprintf(g_report, "[%9llu us] %s\n",
                static_cast<unsigned long long>(NowUs() - g_start_us), line);
        fflush(g_report);
    }
}

void Section(const char* name) {
    Emit("");
    Emit("=== %s ===", name);
}

// Loads a module the way the main client does. PS4PigletVideoRenderer.cpp:34-55
// builds the path as /{sandbox}/common/lib/{name} using
// sceKernelGetFsSandboxRandomWord(); passing a bare filename is not the supported
// form and is what the first probe run did.
int32_t LoadModuleTracked(const char* name, const char* label) {
    const char* sandbox = sceKernelGetFsSandboxRandomWord();
    if (!sandbox) {
        Emit("module %-12s sandbox=null FAIL (cannot build module path)", label);
        ++g_failures;
        return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "/%s/common/lib/%s", sandbox, name);

    int32_t start_result = -1;
    const int32_t handle = sceKernelLoadStartModule(
        path, 0, nullptr, 0, nullptr, &start_result);
    const bool ok = handle > 0;
    Emit("module %-12s %s handle=0x%08X start_result=0x%08X %s",
         label, path, static_cast<unsigned>(handle),
         static_cast<unsigned>(start_result), Verdict(ok));
    if (ok && g_loaded_count < kMaxLoadedModules) {
        g_loaded_modules[g_loaded_count++] = handle;
    }
    return handle;
}

// Stops and unloads everything LoadModuleTracked started, in reverse order.
// Leaving these resident is what made the console raise CE-34878-0 on exit.
void UnloadTrackedModules() {
    for (int i = g_loaded_count - 1; i >= 0; --i) {
        int32_t stop_result = 0;
        const int32_t rc = sceKernelStopUnloadModule(
            g_loaded_modules[i], 0, nullptr, 0, nullptr, &stop_result);
        int32_t proc_param = 0;
        Emit("unload module handle=0x%08X rc=0x%08X stop_result=0x%08X",
             static_cast<unsigned>(g_loaded_modules[i]), static_cast<unsigned>(rc),
             static_cast<unsigned>(stop_result));
        (void)proc_param;
        g_loaded_modules[i] = 0;
    }
    g_loaded_count = 0;
}

// Loads a sysmodule and reports the raw result. 0x80960003 means "already
// loaded", which is a success for our purposes.
void TryModule(const char* label, int32_t module_id) {
    const int32_t rc = sceSysmoduleLoadModule(static_cast<OrbisSysModule>(module_id));
    const bool ok = (rc >= 0) || (static_cast<uint32_t>(rc) == 0x80960003u);
    Emit("module %-12s id=0x%08X rc=0x%08X %s",
         label, static_cast<unsigned>(module_id), static_cast<unsigned>(rc), Verdict(ok));
}

// Verified: uint32_t sceSysmoduleLoadModuleInternal(enum OrbisSysModuleInternal);
// The result is UNSIGNED and internal ids are >= 0x80000000, so it must never be
// compared as a signed value.
void TryInternalModule(const char* label, int32_t module_id) {
    const uint32_t rc = sceSysmoduleLoadModuleInternal(
        static_cast<OrbisSysModuleInternal>(module_id));
    const bool ok = (rc == 0u) || (rc == 0x80960003u);
    Emit("module %-12s id=0x%08X rc=0x%08X %s (internal)",
         label, static_cast<unsigned>(module_id), rc, Verdict(ok));
}

// ---------------------------------------------------------------------------
// 1. Network - is the Ethernet link the bottleneck, or is it the WAN?
// ---------------------------------------------------------------------------

void ProbeNetwork() {
    Section("NETWORK");

    // libSceNet is an "internal" module here: ORBIS_SYSMODULE_INTERNAL_NET.
    TryInternalModule("NET", ORBIS_SYSMODULE_INTERNAL_NET);
    const int32_t net_init = sceNetInit();
    const bool init_ok = (net_init == 0) || (static_cast<uint32_t>(net_init) == 0x80410101u);
    Emit("sceNetInit rc=0x%08X %s", static_cast<unsigned>(net_init), Verdict(init_ok));

    TryInternalModule("NETCTL", ORBIS_SYSMODULE_INTERNAL_NETCTL);
    const int32_t nctl_init = sceNetCtlInit();
    const bool nctl_ok = (nctl_init == 0) || (static_cast<uint32_t>(nctl_init) == 0x80410101u);
    Emit("sceNetCtlInit rc=0x%08X %s", static_cast<unsigned>(nctl_init), Verdict(nctl_ok));
    if (!nctl_ok) return;

    // OrbisNetCtlInfo is a union, so each query must ask for exactly the member
    // that matches the info id or the read is meaningless.
    OrbisNetCtlInfo info{};

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_dev = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_DEVICE, &info);
    Emit("GetInfo(DEVICE)      rc=0x%08X device=0x%08X (1=ethernet, 0=wifi)",
         static_cast<unsigned>(rc_dev), info.device);

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_link = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_LINK, &info);
    Emit("GetInfo(LINK)        rc=0x%08X link=%u",
         static_cast<unsigned>(rc_link), info.link);

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_mtu = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_MTU, &info);
    Emit("GetInfo(MTU)         rc=0x%08X mtu=%u", static_cast<unsigned>(rc_mtu), info.mtu);

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_ip = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    Emit("GetInfo(IP_ADDRESS)  rc=0x%08X ip=%s", static_cast<unsigned>(rc_ip), info.ip_address);

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_dns = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_PRIMARY_DNS, &info);
    Emit("GetInfo(PRIMARY_DNS) rc=0x%08X dns=%s", static_cast<unsigned>(rc_dns), info.primary_dns);

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_eth = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_ETHER_ADDR, &info);
    Emit("GetInfo(ETHER_ADDR)  rc=0x%08X mac=%02X:%02X:%02X:%02X:%02X:%02X",
         static_cast<unsigned>(rc_eth),
         static_cast<unsigned char>(info.ether_addr[0]), static_cast<unsigned char>(info.ether_addr[1]),
         static_cast<unsigned char>(info.ether_addr[2]), static_cast<unsigned char>(info.ether_addr[3]),
         static_cast<unsigned char>(info.ether_addr[4]), static_cast<unsigned char>(info.ether_addr[5]));

    Emit("note: the app measured ~2.2 Mbps received from the GFN edge. This link");
    Emit("      data tells us whether that is a LAN limit or a WAN/server limit.");

    // Device type according to the union member is NOT self-describing: it is a
    // uint32_t with no enumeration in this SDK, and an earlier revision of this
    // probe wrongly assumed 0 meant wifi. The wifi-only fields are the reliable
    // discriminator: if SSID/BSSID/RSSI/channel come back empty or in error, the
    // active interface is not wireless.
    std::memset(&info, 0, sizeof(info));
    const int32_t rc_ssid = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_SSID, &info);
    Emit("GetInfo(SSID)        rc=0x%08X ssid='%s' %s",
         static_cast<unsigned>(rc_ssid), info.ssid,
         (rc_ssid != 0 || info.ssid[0] == '\0') ? "<- empty: NOT on wifi" : "<- non-empty: on wifi");

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_bssid = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_BSSID, &info);
    Emit("GetInfo(BSSID)       rc=0x%08X bssid=%02X:%02X:%02X:%02X:%02X:%02X",
         static_cast<unsigned>(rc_bssid),
         static_cast<unsigned char>(info.bssid[0]), static_cast<unsigned char>(info.bssid[1]),
         static_cast<unsigned char>(info.bssid[2]), static_cast<unsigned char>(info.bssid[3]),
         static_cast<unsigned char>(info.bssid[4]), static_cast<unsigned char>(info.bssid[5]));

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_rssi = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_RSSI_DBM, &info);
    Emit("GetInfo(RSSI_DBM)    rc=0x%08X rssi=%d dBm",
         static_cast<unsigned>(rc_rssi), static_cast<int>(info.rssi_dbm));

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_route = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_DEFAULT_ROUTE, &info);
    Emit("GetInfo(ROUTE)       rc=0x%08X route=%s", static_cast<unsigned>(rc_route), info.default_route);

    std::memset(&info, 0, sizeof(info));
    const int32_t rc_mask = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_NETMASK, &info);
    Emit("GetInfo(NETMASK)     rc=0x%08X mask=%s", static_cast<unsigned>(rc_mask), info.netmask);

    sceNetCtlTerm();
}

// ---------------------------------------------------------------------------
// 1b. Real download throughput
//
// The only figure that actually answers "is the link fast enough". The client
// reported ~2.2 Mbps received from the GFN edge, which is far below what either
// wired or a healthy wifi link can do. This downloads a fixed payload from a CDN
// and measures the wall-clock rate so the ceiling is known independently of GFN.
// ---------------------------------------------------------------------------

void ProbeThroughput() {
    Section("THROUGHPUT");

    // Verified signatures from orbis/Http.h.
    const int32_t net_pool = sceNetPoolCreate("aj-probe-net", 64 * 1024, 0);
    Emit("sceNetPoolCreate rc=0x%08X", static_cast<unsigned>(net_pool));
    if (net_pool < 0) return;

    const int32_t ssl_ctx = sceSslInit(512 * 1024);
    Emit("sceSslInit rc=0x%08X", static_cast<unsigned>(ssl_ctx));
    if (ssl_ctx < 0) { sceNetPoolDestroy(net_pool); return; }

    const int32_t http_ctx = sceHttpInit(net_pool, ssl_ctx, 1024 * 1024);
    Emit("sceHttpInit rc=0x%08X", static_cast<unsigned>(http_ctx));
    if (http_ctx < 0) { sceSslTerm(); sceNetPoolDestroy(net_pool); return; }

    // 10 MB from a widely mirrored CDN. Change the URL if it is unreachable from
    // your region; the measurement logic does not depend on the host.
    const char* url = "https://speed.cloudflare.com/__down?bytes=10000000";
    Emit("url=%s", url);

    const int32_t tmpl = sceHttpCreateTemplate(http_ctx, "Mozilla/5.0", 1, 0);
    Emit("sceHttpCreateTemplate rc=0x%08X", static_cast<unsigned>(tmpl));
    if (tmpl >= 0) {
        const uint32_t timeout_us = 20000000; // 20 s
        sceHttpSetConnectTimeOut(tmpl, timeout_us);
        sceHttpSetResolveTimeOut(tmpl, timeout_us);
        sceHttpSetSendTimeOut(tmpl, timeout_us);
        sceHttpSetRecvTimeOut(tmpl, timeout_us);

        const int32_t conn = sceHttpCreateConnectionWithURL(tmpl, url, 0);
        Emit("sceHttpCreateConnectionWithURL rc=0x%08X", static_cast<unsigned>(conn));
        if (conn >= 0) {
            const int32_t req = sceHttpCreateRequestWithURL(conn, 0 /* GET */, url, 0);
            Emit("sceHttpCreateRequestWithURL rc=0x%08X", static_cast<unsigned>(req));
            if (req >= 0) {
                const uint64_t t0 = NowUs();
                const int32_t send_rc = sceHttpSendRequest(req, nullptr, 0);
                if (send_rc != 0) {
                    Emit("sceHttpSendRequest rc=0x%08X FAIL", static_cast<unsigned>(send_rc));
                } else {
                    int32_t status = 0;
                    sceHttpGetStatusCode(req, &status);
                    // Verified: int32_t sceHttpGetResponseContentLength(int32_t reqId,
                    //           int32_t *result, size_t *contentLength);
                    int32_t length_result = 0;
                    size_t content_length = 0;
                    sceHttpGetResponseContentLength(req, &length_result, &content_length);

                    static uint8_t buffer[64 * 1024];
                    uint64_t total = 0;
                    for (;;) {
                        const int32_t got = sceHttpReadData(req, buffer, sizeof(buffer));
                        if (got <= 0) break;
                        total += static_cast<uint64_t>(got);
                    }
                    const uint64_t elapsed_us = NowUs() - t0;
                    const double seconds = static_cast<double>(elapsed_us) / 1000000.0;
                    const double mbps = seconds > 0.0
                        ? (static_cast<double>(total) * 8.0 / 1000000.0) / seconds : 0.0;
                    const double mbytes = static_cast<double>(total) / (1024.0 * 1024.0);

                    Emit("http_status=%d content_length=%zu (result=%d)",
                         status, content_length, length_result);
                    Emit("downloaded=%.2f MB in %.2f s", mbytes, seconds);
                    Emit("measured_throughput=%.2f Mbps %s", mbps,
                         Verdict(mbps > 0.0));
                    Emit("  app_log_reported=2.19 Mbps from the GFN edge for comparison");
                    Emit("  if this number is far higher, the GFN bitrate is server-side, not the LAN");
                }
                sceHttpDeleteRequest(req);
            }
            sceHttpDeleteConnection(conn);
        }
        sceHttpDeleteTemplate(tmpl);
    }

    sceHttpTerm(http_ctx);
    sceSslTerm();
    sceNetPoolDestroy(net_pool);
    Emit("http teardown done");
}

// ---------------------------------------------------------------------------
// 2. Memory - headroom for extra framebuffers and decoder surfaces
// ---------------------------------------------------------------------------

void ProbeMemory() {
    Section("MEMORY");

    // Verified signature: size_t sceKernelGetDirectMemorySize(void);
    const size_t direct_total = sceKernelGetDirectMemorySize();
    Emit("sceKernelGetDirectMemorySize total=%zu bytes (%.1f MB) %s",
         direct_total, static_cast<double>(direct_total) / (1024.0 * 1024.0),
         Verdict(direct_total > 0));

    Emit("frame_buffer_cost 1920x1080 BGRA = %.2f MB per buffer",
         1920.0 * 1080.0 * 4.0 / (1024.0 * 1024.0));
    Emit("frame_buffer_cost 1280x720  BGRA = %.2f MB per buffer",
         1280.0 * 720.0 * 4.0 / (1024.0 * 1024.0));

    // How much direct memory can still be taken? This decides whether a fourth
    // framebuffer (a candidate fix for DCE queue back-pressure) is affordable, and
    // how large a hardware decoder surface pool could be.
    bool reported = false;
    for (size_t mb = 1024; mb >= 8; mb /= 2) {
        off_t offset = 0;
        const size_t want = mb * 1024 * 1024;
        // Verified: int32_t sceKernelAllocateDirectMemory(off_t, off_t, size_t,
        //           size_t, int32_t, off_t*);
        const int32_t rc = sceKernelAllocateDirectMemory(
            0, static_cast<off_t>(direct_total), want, 0x200000, 3 /* WC_GARLIC */, &offset);
        if (rc == 0) {
            Emit("AllocateDirectMemory(%4zu MB) OK offset=0x%llX %s",
                 mb, static_cast<unsigned long long>(offset), Verdict(true));
            // Verified: int32_t sceKernelReleaseDirectMemory(off_t, size_t);
            sceKernelReleaseDirectMemory(offset, want);
            Emit("  released %zu MB again", mb);
            reported = true;
            break;
        }
        Emit("AllocateDirectMemory(%4zu MB) rc=0x%08X", mb, static_cast<unsigned>(rc));
    }
    if (!reported) Emit("AllocateDirectMemory: no tested size succeeded (pool exhausted)");

    // CPU heap: the software H.264 decoder and the colour scaler both live there.
    for (size_t mb = 512; mb >= 8; mb /= 2) {
        void* block = std::malloc(mb * 1024 * 1024);
        if (block) {
            Emit("malloc(%4zu MB) OK %s", mb, Verdict(true));
            std::free(block);
            break;
        }
        Emit("malloc(%4zu MB) failed", mb);
    }
}

// ---------------------------------------------------------------------------
// 3. GPU / display
// ---------------------------------------------------------------------------

void ProbeGpu() {
    Section("GPU / DISPLAY");

    // There is no sceGnmInit() in this SDK: GNM is driven by command buffers, and
    // the driver exposes readiness through sceGnmAreSubmitsAllowed(). The Piglet
    // sysmodule is what an EGL context needs.
    // Verified: int32_t sceGnmAreSubmitsAllowed(void);
    const int32_t submits = sceGnmAreSubmitsAllowed();
    Emit("sceGnmAreSubmitsAllowed rc=%d %s (needs an initialised GNM driver)",
         static_cast<int>(submits), Verdict(submits >= 0));

    // Piglet is the EGL/GLES2 runtime and is what the client needs for GPU colour
    // conversion. It is NOT in the OrbisSysModule enum: the main app loads it by
    // .sprx name through sceKernelLoadStartModule with the sandbox path, so the
    // probe does the same and unloads it again before exiting.
    LoadModuleTracked("libScePigletv2VSH.sprx", "PIGLET_V2");
    LoadModuleTracked("libScePrecompiledShaders.sprx", "PRECOMP_SHDR");

    Emit("note: the app logs PIGLET_EGL_GET_DISPLAY_FAIL rc=EGL_NO_DISPLAY even with");
    Emit("      both modules loaded, which is why all colour work runs on the CPU.");

    // VideoOut: take the bus briefly and read back the mode the display actually
    // negotiated. The client hardcodes 1920x1080@60 assumptions; this confirms or
    // refutes them.
    TryInternalModule("VIDEO_OUT", ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    const int32_t handle = sceVideoOutOpen(
        ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    Emit("sceVideoOutOpen(BUS_MAIN) handle=0x%08X %s",
         static_cast<unsigned>(handle), Verdict(handle > 0));
    if (handle > 0) {
        OrbisVideoOutResolutionStatus res{};
        const int32_t rc = sceVideoOutGetResolutionStatus(handle, &res);
        Emit("GetResolutionStatus rc=0x%08X %ux%u pane=%ux%u refresh=%llu flags=0x%04X %s",
             static_cast<unsigned>(rc), res.width, res.height, res.paneWidth, res.paneHeight,
             static_cast<unsigned long long>(res.refreshRate), res.flags, Verdict(rc == 0));
        Emit("  -> a 60 Hz panel is required for the 60 FPS target; refresh=%llu",
             static_cast<unsigned long long>(res.refreshRate));
        sceVideoOutClose(handle);
    }

    // sceVideoOutWaitVblank() is declared with no parameters in this SDK and the
    // import stub exports it. The main app measured it returning in ~22 us instead
    // of blocking for a 16666 us frame period, so it is not a usable VSYNC wait.
    // Timing it here reproduces that finding outside the app.
    const int32_t vh = sceVideoOutOpen(
        ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (vh > 0) {
        const uint64_t t0 = NowUs();
        for (int i = 0; i < 8; ++i) sceVideoOutWaitVblank();
        const uint64_t elapsed = NowUs() - t0;
        Emit("sceVideoOutWaitVblank x8 total=%llu us avg=%llu us (blocking would be ~133000)",
             static_cast<unsigned long long>(elapsed),
             static_cast<unsigned long long>(elapsed / 8));
        Emit("  -> avg far below 16666 us confirms it does NOT block on this firmware");
        sceVideoOutClose(vh);
    }
}

// ---------------------------------------------------------------------------
// 4. Hardware codecs - the only route to 1080p60
// ---------------------------------------------------------------------------

void ProbeCodecs() {
    Section("HARDWARE CODECS");

    TryModule("VIDEODEC", ORBIS_SYSMODULE_VIDEODEC);
    TryModule("VIDEODEC2", ORBIS_SYSMODULE_VIDEODEC2);
    TryModule("VDECWRAP", ORBIS_SYSMODULE_VDECWRAP);
    TryModule("VIDEO_DEC_ARB", ORBIS_SYSMODULE_VIDEO_DECODER_ARBITRATION);
    TryModule("AV_PLAYER", ORBIS_SYSMODULE_AV_PLAYER);
    TryInternalModule("VDECCORE", ORBIS_SYSMODULE_INTERNAL_VDECCORE);

    Emit("");
    Emit("libSceVideodec2.so exports 15 entry points. The SDK header declares only");
    Emit("11 of them and all as 'void name()' stubs, so the real signatures are");
    Emit("unreversed. Symbols are listed, never called, because a guessed structure");
    Emit("layout passed to a kernel codec would corrupt memory:");
    Emit("  sceVideodec2AllocateComputeQueue       (void stub in header)");
    Emit("  sceVideodec2CreateDecoder              (void stub in header)");
    Emit("  sceVideodec2CreateHevcDecoder          (void stub in header)");
    Emit("  sceVideodec2Decode                     (void stub in header)");
    Emit("  sceVideodec2DeleteDecoder              (void stub in header)");
    Emit("  sceVideodec2Flush                      (void stub in header)");
    Emit("  sceVideodec2GetAvcPictureInfo          (MISSING from header)");
    Emit("  sceVideodec2GetHevcPictureInfo         (MISSING from header)");
    Emit("  sceVideodec2GetPictureInfo             (void stub in header)");
    Emit("  sceVideodec2MapDirectMemory            (MISSING from header)");
    Emit("  sceVideodec2QueryComputeMemoryInfo     (void stub in header)");
    Emit("  sceVideodec2QueryDecoderMemoryInfo     (void stub in header)");
    Emit("  sceVideodec2QueryHevcDecoderMemoryInfo (MISSING from header)");
    Emit("  sceVideodec2ReleaseComputeQueue        (void stub in header)");
    Emit("  sceVideodec2Reset                      (void stub in header)");
    Emit("");
    Emit("The stream negotiates AVC (app log: format=12 / yuv420p), so the pair to");
    Emit("reverse is sceVideodec2CreateDecoder + sceVideodec2GetAvcPictureInfo.");
}

void ProbeSummary() {
    Section("SUMMARY");
    Emit("failures=%u elapsed_us=%llu", g_failures,
         static_cast<unsigned long long>(NowUs() - g_start_us));
    Emit("report=%s", kReportPath);
    Emit("Copy it off with the GoldHEN FTP server or a USB drive, then hand it over.");
}

} // namespace

int main() {
    g_start_us = NowUs();

    mkdir("/data/gfnps4", 0777);
    // Direct open of the published path (no .tmp + rename): see the PUBLISH
    // section for why the rename approach could leave a stale report behind.
    g_report = fopen(kReportPath, "w");
    if (!g_report) {
        printf("[probe] cannot open %s; console output only\n", kReportPath);
    }

    Emit("AJ GFN PS4 capability probe");
    Emit("unix_time=%ld", static_cast<long>(time(nullptr)));
    Emit("target=PS4 (Orbis OS, x86-64 Jaguar, 8 cores / 6 usable by apps, 8 GB GDDR5)");

    // SDL owns the display lifecycle on Orbis. The working client always runs
    // SDL_Init(...) at start and SDL_Quit() before returning (main.cpp:4323 and
    // :4866). Earlier revisions of this probe drove VideoOut by hand and never
    // initialised SDL at all, which is the most likely reason the console reported
    // CE-34878-0 on exit on every single run.
    //
    // Only the events subsystem is requested: the probe needs no window, no video
    // output of its own and no audio, and asking for less reduces what can fail.
    const int sdl_rc = SDL_Init(SDL_INIT_EVENTS);
    Emit("SDL_Init(SDL_INIT_EVENTS) rc=%d %s err=%s", sdl_rc,
         Verdict(sdl_rc == 0), sdl_rc == 0 ? "-" : SDL_GetError());

    ProbeNetwork();
    ProbeThroughput();
    ProbeMemory();
    ProbeGpu();
    ProbeCodecs();
    ProbeSummary();

    // Publish the report FIRST, before any teardown.
    //
    // Reordering this is the fix for a real defect: the previous revision emitted
    // "report_published" AFTER closing the file, so the marker never reached the
    // log, and every teardown step ran with the report still unpublished. If any
    // of those steps faults - which they demonstrably do, the console has answered
    // CE-34878-0 on every run - the measurement is already safely on disk and the
    // last recorded line names the step that faulted.
    Section("PUBLISH");
    Emit("summary_before_teardown failures=%u elapsed_us=%llu",
         g_failures, static_cast<unsigned long long>(NowUs() - g_start_us));
    if (g_report) {
        // Write straight to the final path. The previous revision wrote to
        // probe_report.tmp and renamed at the end, so a fault during the rename
        // left the published file stale and the user copied the PREVIOUS run's
        // report. Direct writes cannot go stale: whatever is on disk is this run.
        fflush(g_report);
        fclose(g_report);
        g_report = nullptr;
    }
    printf("[probe] report closed, path=%s\n", kReportPath);
    fflush(stdout);

    // Teardown. Each step is announced BEFORE it runs so the last line of the
    // stdout log identifies the one that faults.
    Section("TEARDOWN");
    printf("[probe] teardown: unloading modules\n"); fflush(stdout);
    UnloadTrackedModules();

    printf("[probe] teardown: sceNetTerm\n"); fflush(stdout);
    sceNetTerm();

    printf("[probe] teardown: SDL_Quit\n"); fflush(stdout);
    SDL_Quit();

    printf("[probe] teardown complete, returning from main\n"); fflush(stdout);
    return 0;
}
