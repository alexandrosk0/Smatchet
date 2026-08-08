#include "LuaConsolePlugin.h"
#include "LuaConsolePlugin_detail.h"
#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
// The prologue below (project headers + SmatchetLocalizedImGui #define + std-library include block) is the shared
// preamble every panel/plugin TU carries — SmatchetAiAssistantUi / SmatchetBulkTicketsUi / SmatchetOfflineQueueUi /
// SmatchetPreferencesUi*. Every edit that lengthens it (<future> for the Issue #1925 async script load, the
// SmatchetWindowExpand pair for the tab-bar toggle) re-flags an already-common run rather than introducing new
// copy-paste, and there is no shared prologue header to factor into without worse coupling — the DRY gate doc
// endorses an exemption over cross-context abstraction. dup_audit._suppressed accepts a marker on the nearest
// non-blank line above the clone start OR anywhere inside the span — this one sits directly above, since the
// token-run start drifts and only that position is stable for a prologue clone that begins at the includes.
// SMATCHET_DEVIATION(rule=duplication; reason=shared panel/plugin TU prologue; owner=orchestrator; revisit=2026-12-01)
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetThemedTextEditorPalette.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui
#include <algorithm>
#include <cctype>
#include <iterator>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <string>
#include <vector>

namespace {

constexpr const char* kRunLua = "RunLua.lua";

void SaveLuaLayoutDebounced(float scriptPaneHeightPx) {
    static std::chrono::steady_clock::time_point s_lastWrite{};
    static float s_lastSaved = -1.0f;
    const auto now = std::chrono::steady_clock::now();
    // Skip writes when the change is negligible (the 350 ms time-check was a redundant guard
    // subsumed by this condition — the second if always fired before the timed one could not).
    if (s_lastSaved >= 0.0f && std::fabs(scriptPaneHeightPx - s_lastSaved) <= 0.75f &&
        now - s_lastWrite < std::chrono::milliseconds(350)) {
        return;
    }
    nlohmann::json j = ConfigManager::LoadMergedConfigJson();
    j["lua_scripts_panel_height_px"] = scriptPaneHeightPx;
    ConfigManager::WriteConfigJson(j);
    s_lastSaved = scriptPaneHeightPx;
    s_lastWrite = now;
}

ImVec2 ClampLuaWindowPos(const ImVec2& pos, const ImVec2& size) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        return pos;
    }
    const float maxX = (std::max)(vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - size.x);
    const float maxY = (std::max)(vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - size.y);
    return ImVec2((std::max)(vp->WorkPos.x, (std::min)(pos.x, maxX)),
                  (std::max)(vp->WorkPos.y, (std::min)(pos.y, maxY)));
}

void PrepareLuaWindowLayout(UiDrawSession& d) {
    // 0 when the bottom panel is gone; docking there would mint an orphan node that only
    // looks docked (EnsureDockSlotAlive). The arm below is left unconsumed in that case so
    // the redock still fires once the node is back.
    const ImGuiID slot = SmatchetDockNodeIds::EnsureDockSlotAlive(SmatchetDockNodeIds::kBottomPanel);
    if (slot != 0) {
        if (d.pendingReDockWindows.erase("scripting") > 0) {
            ImGui::SetNextWindowDockID(slot, ImGuiCond_Always);
        } else if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
            ImGui::SetNextWindowDockID(slot, ImGuiCond_FirstUseEver);
        }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        ImGui::SetNextWindowSize(ImVec2(650.0f, 720.0f), ImGuiCond_FirstUseEver);
        return;
    }
    const ImVec2 size((std::min)(700.0f, (std::max)(520.0f, vp->WorkSize.x * 0.42f)),
                      (std::min)(780.0f, (std::max)(520.0f, vp->WorkSize.y * 0.82f)));
    const ImVec2 pos(vp->WorkPos.x + vp->WorkSize.x - size.x - 24.0f, vp->WorkPos.y + 48.0f);
    ImGui::SetNextWindowPos(ClampLuaWindowPos(pos, size), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
}

void RepairLuaWindowLayout(UiDrawSession& d) {
    // An expanded window is deliberately undocked, and it is pinned fullscreen every frame
    // by SmatchetWindowExpand::BeginWindow. Arming the re-dock latch here would fight that
    // pin and also strand a forced redock that fires after the minimize.
    // The re-dock-latch + off-screen-rect repair below is the same shape every dockable panel carries
    // (RepairMcpWindowLayout, RepairPlanDocWindowLayout, …) with per-window ids and clamps; adding the guard
    // above re-flagged that long-standing run. Consolidation is tracked as the EnsureDockSlotAlive audit item —
    // factoring it here would couple a plugin TU to Core's UI layer for no behavioural gain.
    // SMATCHET_DEVIATION(rule=duplication; reason=per-window dock-repair shape; owner=orchestrator; revisit=2026-12-01)
    if (ImGui::IsWindowDocked() || SmatchetWindowExpand::IsCurrentWindowExpanded(d)) {
        return;
    }
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
        d.pendingReDockWindows.insert("scripting");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        return;
    }
    ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 pos = ImGui::GetWindowPos();
    const bool tooSmall = size.x < 420.0f || size.y < 360.0f;
    const bool offscreen = pos.x > vp->WorkPos.x + vp->WorkSize.x - 96.0f ||
                           pos.y > vp->WorkPos.y + vp->WorkSize.y - 72.0f || pos.x + size.x < vp->WorkPos.x + 96.0f ||
                           pos.y + size.y < vp->WorkPos.y + 72.0f;
    if (!tooSmall && !offscreen) {
        return;
    }
    size.x = (std::min)((std::max)(size.x, 520.0f), vp->WorkSize.x * 0.92f);
    size.y = (std::min)((std::max)(size.y, 520.0f), vp->WorkSize.y * 0.88f);
    const ImVec2 fallback(vp->WorkPos.x + vp->WorkSize.x - size.x - 24.0f, vp->WorkPos.y + 48.0f);
    ImGui::SetWindowPos(ClampLuaWindowPos(fallback, size), ImGuiCond_Always);
    ImGui::SetWindowSize(size, ImGuiCond_Always);
}

