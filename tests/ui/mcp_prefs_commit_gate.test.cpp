// mcp_prefs_commit_gate.test.cpp — bucket-E coverage for the MCP Preferences
// commit gate shipped for #2110 (Preferences restarted the MCP plugin on every
// keystroke in the auth-token field).
//
// Before the fix, DrawMcpSectionBody ran a per-frame state diff of the MCP widget
// buffers against d.cfg and committed the token buffer the same frame it changed,
// and every commit drove PluginHost::SyncMcpPluginWithConfig — a synchronous MCP
// listen-socket teardown and rebind. Typing an N-character token produced N
// restarts. The fix gates the two typed fields (token, port) on "this field is
// not currently being edited".
//
// Drift warning — IF YOU CHANGE
//   - Source/Core/include/Ui/SmatchetSecretInput.h:27-35 (the bool* outEditing
//     out-param and the point it is sampled), OR
//   - Source/Core/src/Ui/SmatchetPreferencesUi.cpp:874-885 (portEditing sample),
//     :903-906 (tokenEditing sample), or :920-951 (the gated dirty-diff +
//     commit + SyncMcpPluginWithConfig block),
// UPDATE THIS FILE. Variant 1 binds directly to the real header, but variant 2's
// commit block is a REPLICA: DrawMcpSectionBody lives in an anonymous namespace
// and needs a live AppController + PluginHost, so it is not callable from here.
//
// Two variants:
//   1. SecretInput_ReportsEditingState (PRIMARY, binds real production code) —
//      drives the REAL SmatchetSecretInputText and asserts its new out-param
//      reports keyboard focus. Also asserts the hazard the out-param exists for:
//      an ImGui::IsItemActive() sampled by the CALLER after the widget returns
//      reads the trailing SmallButton / TextDisabled, NOT the input, so it is
//      false while the field is being typed into. Deleting the out-param and
//      "simplifying" to a call-site sample reintroduces #2110; this fails first.
//   2. CommitGate_OneSyncPerBurst (SECONDARY, replica of the commit block) —
//      locks the gate COMPOSITION: both the dirty comparison and the assignment
//      into cfg are gated, so a typing burst yields zero syncs and zero prefix
//      writes, then exactly one sync on focus loss, while an ungated checkbox
//      still syncs immediately. Gating only the comparison — the tempting
//      simplification — flushes a half-typed prefix as soon as another field
//      trips the diff, and is asserted against here.
//
// Not covered: the "MCP Port" InputInt. Its gate is the same composition with a
// directly-sampled IsItemActive() (safe there: InputInt wraps input + step
// buttons in a group, and EndGroup copies a contained ActiveId onto the group),
// but the test engine cannot address the inner input of an InputInt-with-step-
// buttons by a stable item path, so driving it would be a flaky assert rather
// than coverage.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_MCP)

#include "Ui/SmatchetSecretInput.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <string>

namespace {

// --- Variant 1: the real widget reports its own editing state -------------

struct SecretInputState {
    char buf[128];
    bool reportedEditing;   // what SmatchetSecretInputText's out-param says
    bool naiveCallerActive; // what a caller's own IsItemActive() would have said
};

SecretInputState g_secretState;

void ResetSecretState() {
    std::memset(g_secretState.buf, 0, sizeof(g_secretState.buf));
    g_secretState.reportedEditing = false;
    g_secretState.naiveCallerActive = false;
}

void RegisterSecretInputEditingStateVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "McpPrefs", "SecretInput_ReportsEditingState");
    t->UserData = &g_secretState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        SecretInputState* s = static_cast<SecretInputState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(520, 120), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::McpSecretInput", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            s->reportedEditing = false;
            SmatchetSecretInputText("##McpTokenProbe", s->buf, sizeof(s->buf), &s->reportedEditing);
            // Sampled exactly where DrawMcpSectionBody would sample it if the widget did
            // not report its own state. The last submitted item here is the Show/Hide
            // SmallButton or the char-count TextDisabled — never the inner InputText.
            s->naiveCallerActive = ImGui::IsItemActive();
            ImGui::Button("Sink##McpSecretInput"); // focus target: click to deactivate
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        SecretInputState* s = static_cast<SecretInputState*>(ctx->Test->UserData);
        ResetSecretState();
        ctx->SetRef("SmatchetTest::McpSecretInput");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(!s->reportedEditing); // nothing focused yet

        ctx->ItemClick("##McpTokenProbe");
        ctx->KeyChars("abc");
        ctx->Yield();

        // The buffer now holds a half-typed PREFIX, and the widget says so.
        IM_CHECK_STR_EQ(s->buf, "abc");
        IM_CHECK(s->reportedEditing);
        // ...while the naive call-site sample does NOT. This is the whole reason the
        // out-param exists (#2110): a caller gating on its own IsItemActive() here
        // would see "not editing" and commit the prefix on every keystroke.
        IM_CHECK(!s->naiveCallerActive);

        ctx->ItemClick("Sink##McpSecretInput");
        ctx->Yield();
        IM_CHECK(!s->reportedEditing);
        IM_CHECK_STR_EQ(s->buf, "abc"); // deactivation keeps the typed value
    };
}

// --- Variant 2: commit-gate composition (replica) -------------------------

