// mcp_live_http_auth.test.cpp — bucket-E integration coverage for the MCP
// live-HTTP `Authorize` path (AGENTIC_INFRA_AUDIT.md finding C6).
//
// WHY THIS EXISTS: `IsMcpHostOriginAllowed`, `ConstantTimeStringEquals`, and
// `CanAcceptSseConnection` all have pure-helper doctests, but nothing drove
// `McpPlugin::Authorize` over a REAL httplib socket — the layer where route
// registration order and header plumbing could silently diverge from the pure
// helpers. This test starts a real `McpPlugin` and asserts the wire behaviour
// a hostile local client would observe.
//
// TEST-LOCAL SERVER, NOT THE RIG'S: the bucket-E run itself is driven over MCP
// (the CLI parent spawns this exe with --mcp-port and dispatches ui_test.run
// through the registered plugin). Restarting THAT plugin via
// PluginHost::SyncMcpPluginWithConfig would sever the parent's connection
// mid-run — so this test constructs a SECOND McpPlugin instance on its own
// port and starts it directly with OnStart(app). The fixture snapshots and
// restores both the persisted config (OnStart re-reads ConfigManager::Load()
// for the auth token) and instance.json (OnStart of the test plugin overwrites
// it; OnStop deletes it — either would orphan the rig's own discovery file).
//
// ASSERTED OVER REAL HTTP (loopback bind, token configured,
// require_token_on_loopback ON — the secure defaults):
//   1. valid token, loopback Host, no Origin          → 200 (tools/list body)
//   2. no token                                        → 401 + WWW-Authenticate
//   3. wrong token                                     → 401
//   4. hostile Host (DNS-rebind shape) + VALID token   → 403 (Host gate fires
//      before the token check — cpp-httplib v0.49 honours a caller-supplied
//      Host header: it only auto-fills when the request has none)
//   5. hostile Origin + VALID token                    → 403
//   6. kMaxConcurrentSseConnections (4) streams held   → 5th SSE connect → 503

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_MCP)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "ConfigManager.h"
#include "McpJsonRpcPure.h"
#include "McpPlugin.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char kTestToken[] = "bucket-e-mcp-live-auth-token";

// Fixture with unconditional teardown: IM_CHECK returns out of TestFunc on
// failure, so every cleanup step lives in the destructor — SSE holder threads
// joined, the test server stopped, instance.json and the persisted config
// restored — no matter where the test bails.
struct LiveMcpFixture {
    std::unique_ptr<McpPlugin> Plugin;
    TrackerConfig OriginalCfg;
    bool CfgSnapshotted = false;
    std::string InstancePath;
    std::string InstanceJsonBackup;
    bool HadInstanceJson = false;
    std::atomic<bool> SseRelease{false};
    std::atomic<int> SseConnected{0};
    std::vector<std::thread> SseHolders;

    ~LiveMcpFixture() {
        SseRelease.store(true, std::memory_order_release);
        for (size_t i = 0; i < SseHolders.size(); ++i) {
            if (SseHolders[i].joinable()) {
                SseHolders[i].join();
            }
        }
        if (Plugin) {
            Plugin->OnStop(); // also deletes instance.json (ours, restored below)
            Plugin.reset();
        }
        if (HadInstanceJson && !InstancePath.empty()) {
            std::ofstream out(InstancePath.c_str(), std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out << InstanceJsonBackup;
            }
        }
        if (CfgSnapshotted) {
            ConfigManager::Save(OriginalCfg);
        }
    }
};

httplib::Client MakeClient(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);
    cli.set_write_timeout(5, 0);
    return cli;
}