void ReloadSmatchetHooksSetupScript(AppController& app) { app.RunLuaSetupScript("SmatchetHooks.lua"); }

// Pillar 2 (Issue #1925): runs on the std::async worker owned by LuaConsolePlugin::scriptLoadFuture_,
// never on the UI thread. Scripts are small (typically < 100 KB) and the read is sub-ms typical, but
// the LATENCY of a single open/read is unbounded on a network share or an antivirus-filtered path —
// which is the reason it had to leave the render thread regardless of size.
LuaScriptReadStatus ReadFileAll(const std::string& path, std::string& out) {
    // Same 8 MiB ceiling AppController_LuaScriptFiles.cpp applies to script loads (SECURITY_AUDIT
    // #33 class — no unbounded slurp). Sizing off seekg/tellg and reading into an exactly-sized
    // buffer, rather than draining rdbuf() into a stringstream, is what makes the cap load-bearing:
    // the stringstream form allocates the whole file before anything can check its size.
    constexpr std::streamoff kMaxLuaScriptBytes = 8ll * 1024 * 1024;
    /* PILLAR2_WORKER_ONLY */ // est-latency: ~1ms — script read on the std::async worker owned by
                              // LuaConsolePlugin::scriptLoadFuture_; no frame budget applies.
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return LuaScriptReadStatus::NotOpenable;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) {
        return LuaScriptReadStatus::NotOpenable;
    }
    if (size > kMaxLuaScriptBytes) {
        return LuaScriptReadStatus::TooLarge;
    }
    f.seekg(0, std::ios::beg);
    out.assign(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !f.read(&out[0], size)) {
        return LuaScriptReadStatus::NotOpenable;
    }
    return LuaScriptReadStatus::Ok;
}

bool WriteFileAll(const std::string& path, const std::string& content, std::string& err) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) {
        err = "Could not write: " + path;
        return false;
    }
    o << content;
    if (!o.good()) {
        err = "Write failed (disk full or I/O error): " + path;
        return false;
    }
    return true;
}