struct CommitGateState {
    // Widget buffers — mirror UiDrawSession's mcpAuthTokenBuf / mcpEnabled.
    char tokenBuf[128];
    bool enabled;
    // Committed config — mirrors d.cfg.McpAuthToken / d.cfg.McpEnabled.
    std::string cfgToken;
    bool cfgEnabled;
    // Observable side effect — stands in for PluginHost::SyncMcpPluginWithConfig,
    // i.e. one MCP listen-socket teardown + rebind per increment.
    int syncCalls;
    bool tokenEditing;
};

CommitGateState g_gateState;

void ResetCommitGateState() {
    std::memset(g_gateState.tokenBuf, 0, sizeof(g_gateState.tokenBuf));
    g_gateState.enabled = false;
    g_gateState.cfgToken.clear();
    g_gateState.cfgEnabled = false;
    g_gateState.syncCalls = 0;
    g_gateState.tokenEditing = false;
}

// Replica of SmatchetPreferencesUi.cpp:920-951. Checkboxes are one event per
// change and stay ungated; the token field gates BOTH the comparison and the
// assignment on !editing.
void ApplyCommitGateReplica(CommitGateState& s) {
    const std::string tokenBufStr(s.tokenBuf);
    const bool tokenCommittable = !s.tokenEditing;
    const bool dirty = (s.enabled != s.cfgEnabled) || (tokenCommittable && tokenBufStr != s.cfgToken);
    if (dirty) {
        s.cfgEnabled = s.enabled;
        if (tokenCommittable) {
            s.cfgToken = tokenBufStr;
        }
        ++s.syncCalls;
    }
}

void RegisterCommitGateOneSyncPerBurstVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "McpPrefs", "CommitGate_OneSyncPerBurst");
    t->UserData = &g_gateState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        CommitGateState* s = static_cast<CommitGateState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(520, 160), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::McpCommitGate", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Checkbox("Enable MCP server##McpGate", &s->enabled);
            s->tokenEditing = false;
            SmatchetSecretInputText("##McpGateToken", s->tokenBuf, sizeof(s->tokenBuf), &s->tokenEditing);
            ImGui::Button("Sink##McpCommitGate");
            ApplyCommitGateReplica(*s);
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        CommitGateState* s = static_cast<CommitGateState*>(ctx->Test->UserData);
        ResetCommitGateState();
        ctx->SetRef("SmatchetTest::McpCommitGate");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(s->syncCalls, 0); // buffers already match cfg — no spurious sync

        // A typing burst. The pre-#2110 code produced one sync PER CHARACTER and wrote
        // every prefix ("s", "se", "sec", ...) into cfg.
        ctx->ItemClick("##McpGateToken");
        ctx->KeyChars("secret");
        ctx->Yield();
        IM_CHECK(s->tokenEditing);
        IM_CHECK_EQ(s->syncCalls, 0);
        IM_CHECK_STR_EQ(s->cfgToken.c_str(), ""); // no prefix reached the config

        // Focus loss commits once.
        ctx->ItemClick("Sink##McpCommitGate");
        ctx->Yield();
        IM_CHECK(!s->tokenEditing);
        IM_CHECK_EQ(s->syncCalls, 1);
        IM_CHECK_STR_EQ(s->cfgToken.c_str(), "secret");

        // Idle frames must not re-sync — the diff is settled.
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(s->syncCalls, 1);

        // The ASSIGNMENT is gated independently of the comparison. Type a prefix, leave
        // the field active, then change the checkbox state WITHOUT stealing focus — the
        // production analogue is another writer moving d.cfg.McpEnabled (a `config.set`
        // over CLI / MCP / Lua, or the MCP server panel's own toggle) while Preferences
        // is open and the token field is mid-edit. The checkbox term trips `dirty`, but
        // the gated assignment keeps the half-typed token out of cfg. Gating only the
        // comparison — the tempting simplification — flushes "secretXY" right here.
        //
        // Note this is deliberately NOT driven by clicking the checkbox: a click
        // deactivates the token field on the same frame, which legitimately commits it.
        ctx->ItemClick("##McpGateToken");
        ctx->KeyChars("XY");
        ctx->Yield();
        IM_CHECK_EQ(s->syncCalls, 1);
        s->enabled = true; // not a click: the token field keeps keyboard focus
        ctx->Yield();
        ctx->Yield(); // second frame proves the diff settles instead of re-syncing
        IM_CHECK(s->tokenEditing);
        IM_CHECK_EQ(s->syncCalls, 2);
        IM_CHECK(s->cfgEnabled);
        IM_CHECK_STR_EQ(s->cfgToken.c_str(), "secret"); // "secretXY" is still being typed

        // Releasing focus commits the full value — once.
        ctx->ItemClick("Sink##McpCommitGate");
        ctx->Yield();
        IM_CHECK_EQ(s->syncCalls, 3);
        IM_CHECK_STR_EQ(s->cfgToken.c_str(), "secretXY");
    };
}

} // namespace

extern "C" void SmatchetRegisterMcpPrefsCommitGateTests(ImGuiTestEngine* engine) {
    RegisterSecretInputEditingStateVariant(engine);
    RegisterCommitGateOneSyncPerBurstVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_MCP