void RegisterLiveHttpAuthTest(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "McpLiveHttp", "Authorize_RealSocket");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        LiveMcpFixture fx;

        // Snapshot instance.json BEFORE our plugin's OnStart overwrites it with
        // the test port (and OnStop later deletes it) — the rig's parent CLI
        // owns that file.
        fx.InstancePath = ConfigManager::GetUserDataDirectory() + "instance.json";
        {
            std::ifstream in(fx.InstancePath.c_str(), std::ios::binary);
            if (in.is_open()) {
                std::ostringstream buf;
                buf << in.rdbuf();
                fx.InstanceJsonBackup = buf.str();
                fx.HadInstanceJson = true;
            }
        }

        // OnStart reads the persisted config for the auth token + hardening
        // knobs; persist a known token, then restore the snapshot as soon as
        // the server is up (the plugin never re-reads config after OnStart).
        fx.OriginalCfg = ConfigManager::Load();
        fx.CfgSnapshotted = true;
        {
            TrackerConfig cfg = fx.OriginalCfg;
            cfg.McpAuthToken = kTestToken;
            cfg.McpRequireTokenOnLoopback = true;
            cfg.McpAllowRemote = false; // loopback bind → Host/Origin gate armed
            ConfigManager::Save(cfg);
        }

        // Start the test-local server; a busy candidate port just fails the
        // bind (ServerRunning stays false) — try the next one.
        const int kCandidatePorts[] = {38763, 45817, 52929};
        int port = 0;
        for (size_t p = 0; p < sizeof(kCandidatePorts) / sizeof(kCandidatePorts[0]); ++p) {
            fx.Plugin = std::make_unique<McpPlugin>(kCandidatePorts[p]);
            fx.Plugin->OnStart(*app);
            for (int i = 0; i < 300 && !fx.Plugin->GetStatus().ServerRunning; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (fx.Plugin->GetStatus().ServerRunning) {
                port = kCandidatePorts[p];
                break;
            }
            fx.Plugin->OnStop();
        }
        IM_CHECK(port != 0);
        ConfigManager::Save(fx.OriginalCfg); // token captured by the server; restore early
        IM_CHECK(fx.Plugin->GetStatus().AuthRequired);

        // 1. Valid token, loopback Host (client auto-fills 127.0.0.1:<port>),
        //    no Origin → 200 with the tools/list payload.
        {
            httplib::Client cli = MakeClient(port);
            httplib::Headers h;
            h.emplace("X-Smatchet-Token", kTestToken);
            httplib::Result httpRes = cli.Get("/mcp/tools/list", h);
            IM_CHECK(httpRes.error() == httplib::Error::Success);
            IM_CHECK_EQ(httpRes->status, 200);
            IM_CHECK(httpRes->body.find("tools") != std::string::npos);
        }

        // 2. No token → 401 + WWW-Authenticate challenge.
        {
            httplib::Client cli = MakeClient(port);
            httplib::Result httpRes = cli.Get("/mcp/tools/list");
            IM_CHECK(httpRes.error() == httplib::Error::Success);
            IM_CHECK_EQ(httpRes->status, 401);
            IM_CHECK(httpRes->has_header("WWW-Authenticate"));
        }

        // 3. Wrong token → 401 (constant-time compare rejects).
        {
            httplib::Client cli = MakeClient(port);
            httplib::Headers h;
            h.emplace("X-Smatchet-Token", "not-the-token");
            httplib::Result httpRes = cli.Get("/mcp/tools/list", h);
            IM_CHECK(httpRes.error() == httplib::Error::Success);
            IM_CHECK_EQ(httpRes->status, 401);
        }

        // 4. DNS-rebind shape: attacker's hostname in Host, token VALID —
        //    the Host gate must fire before the token is even considered.
        {
            httplib::Client cli = MakeClient(port);
            httplib::Headers h;
            h.emplace("Host", "evil.example");
            h.emplace("X-Smatchet-Token", kTestToken);
            httplib::Result httpRes = cli.Get("/mcp/tools/list", h);
            IM_CHECK(httpRes.error() == httplib::Error::Success);
            IM_CHECK_EQ(httpRes->status, 403);
        }

        // 5. Cross-origin browser shape: hostile Origin, token VALID → 403.
        {
            httplib::Client cli = MakeClient(port);
            httplib::Headers h;
            h.emplace("Origin", "https://evil.example");
            h.emplace("X-Smatchet-Token", kTestToken);
            httplib::Result httpRes = cli.Get("/mcp/tools/list", h);
            IM_CHECK(httpRes.error() == httplib::Error::Success);
            IM_CHECK_EQ(httpRes->status, 403);
        }

        // 6. SSE cap: hold kMaxConcurrentSseConnections streams open (each
        //    holder counts itself connected on the initial `endpoint` event and
        //    then keeps the stream alive until SseRelease), then assert the
        //    next connect is rejected 503 with Retry-After.
        {
            const int kCap = smatchet::mcp::pure::kMaxConcurrentSseConnections;
            LiveMcpFixture* fxp = &fx;
            for (int i = 0; i < kCap; ++i) {
                fx.SseHolders.emplace_back([fxp, port]() {
                    httplib::Client cli("127.0.0.1", port);
                    cli.set_connection_timeout(5, 0);
                    // Read timeout must exceed the server's 1 s heartbeat cadence.
                    cli.set_read_timeout(10, 0);
                    httplib::Headers h;
                    h.emplace("X-Smatchet-Token", kTestToken);
                    bool counted = false;
                    cli.Get("/mcp/sse", h, [fxp, &counted](const char*, size_t) {
                        if (!counted) {
                            counted = true;
                            fxp->SseConnected.fetch_add(1, std::memory_order_acq_rel);
                        }
                        // false aborts the stream — released by the fixture.
                        return !fxp->SseRelease.load(std::memory_order_acquire);
                    });
                });
            }
            for (int i = 0; i < 500 && fx.SseConnected.load(std::memory_order_acquire) < kCap; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            IM_CHECK_EQ(fx.SseConnected.load(std::memory_order_acquire), kCap);

            httplib::Client cli = MakeClient(port);
            httplib::Headers h;
            h.emplace("X-Smatchet-Token", kTestToken);
            httplib::Result httpRes = cli.Get("/mcp/sse", h);
            IM_CHECK(httpRes.error() == httplib::Error::Success);
            IM_CHECK_EQ(httpRes->status, 503);
            IM_CHECK(httpRes->has_header("Retry-After"));
        }
        // Fixture destructor: release + join SSE holders, stop the test server,
        // restore instance.json and the persisted config.
    };
}

} // namespace

extern "C" void SmatchetRegisterMcpLiveHttpAuthTests(ImGuiTestEngine* engine) {
    RegisterLiveHttpAuthTest(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_MCP