static std::vector<std::string> BuildAutocompleteCandidates(const std::vector<std::string>& scriptNames) {
    std::vector<std::string> out;
    const TextEditor::LanguageDefinition& def = TextEditor::LanguageDefinition::Lua();
    std::copy_if(def.mKeywords.begin(), def.mKeywords.end(), std::back_inserter(out),
                 [](const std::string& kw) { return !kw.empty(); });
    static const char* kApi[] = {"smatchet",     "tracker",
                                 "ui",           "log_info",
                                 "create_issue", "process_ticket",
                                 "ticket",       "register_global_action",
                                 "require",      "pairs",
                                 "ipairs",       "type",
                                 "tostring",     "tonumber",
                                 "string",       "table",
                                 "math",         "os",
                                 "io",           "error",
                                 "pcall",        "xpcall"};
    std::transform(std::begin(kApi), std::end(kApi), std::back_inserter(out),
                   [](const char* s) { return std::string(s); });
    std::copy(scriptNames.begin(), scriptNames.end(), std::back_inserter(out));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static bool AsciiCaseInsensitivePrefix(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool LuaConsolePlugin::IsHooksFile(const std::string& rel) { return rel == "SmatchetHooks.lua"; }

int LuaConsolePlugin::TryParseLuaErrorLine(const std::string& err) {
    // Hand-parser replaces std::regex_search (§5.2): avoids backtracking stall on malformed input
    // on the UI thread. Lua errors are "<source>:<line>: <message>"; we scan for ":<digits>:".
    const size_t n = err.size();
    for (size_t i = 0; i < n; ++i) {
        if (err[i] == ':') {
            size_t j = i + 1;
            if (j >= n || err[j] < '0' || err[j] > '9')
                continue;
            size_t k = j;
            while (k < n && err[k] >= '0' && err[k] <= '9')
                ++k;
            if (k < n && err[k] == ':' && k > j) {
                try {
                    return std::stoi(err.substr(j, k - j));
                } catch (...) { // catch-all-ok: stoi on untrusted error string
                }
            }
        }
    }
    return -1;
}

void LuaConsolePlugin::EnsureLuaLanguageDef() {
    if (luaLangReady_) {
        return;
    }
    luaLangWithApi_ = TextEditor::LanguageDefinition::Lua();
    TextEditor::Identifier id;
    id.mDeclaration = "Smatchet / Lua API";
    static const char* kExtra[] = {"smatchet",     "tracker",        "ui",     "log_info",
                                   "create_issue", "process_ticket", "ticket", "register_global_action",
                                   "imgui",        "require"};
    for (const char* s : kExtra) {
        luaLangWithApi_.mIdentifiers[std::string(s)] = id;
    }
    luaEditor_.SetLanguageDefinition(luaLangWithApi_);
    // Initial palette — Render() re-applies every frame so the live SmatchetTheme
    // tracks. See SmatchetThemedTextEditorPalette.h for why we don't use
    // TextEditor::GetDarkPalette() directly (it's a static-const palette and
    // does not follow theme switches).
    luaEditor_.SetPalette(SmatchetTheme::GetThemedLuaConsolePalette());
    luaEditor_.SetTabSize(4);
    luaLangReady_ = true;
}

void LuaConsolePlugin::RefreshScriptList(const AppController& app, bool forceRescan) {
    const double now = ImGui::GetTime();
    if (!forceRescan) {
        constexpr double kMinIntervalSec = 0.35;
        if ((now - lastScriptListRefreshImGuiTime_) < kMinIntervalSec) {
            return;
        }
    }
    lastScriptListRefreshImGuiTime_ = now;

    scriptList_ = app.ListLuaScriptFiles();
    static const char* kEnsure[] = {"Automation.lua", "SmatchetHooks.lua", kRunLua};
    for (const char* n : kEnsure) {
        const std::string name(n);
        if (std::find(scriptList_.begin(), scriptList_.end(), name) == scriptList_.end()) {
            scriptList_.push_back(name);
        }
    }
    std::sort(scriptList_.begin(), scriptList_.end());
    scriptList_.erase(std::unique(scriptList_.begin(), scriptList_.end()), scriptList_.end());
    SyncSelectionToList();
}

void LuaConsolePlugin::SyncSelectionToList() {
    if (lua_console_detail::ResolveSelectedScriptIndex(scriptList_, selectedScriptName_) >= 0) {
        return;
    }
    if (selectedScriptName_.empty() && !scriptList_.empty()) {
        selectedScriptName_ = scriptList_.front();
        return;
    }
    selectedScriptName_.clear();
}

void LuaConsolePlugin::KickScriptLoad(const AppController& app, const std::string& name) {
    // ResolveLuaScriptPath is a pure string operation (traversal rejection + directory join) —
    // no filesystem hit — so resolving on the UI thread before the launch is Pillar-2 safe.
    const std::string path = app.ResolveLuaScriptPath(name);
    // A fresh read supersedes any earlier refusal; only an outcome that lands can re-raise it.
    scriptLoadRefusedReason_.clear();
    if (path.empty()) {
        // Caller already blanked the editor, so a load that never starts must still block Save —
        // otherwise the blank buffer writes over whatever the name resolves to later.
        scriptLoadRefusedReason_ = "Could not resolve the script path — reselect the script.";
        return;
    }
    std::future<ScriptLoadResult> pending;
    try {
        pending = std::async(std::launch::async, [name, path]() {
            ScriptLoadResult result;
            result.name = name;
            result.status = ReadFileAll(path, result.content);
            return result;
        });
    } catch (const std::exception& ex) {
        // A launch can throw before any future exists — system_error when threads are exhausted,
        // or bad_alloc. Latching scriptLoadInFlight_ ahead of the launch would be unrecoverable:
        // the future stays invalid, so PollScriptLoad early-outs and nothing ever clears the flag,
        // leaving Save blocked for the rest of the session.
        LOG_ERROR("Lua script load could not start: %s", ex.what());
        g_ui.gridEditError = "Could not start reading the script.";
        scriptLoadRefusedReason_ = "The script never loaded — reselect it before saving.";
        reloadNoticeName_.clear();
        return;
    }
    // Both flag and future are set only once the launch has succeeded.
    scriptLoadFuture_ = std::move(pending);
    scriptLoadInFlight_ = true;
}

bool LuaConsolePlugin::StartLoadSelectedScriptIntoEditor(const AppController& app, std::string& outErr) {
    if (selectedScriptName_.empty()) {
        outErr = "No script selected.";
        return false;
    }
    const std::string path = app.ResolveLuaScriptPath(selectedScriptName_);
    if (path.empty()) {
        outErr = "Invalid script path.";
        return false;
    }
    // Blank the editor for the in-flight window and re-baseline the snapshot with it. Leaving the
    // previous file's text visible would be worse than a blank pane (it looks like the new script's
    // content), and leaving diskSnapshot_ stale would make the dirty check fire a spurious
    // unsaved-switch modal against a buffer that is about to be replaced anyway.
    luaEditor_.SetText(std::string());
    // SetText() strips '\r'; compare dirty state against editor text, not raw file bytes.
    diskSnapshot_ = luaEditor_.GetText();
    ClearErrorMarkers();
    if (scriptLoadInFlight_) {
        // Do NOT re-assign scriptLoadFuture_ here — for a std::async future operator= blocks until
        // the old task completes. Queue the name; PollScriptLoad re-kicks once the stale read lands.
        queuedLoadName_ = selectedScriptName_;
        return true;
    }
    KickScriptLoad(app, selectedScriptName_);
    (void)outErr;
    return true;
}

void LuaConsolePlugin::PollScriptLoad(const AppController& app) {
    if (!scriptLoadInFlight_ || !scriptLoadFuture_.valid()) {
        return;
    }
    if (scriptLoadFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    // Clear the flag BEFORE get(): the future is consumed either way, so an exception thrown by
    // the worker and rethrown here must not leave scriptLoadInFlight_ latched true — that would
    // wedge Save behind a load that can never land, for the rest of the session.
    scriptLoadInFlight_ = false;
    ScriptLoadResult result;
    try {
        result = scriptLoadFuture_.get();
    } catch (const std::exception& ex) {
        // Not an empty catch: a worker throw (bad_alloc on the sized buffer, an ifstream
        // exception) is reported and the load abandoned. queuedLoadName_ is dropped with it —
        // re-selecting the script kicks a fresh read.
        LOG_ERROR("Lua script load failed: %s", ex.what());
        g_ui.gridEditError = "Could not read the script.";
        // The editor is blank and diskSnapshot_ was re-baselined onto that blank — exactly the
        // TooLarge hazard, so Save has to be gated here too. Without this the next Save writes the
        // empty buffer over a script that is intact on disk.
        scriptLoadRefusedReason_ = "The script could not be read — reselect it before saving.";
        queuedLoadName_.clear();
        reloadNoticeName_.clear();
        return;
    }
    if (reloadNoticeName_ != selectedScriptName_) {
        // The user moved off the script they asked to reload — its banner is moot. Checked
        // before the branches below so a superseded reload cannot report against a later load.
        reloadNoticeName_.clear();
    }
    if (!queuedLoadName_.empty()) {
        const std::string next = queuedLoadName_;
        queuedLoadName_.clear();
        KickScriptLoad(app, next); // selection moved mid-read — this body is stale
        return;
    }
    if (result.name != selectedScriptName_) {
        return; // selection cleared or retargeted without a queued kick
    }
    if (result.status == LuaScriptReadStatus::TooLarge) {
        // Deliberately NOT the "-- New file" stub: the file exists and has content, so stubbing it
        // would re-baseline diskSnapshot_ on the placeholder and the next Save would truncate the
        // real script. Leave the editor blank and gate Save instead.
        scriptLoadRefusedReason_ = "Script is too large to edit here — open it in an external editor.";
        g_ui.gridEditError = "Script too large to open in the editor (8 MiB cap): " + result.name;
        reloadNoticeName_.clear();
        return;
    }
    luaEditor_.SetText(result.status == LuaScriptReadStatus::Ok ? result.content : std::string("-- New file\n"));
    diskSnapshot_ = luaEditor_.GetText();
    ClearErrorMarkers();
    if (!reloadNoticeName_.empty() && reloadNoticeName_ == result.name) {
        // Only an explicit "Reload from disk" reports an outcome. A read failure is silent for
        // every other path because the "-- New file" stub above is the intended result there
        // (picking a name with no file on disk starts a new script) — but the user who pressed
        // Reload asked about a file that is supposed to exist, so a failure is an error to them.
        reloadNoticeName_.clear();
        if (result.status == LuaScriptReadStatus::Ok) {
            g_ui.gridEditSuccess = "Reloaded " + result.name;
            g_ui.gridEditError.clear();
        } else {
            g_ui.gridEditError = "Could not read " + result.name;
        }
    }
}

bool LuaConsolePlugin::SaveCurrentScript(const AppController& app, std::string& outErr) {
    if (selectedScriptName_.empty()) {
        outErr = "No script selected.";
        return false;
    }
    if (scriptLoadInFlight_) {
        // The editor is blanked while the async load is in flight (Issue #1925) — writing it now
        // would truncate the file on disk. Every Run path saves first, so this one guard covers
        // Save, Run, and the unsaved-switch modal.
        outErr = "Still loading the script — try again in a moment.";
        return false;
    }
    if (!scriptLoadRefusedReason_.empty()) {
        // Same hazard as the in-flight case, but persistent: the editor is blank because the read
        // was refused or never landed, not because the file is empty. Writing the buffer back would
        // truncate a real script. The reason string carries the remedy for whichever cause it was.
        outErr = scriptLoadRefusedReason_;
        return false;
    }
    // Target the tracked script by identity, never by list index: RefreshScriptList
    // re-sorts every ~0.35s and an index would point at whichever file now sits
    // there, silently clobbering it (DR11).
    const std::string path = app.ResolveLuaScriptPath(selectedScriptName_);
    if (path.empty()) {
        outErr = "Invalid path.";
        return false;
    }
    const std::string body = luaEditor_.GetText();
    if (!WriteFileAll(path, body, outErr)) {
        return false;
    }
    diskSnapshot_ = body;
    RefreshScriptList(app, true);
    return true;
}

void LuaConsolePlugin::ApplyErrorMarkersFromMessage(const std::string& errMsg) {
    const int line = TryParseLuaErrorLine(errMsg);
    TextEditor::ErrorMarkers markers;
    if (line > 0) {
        markers[line] = errMsg;
    }
    luaEditor_.SetErrorMarkers(markers);
}

void LuaConsolePlugin::ClearErrorMarkers() { luaEditor_.SetErrorMarkers(TextEditor::ErrorMarkers()); }

void LuaConsolePlugin::DrawAutocompletePopup() {
    if (!ImGui::BeginPopup("lua_ac")) {
        return;
    }
    const std::string prefix = luaEditor_.GetWordUnderCursor();
    const std::vector<std::string> candidates = BuildAutocompleteCandidates(scriptList_);
    int shown = 0;
    for (const auto& c : candidates) {
        if (!prefix.empty() && !AsciiCaseInsensitivePrefix(c, prefix)) {
            continue;
        }
        ImGui::PushID(shown);
        if (ImGui::Selectable(c.c_str())) {
            luaEditor_.SelectWordUnderCursor();
            luaEditor_.DeleteSelection();
            luaEditor_.InsertText(c);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        ++shown;
    }
    if (shown == 0) {
        ImGui::TextDisabled("(no matches)");
    }
    ImGui::EndPopup();
}

LuaConsolePlugin::~LuaConsolePlugin() {
    if (!scriptLoadFuture_.valid()) {
        return;
    }
    // The member's own destructor would block here anyway (std::async shared state joins on
    // destruction) — draining explicitly turns that into a wait we can explain. A script read is
    // one bounded file slurp, so the grace period is generous; exceeding it means the path is
    // slow/remote/AV-filtered, and the log line is what turns an unexplained teardown hang into a
    // diagnosable one. The wait is not skippable: the task borrows nothing from `this`, but its
    // shared state must be released before the future member goes away.
    if (scriptLoadFuture_.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        LOG_WARN("LuaConsole: script read still running at plugin teardown; blocking until it completes");
    }
}

void LuaConsolePlugin::OnEarlyInit(AppController& app) {
    EnsureLuaLanguageDef();
    app.AddAutomationLogSink([this](const std::string& msg) { console_.AddLog(std::string("[LUA] ") + msg); });
    // Dedicated error sink: adds to the persistent error panel and auto-scrolls.
    app.AddAutomationErrorSink([this](const std::string& msg) { console_.AddError(msg); });
    const std::string autoPath = app.ResolveLuaScriptPath("Automation.lua");
    const std::string hooksPath = app.ResolveLuaScriptPath("SmatchetHooks.lua");
    const auto fileReadable = [](const std::string& p) -> bool {
        if (p.empty()) {
            return false;
        }
        /* PILLAR2_WORKER_ONLY */ // est-latency: ~1ms — plugin Initialize() runs at startup, not in the render loop; no
                                  // frame budget applies.
        std::ifstream f(p, std::ios::binary);
        return f.is_open();
    };
    LOG_INFO("LuaConsolePlugin: Automation.lua resolved=\"%s\" readable=%s", autoPath.c_str(),
             fileReadable(autoPath) ? "yes" : "no");
    LOG_INFO("LuaConsolePlugin: SmatchetHooks.lua resolved=\"%s\" readable=%s", hooksPath.c_str(),
             fileReadable(hooksPath) ? "yes" : "no");

    RefreshScriptList(app, true);
    selectedScriptName_ = "Automation.lua";
    SyncSelectionToList();
    std::string err;
    // Kick only — the read lands on a later frame via PollScriptLoad at the top of OnDraw.
    (void)StartLoadSelectedScriptIntoEditor(app, err);
    {
        const nlohmann::json j = ConfigManager::LoadMergedConfigJson();
        const float loadedHeight = static_cast<float>(j.value("lua_scripts_panel_height_px", 0.0));
        if (loadedHeight > 0.0f) {
            luaAutomationScriptPaneUserPx_ = loadedHeight;
        }
    }
}

bool LuaConsolePlugin::BeginLuaWindow(bool wantFocus) {
    PrepareLuaWindowLayout(g_ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));

    SmatchetWindowExpand::BeginWindow(g_ui, "Scripting");
    if (!ImGui::Begin("Scripting", &g_ui.showLuaAutomationWindow)) {
        onScriptsTabLastFrame_ = false;
        ImGui::End();
        ImGui::PopStyleVar();
        if (wantFocus) {
            g_ui.requestLuaAutomationFocus = false;
        }
        return false;
    }

    SmatchetWindowExpand::DrawToggle(g_ui);
    if (wantFocus) {
        ImGui::SetWindowFocus();
        g_ui.requestLuaAutomationFocus = false;
    }
    RepairLuaWindowLayout(g_ui);

    if (g_ui.requestScriptingEditorTabFocus) {
        tabSel_ = 0;
        pendingSelectScriptsTab_ = true;
        g_ui.requestScriptingEditorTabFocus = false;
    }
    return true;
}

void LuaConsolePlugin::UpdateSplitLayout(DrawCtx& ctx) {
    if (ctx.onToolsTab) {
        if (!onToolsTabLastFrame_) {
            scriptPaneHeightBeforeToolsTab_ = luaAutomationScriptPaneUserPx_;
            if (luaAutomationScriptPaneUserPx_ <= 0.0f) {
                luaAutomationScriptPaneUserPx_ =
                    (std::max)(ctx.kMinScriptH, ctx.splitStackTotalH - ctx.kToolsLogH - ctx.kSplitGrabH);
            }
        }
        onToolsTabLastFrame_ = true;
    } else {
        if (onToolsTabLastFrame_) {
            luaAutomationScriptPaneUserPx_ = scriptPaneHeightBeforeToolsTab_;
        }
        onToolsTabLastFrame_ = false;
        const float maxScriptHScripts =
            (std::max)(ctx.kMinScriptH, ctx.splitStackTotalH - ctx.kMinLogH - ctx.kSplitGrabH);
        if (luaAutomationScriptPaneUserPx_ <= 0.f) {
            luaAutomationScriptPaneUserPx_ =
                (std::min)(maxScriptHScripts, (std::max)(ctx.kMinScriptH, ctx.splitStackTotalH * 0.42f));
        }
    }
    const float minLogH = ctx.onToolsTab ? ctx.kToolsLogH : ctx.kMinLogH;
    const float maxScriptH = (std::max)(ctx.kMinScriptH, ctx.splitStackTotalH - minLogH - ctx.kSplitGrabH);
    luaAutomationScriptPaneHeightPx_ =
        (std::min)(maxScriptH, (std::max)(ctx.kMinScriptH, luaAutomationScriptPaneUserPx_));
}

void LuaConsolePlugin::DrawRunButtonRow(DrawCtx& ctx, const std::string& curName) {
    AppController& app = ctx.app;
    const bool isHooks = IsHooksFile(curName);
    const bool isAutomation = (curName == "Automation.lua");
    if (isHooks) {
        ImGui::SameLine();
        if (ImGui::Button("Run", ImVec2(70, 0))) {
            std::string e;
            if (SaveCurrentScript(app, e)) {
                LOG_TRACE("LuaConsole: reloading SmatchetHooks.lua from disk");
                ReloadSmatchetHooksSetupScript(app);
                g_ui.gridEditSuccess = "Hooks reloaded";
                ClearErrorMarkers();
            } else {
                g_ui.gridEditError = e;
            }
        }
        return;
    }
    ImGui::SameLine();
    if (isAutomation) {
        if (ImGui::Button("Run on selected", ImVec2(150, 0))) {
            std::string e;
            if (SaveCurrentScript(app, e)) {
                const auto snap = app.GetActiveTicketsSnapshot();
                const auto& tickets = *snap;
                std::vector<std::string> ticketIds;
                ticketIds.reserve(tickets.size());
                for (const auto& t : tickets) {
                    ticketIds.push_back(t.id);
                }
                // Lua scripts run on the FOCUSED pane's selection (ADR-0018 focused-pane semantics).
                const GridPane& focusedPane = g_ui.focusedPane();
                const std::vector<std::string> selectedIds = lua_console_detail::CollectSelectedTicketIds(
                    focusedPane.gridState.RectSel.Rows, focusedPane.gridState.RectSel.Active,
                    focusedPane.gridState.RectSel.MinRow(), focusedPane.gridState.RectSel.MaxRow(),
                    focusedPane.cachedSortedIndices, ticketIds);
                LOG_TRACE("LuaConsole: Run %s on %zu selected issue(s)", curName.c_str(), selectedIds.size());
                app.RunAutoScript(curName, selectedIds);
                ClearErrorMarkers();
            } else {
                g_ui.gridEditError = e;
            }
        }
        return;
    }
    ImGui::Checkbox("Background", &runInBackground_);
    ImGui::SameLine();
    if (ImGui::Button("Run", ImVec2(70, 0))) {
        std::string e;
        if (SaveCurrentScript(app, e)) {
            if (runInBackground_) {
                console_.AddLog("[RUN] Submitting " + curName + " for background execution...");
                app.RunFlatScriptAsync(curName);
                g_ui.gridEditSuccess = "Submitted background job for " + curName;
                ClearErrorMarkers();
            } else {
                std::string err;
                std::string summary;
                console_.AddLog("[RUN] Executing " + curName + " on main thread...");
                if (!app.ExecuteLuaConsoleSnippet(luaEditor_.GetText(), err, summary)) {
                    console_.AddLog(std::string("[ERROR] ") + err);
                    g_ui.gridEditError = "Lua: " + err;
                    ApplyErrorMarkersFromMessage(err);
                } else {
                    g_ui.gridEditSuccess = curName + " run finished";
                    ClearErrorMarkers();
                    if (!summary.empty()) {
                        console_.AddLog(std::string("[RETURN] ") + summary);
                    } else {
                        console_.AddLog("[RUN] Finished (no return value).");
                    }
                }
            }
        } else {
            g_ui.gridEditError = e;
        }
    }
}

void LuaConsolePlugin::DrawRegisteredActionsRow(DrawCtx& ctx) {
    // Live registered-action row — refreshed every frame from
    // `GetLuaGlobalActionNames()`. Surfaces what user scripts have registered
    // via `ui.register_global_action(...)` without forcing a tab switch to
    // "Tools & Actions".
    const auto liveNames = ctx.app.GetLuaGlobalActionNames();
    if (liveNames.empty()) {
        ImGui::TextDisabled("Registered actions: 0");
    } else {
        const std::string joined = lua_console_detail::JoinRegisteredActionNames(liveNames);
        ImGui::TextDisabled("Registered actions (%zu):", liveNames.size());
        ImGui::TextWrapped("  %s", joined.c_str());
    }
}

void LuaConsolePlugin::DrawEditorAndAutocomplete() {
    ImGui::Spacing();
    const float editorH = (std::max)(80.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing());
    // Refresh palette every frame so a SmatchetTheme switch (driven by
    // SmatchetUI::Draw -> SmatchetTheme::ApplyStyle) propagates instantly.
    // See SmatchetThemedTextEditorPalette.h for the per-frame-cost
    // rationale (~84 bytes copy + 1 SetPalette per frame — negligible).
    luaEditor_.SetPalette(SmatchetTheme::GetThemedLuaConsolePalette());
    luaEditor_.Render("##lua_text_editor", ImVec2(-1.0f, editorH), false);

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Space)) {
        ImGui::OpenPopup("lua_ac");
    }
    DrawAutocompletePopup();

    ImGui::Spacing();
    ImGui::TextDisabled("Ctrl+Space: autocomplete. Lua errors from Run may mark lines when a :line: is present.");
}

void LuaConsolePlugin::DrawScriptsTab(DrawCtx& ctx, const std::string& curName) {
    AppController& app = ctx.app;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("File");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    bool openUnsavedSwitchAfterCombo = false;
    if (ImGui::BeginCombo("##script_pick", curName.c_str())) {
        for (int i = 0; i < static_cast<int>(scriptList_.size()); ++i) {
            const std::string& entry = scriptList_[static_cast<size_t>(i)];
            const bool sel = (entry == selectedScriptName_);
            if (ImGui::Selectable(entry.c_str(), sel)) {
                if (entry != selectedScriptName_) {
                    if (luaEditor_.GetText() != diskSnapshot_) {
                        pendingScriptName_ = entry;
                        // OpenPopup from inside BeginCombo is unreliable (modal vs combo popup stack).
                        // Defer until after EndCombo so BeginPopupModal runs same frame after OpenPopup.
                        openUnsavedSwitchAfterCombo = true;
                    } else {
                        // Restore the previous selection if the load never started: the only false
                        // returns happen before the editor is blanked, so the old script is still
                        // on screen and leaving selectedScriptName_ on the unloadable entry would
                        // point Save at a file the editor never showed.
                        const std::string prev = selectedScriptName_;
                        selectedScriptName_ = entry;
                        std::string e;
                        if (!StartLoadSelectedScriptIntoEditor(app, e)) {
                            selectedScriptName_ = prev;
                            g_ui.gridEditError = e;
                        }
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    if (openUnsavedSwitchAfterCombo) {
        ImGui::OpenPopup("##lua_unsaved_switch");
    }
    if (ImGui::BeginPopupModal("##lua_unsaved_switch", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Save changes to the current script before switching?");
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            std::string e;
            if (SaveCurrentScript(app, e)) {
                const std::string prev = selectedScriptName_;
                selectedScriptName_ = pendingScriptName_;
                if (!StartLoadSelectedScriptIntoEditor(app, e)) {
                    selectedScriptName_ = prev;
                    g_ui.gridEditError = e;
                }
            } else {
                g_ui.gridEditError = e;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(120, 0))) {
            const std::string prev = selectedScriptName_;
            selectedScriptName_ = pendingScriptName_;
            std::string e;
            if (!StartLoadSelectedScriptIntoEditor(app, e)) {
                selectedScriptName_ = prev;
                g_ui.gridEditError = e;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(70, 0))) {
        std::string e;
        if (SaveCurrentScript(app, e)) {
            g_ui.gridEditSuccess = "Saved " + curName;
            g_ui.gridEditError.clear();
        } else {
            g_ui.gridEditError = e;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk", ImVec2(130, 0))) {
        std::string e;
        if (StartLoadSelectedScriptIntoEditor(app, e)) {
            // Nothing is reloaded yet — the read lands on a later frame and can still fail or be
            // superseded. PollScriptLoad reports the outcome against this name.
            reloadNoticeName_ = curName;
            g_ui.gridEditError.clear();
        } else {
            g_ui.gridEditError = e;
        }
    }
    if (scriptLoadInFlight_) {
        // Visible cue for the off-thread read (Pillar 2 cue contract — see Source/Core/src/Ui/AGENTS.md).
        ImGui::SameLine();
        ImGui::TextDisabled("Loading...");
    }

    DrawRunButtonRow(ctx, curName);
    DrawRegisteredActionsRow(ctx);
    DrawEditorAndAutocomplete();
}

void LuaConsolePlugin::DrawToolsTab(DrawCtx& ctx) {
    AppController& app = ctx.app;
    tabSel_ = 1;
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Quick Access Tools");
    ImGui::SameLine();
    ImGui::TextDisabled("(Registered via ui.register_global_action)");
    ImGui::Separator();
    ImGui::Spacing();

    const auto globalNames = app.GetLuaGlobalActionNames();
    if (globalNames.empty()) {
        ImGui::TextDisabled("No global actions registered.");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::TextWrapped("Global actions are defined in SmatchetHooks.lua using "
                           "'ui.register_global_action(name, callback)'.");
        ImGui::PopStyleColor();
    } else {
        for (const auto& name : globalNames) {
            ImGui::PushID(name.c_str());
            if (ImGui::Button(name.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, 32.0f))) {
                app.ExecuteLuaGlobalAction(name);
                g_ui.gridEditSuccess = "Queued: " + name;
            }
            if (ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.48f < ImGui::GetContentRegionMax().x) {
                ImGui::SameLine();
            }
            ImGui::PopID();
        }
    }
}

void LuaConsolePlugin::DrawScriptPane(DrawCtx& ctx) {
    AppController& app = ctx.app;
    ImGui::BeginChild("##lua_script_pane", ImVec2(-1.0f, luaAutomationScriptPaneHeightPx_), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("##lua_main_tabs", ImGuiTabBarFlags_None)) {
        ImGuiTabItemFlags scriptFlags = ImGuiTabItemFlags_None;
        if (pendingSelectScriptsTab_) {
            scriptFlags |= ImGuiTabItemFlags_SetSelected;
            pendingSelectScriptsTab_ = false;
        }
        if (ImGui::BeginTabItem("Scripts", nullptr, scriptFlags)) {
            tabSel_ = 0;
            RefreshScriptList(app);
            onScriptsTabLastFrame_ = true;

            const std::string curName = selectedScriptName_.empty() ? std::string("(none)") : selectedScriptName_;
            DrawScriptsTab(ctx, curName);
            ImGui::EndTabItem();
        } else {
            onScriptsTabLastFrame_ = false;
        }

        if (ImGui::BeginTabItem("Tools & Actions")) {
            DrawToolsTab(ctx);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void LuaConsolePlugin::DrawSplitterAndConsole(DrawCtx& ctx) {
    ImGui::InvisibleButton("##lua_script_log_split", ImVec2(ImGui::GetContentRegionAvail().x, ctx.kSplitGrabH));
    const bool splitHot = ImGui::IsItemHovered();
    const bool splitActive = ImGui::IsItemActive();
    if (splitHot || splitActive) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    {
        // Visible splitter — `InvisibleButton` alone was unfindable, especially when the
        // window was docked short at the bottom and only a 6 px transparent strip remained
        // between editor and log.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const ImU32 lineCol = ImGui::GetColorU32(splitActive ? ImGuiCol_SeparatorActive
                                                 : splitHot  ? ImGuiCol_SeparatorHovered
                                                             : ImGuiCol_Separator);
        const float midY = (rMin.y + rMax.y) * 0.5f;
        dl->AddLine(ImVec2(rMin.x + 4.0f, midY), ImVec2(rMax.x - 4.0f, midY), lineCol, 1.5f);
        const float cx = (rMin.x + rMax.x) * 0.5f;
        const ImU32 dotCol = ImGui::GetColorU32(splitHot || splitActive ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        for (int i = -2; i <= 2; ++i) {
            dl->AddCircleFilled(ImVec2(cx + static_cast<float>(i) * 6.0f, midY), 1.5f, dotCol);
        }
    }
    if (splitActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float next = luaAutomationScriptPaneHeightPx_ + ImGui::GetIO().MouseDelta.y;
        const float localMinLogH = ctx.onToolsTab ? ctx.kToolsLogH : ctx.kMinLogH;
        const float localMaxScriptH =
            (std::max)(ctx.kMinScriptH, ctx.splitStackTotalH - localMinLogH - ctx.kSplitGrabH);
        const float clamped = (std::min)(localMaxScriptH, (std::max)(ctx.kMinScriptH, next));
        if (std::fabs(clamped - luaAutomationScriptPaneHeightPx_) > 0.5f) {
            luaAutomationScriptPaneUserPx_ = clamped;
            luaAutomationScriptPaneHeightPx_ = clamped;
            SaveLuaLayoutDebounced(luaAutomationScriptPaneUserPx_);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    const float logChildH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##console_area", ImVec2(-1, logChildH),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    console_.DrawPanelContents(true, -1.0f, true, false);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void LuaConsolePlugin::OnDraw(AppController& app) {
    // Ahead of the window-hidden early-out below: a read kicked from OnEarlyInit (or while the
    // Scripting window was closed) must still be able to land.
    PollScriptLoad(app);

    // Auto-open + focus the Scripting window when the background automation worker signals
    // a Lua error (set via scriptingWindowOpenRequested_ in AppController).
    if (app.ConsumeScriptingWindowRequest()) {
        g_ui.showLuaAutomationWindow = true;
        g_ui.requestLuaAutomationFocus = true;
    }

    if (!g_ui.showLuaAutomationWindow) {
        onScriptsTabLastFrame_ = false;
        app.DrawLuaWindows();
        return;
    }

    const bool wantFocus = g_ui.requestLuaAutomationFocus;
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }

    if (!BeginLuaWindow(wantFocus)) {
        return;
    }

    DrawCtx ctx{app, (tabSel_ == 1), ImGui::GetContentRegionAvail().y, 6.0f, 72.0f, 80.0f, 56.0f};
    UpdateSplitLayout(ctx);
    DrawScriptPane(ctx);
    DrawSplitterAndConsole(ctx);

    ImGui::End();
    ImGui::PopStyleVar();

    app.DrawLuaWindows();
}
