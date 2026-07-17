// mcp_resources.test.cpp — bucket-E integration coverage for the MCP `resources`
// primitive (AGENTIC_INFRA_AUDIT.md Proposal P2).
//
// WHY THIS EXISTS: the `smatchet://commands` command-catalog resource is served
// in-process by the JSON-RPC dispatch (resources/list + resources/read), sharing
// one BuildToolCatalogJson() source with tools/list so the two can't drift. Only
// a real socket proves the new dispatch arms are wired, gated by Authorize, and
// emit the JSON-RPC envelope a client observes. Resources are in-process ONLY —
// dev-repo docs are not shipped beside the binary, so the catalog is the one
// always-available resource (option 2 of the P2/P3 design decision).
//
// TEST-LOCAL SERVER, NOT THE RIG'S: identical rationale to
// mcp_live_http_auth.test.cpp — the bucket-E run is itself driven over MCP, so
// this constructs a SECOND McpPlugin on its own port, snapshotting/restoring the
// persisted config (OnStart re-reads the auth token) and instance.json.
//
// ASSERTED OVER REAL JSON-RPC (loopback bind, token configured):
//   1. initialize                              → capabilities advertises "resources"
//   2. resources/list                          → the smatchet://commands descriptor
//   3. resources/read smatchet://commands      → a contents entry with tool schemas
//   4. resources/read on an unknown URI        → in-envelope JSON-RPC -32602 (HTTP 200)
//   5. resources/list with NO token            → 401 (single Authorize gate covers it)

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_MCP)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "ConfigManager.h"
#include "McpPlugin.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <httplib.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace {

const char kTestToken[] = "bucket-e-mcp-resources-token";

// Fixture with unconditional teardown (IM_CHECK returns out of TestFunc on
// failure): stop the test server, restore instance.json + the persisted config.
struct LiveMcpFixture {
    std::unique_ptr<McpPlugin> Plugin;
    TrackerConfig OriginalCfg;
    bool CfgSnapshotted = false;
    std::string InstancePath;
    std::string InstanceJsonBackup;
    bool HadInstanceJson = false;

    ~LiveMcpFixture() {
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

// POST a JSON-RPC body to /mcp/messages carrying the valid auth token.
httplib::Result PostRpc(int port, const std::string& body) {
    httplib::Client cli = MakeClient(port);
    httplib::Headers h;
    h.emplace("X-Smatchet-Token", kTestToken);
    return cli.Post("/mcp/messages", h, body, "application/json");
}

void RegisterResourcesTest(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "McpResources", "CommandCatalog_RealSocket");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        LiveMcpFixture fx;

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

        fx.OriginalCfg = ConfigManager::Load();
        fx.CfgSnapshotted = true;
        {
            TrackerConfig cfg = fx.OriginalCfg;
            cfg.McpAuthToken = kTestToken;
            cfg.McpRequireTokenOnLoopback = true;
            cfg.McpAllowRemote = false; // loopback bind
            ConfigManager::Save(cfg);
        }

        const int kCandidatePorts[] = {38771, 45823, 52937};
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

        // 1. initialize advertises the resources capability.
        {
            httplib::Result r = PostRpc(port, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
            IM_CHECK(r.error() == httplib::Error::Success);
            IM_CHECK_EQ(r->status, 200);
            IM_CHECK(r->body.find("\"resources\"") != std::string::npos);
        }

        // 2. resources/list returns the in-process smatchet://commands descriptor.
        {
            httplib::Result r = PostRpc(port, R"({"jsonrpc":"2.0","id":2,"method":"resources/list"})");
            IM_CHECK(r.error() == httplib::Error::Success);
            IM_CHECK_EQ(r->status, 200);
            IM_CHECK(r->body.find("smatchet://commands") != std::string::npos);
        }

        // 3. resources/read on the catalog returns a contents entry with tool schemas.
        {
            httplib::Result r = PostRpc(
                port, R"({"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"smatchet://commands"}})");
            IM_CHECK(r.error() == httplib::Error::Success);
            IM_CHECK_EQ(r->status, 200);
            IM_CHECK(r->body.find("\"contents\"") != std::string::npos);
            IM_CHECK(r->body.find("inputSchema") != std::string::npos);
        }

        // 4. resources/read on an unknown URI → in-envelope JSON-RPC -32602 (HTTP 200).
        {
            httplib::Result r = PostRpc(
                port, R"({"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"smatchet://nope"}})");
            IM_CHECK(r.error() == httplib::Error::Success);
            IM_CHECK_EQ(r->status, 200);
            IM_CHECK(r->body.find("-32602") != std::string::npos);
        }

        // 5. No token → 401: the single Authorize gate covers the new methods too.
        {
            httplib::Client cli = MakeClient(port);
            httplib::Result r =
                cli.Post("/mcp/messages", R"({"jsonrpc":"2.0","id":5,"method":"resources/list"})", "application/json");
            IM_CHECK(r.error() == httplib::Error::Success);
            IM_CHECK_EQ(r->status, 401);
        }
    };
}

} // namespace

extern "C" void SmatchetRegisterMcpResourcesTests(ImGuiTestEngine* engine) { RegisterResourcesTest(engine); }

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_MCP
