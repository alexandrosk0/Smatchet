#include "LuaConsolePlugin.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char* kRunLua = "RunLua.lua";

void SaveLuaLayoutDebounced(float scriptPaneHeightPx) {
    static std::chrono::steady_clock::time_point s_lastWrite{};
    static float s_lastSaved = -1.0f;
    const auto now = std::chrono::steady_clock::now();
    if (s_lastSaved >= 0.0f && std::fabs(scriptPaneHeightPx - s_lastSaved) <= 0.75f &&
        now - s_lastWrite < std::chrono::milliseconds(350)) {
        return;
    }
    if (s_lastSaved >= 0.0f && std::fabs(scriptPaneHeightPx - s_lastSaved) <= 0.75f) {
        return;
    }
    nlohmann::json j = ConfigManager::LoadMergedConfigJson();
    j["lua_scripts_panel_height_px"] = scriptPaneHeightPx;
    ConfigManager::WriteConfigJson(j);
    s_lastSaved = scriptPaneHeightPx;
    s_lastWrite = now;
}

void ReloadSmatchetHooksSetupScript(AppController& app) {
    app.RunLuaSetupScript("SmatchetHooks.lua");
}

bool ReadFileAll(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileAll(const std::string& path, const std::string& content, std::string& err) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) {
        err = "Could not write: " + path;
        return false;
    }
    o << content;
    return true;
}

static std::vector<std::string> BuildAutocompleteCandidates(const std::vector<std::string>& scriptNames) {
    std::vector<std::string> out;
    const TextEditor::LanguageDefinition& def = TextEditor::LanguageDefinition::Lua();
    for (const auto& kw : def.mKeywords) {
        if (!kw.empty()) {
            out.push_back(kw);
        }
    }
    static const char* kApi[] = {"smatchet", "tracker", "ui", "log_info", "create_issue", "process_ticket", "ticket",
                                 "register_global_action", "require", "pairs", "ipairs", "type", "tostring", "tonumber",
                                 "string", "table", "math", "os", "io", "error", "pcall", "xpcall"};
    for (const char* s : kApi) {
        out.emplace_back(s);
    }
    for (const auto& s : scriptNames) {
        out.push_back(s);
    }
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

bool LuaConsolePlugin::IsHooksFile(const std::string& rel) {
    return rel == "SmatchetHooks.lua";
}

bool LuaConsolePlugin::IsRunLuaFile(const std::string& rel) {
    return rel == kRunLua;
}

int LuaConsolePlugin::TryParseLuaErrorLine(const std::string& err) {
    try {
        std::regex re(R"((?:^|\n)[^\n:]{0,400}:(\d+):)");
        std::smatch m;
        if (std::regex_search(err, m, re)) {
            return std::stoi(m[1].str());
        }
    } catch (...) {
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
    static const char* kExtra[] = {"smatchet", "tracker", "ui", "log_info", "create_issue", "process_ticket", "ticket",
                                   "register_global_action", "imgui", "require"};
    for (const char* s : kExtra) {
        luaLangWithApi_.mIdentifiers[std::string(s)] = id;
    }
    luaEditor_.SetLanguageDefinition(luaLangWithApi_);
    luaEditor_.SetPalette(TextEditor::GetDarkPalette());
    luaEditor_.SetTabSize(4);
    luaLangReady_ = true;
}

void LuaConsolePlugin::RefreshScriptList(AppController& app, bool forceRescan) {
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
    if (selectedScriptIndex_ < 0 || selectedScriptIndex_ >= static_cast<int>(scriptList_.size())) {
        selectedScriptIndex_ = 0;
    }
}

bool LuaConsolePlugin::LoadSelectedScriptIntoEditor(AppController& app, std::string& outErr) {
    if (scriptList_.empty()) {
        outErr = "No script files.";
        return false;
    }
    const std::string& name = scriptList_[static_cast<size_t>(selectedScriptIndex_)];
    const std::string path = app.ResolveLuaScriptPath(name);
    if (path.empty()) {
        outErr = "Invalid script path.";
        return false;
    }
    std::string content;
    if (!ReadFileAll(path, content)) {
        content = "-- New file\n";
    }
    luaEditor_.SetText(content);
    // SetText() strips '\r'; compare dirty state against editor text, not raw file bytes.
    diskSnapshot_ = luaEditor_.GetText();
    ClearErrorMarkers();
    (void)outErr;
    return true;
}

bool LuaConsolePlugin::SaveCurrentScript(AppController& app, std::string& outErr) {
    if (scriptList_.empty()) {
        return false;
    }
    const std::string& name = scriptList_[static_cast<size_t>(selectedScriptIndex_)];
    const std::string path = app.ResolveLuaScriptPath(name);
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

void LuaConsolePlugin::ClearErrorMarkers() {
    luaEditor_.SetErrorMarkers(TextEditor::ErrorMarkers());
}

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

void LuaConsolePlugin::OnEarlyInit(AppController& app) {
    EnsureLuaLanguageDef();
    app.AddAutomationLogSink([this](const std::string& msg) { console_.AddLog(std::string("[LUA] ") + msg); });
    const std::string autoPath = app.ResolveLuaScriptPath("Automation.lua");
    const std::string hooksPath = app.ResolveLuaScriptPath("SmatchetHooks.lua");
    const auto fileReadable = [](const std::string& p) -> bool {
        if (p.empty()) {
            return false;
        }
        std::ifstream f(p, std::ios::binary);
        return f.is_open();
    };
    LOG_INFO("LuaConsolePlugin: Automation.lua resolved=\"%s\" readable=%s", autoPath.c_str(),
             fileReadable(autoPath) ? "yes" : "no");
    LOG_INFO("LuaConsolePlugin: SmatchetHooks.lua resolved=\"%s\" readable=%s", hooksPath.c_str(),
             fileReadable(hooksPath) ? "yes" : "no");

    RefreshScriptList(app, true);
    auto it = std::find(scriptList_.begin(), scriptList_.end(), std::string("Automation.lua"));
    if (it != scriptList_.end()) {
        selectedScriptIndex_ = static_cast<int>(it - scriptList_.begin());
    } else {
        selectedScriptIndex_ = 0;
    }
    std::string err;
    (void)LoadSelectedScriptIntoEditor(app, err);
    {
        const nlohmann::json j = ConfigManager::LoadMergedConfigJson();
        const float loadedHeight = static_cast<float>(j.value("lua_scripts_panel_height_px", 0.0));
        if (loadedHeight > 0.0f) {
            luaAutomationScriptPaneUserPx_ = loadedHeight;
        }
    }
}

void LuaConsolePlugin::OnDraw(AppController& app) {
    if (!g_ui.showLuaAutomationWindow) {
        onScriptsTabLastFrame_ = false;
        app.DrawLuaWindows();
        return;
    }

    const bool wantFocus = g_ui.requestLuaAutomationFocus;
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }

    ImGui::SetNextWindowSize(ImVec2(650, 720), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));

    if (!ImGui::Begin("Scripting", &g_ui.showLuaAutomationWindow)) {
        onScriptsTabLastFrame_ = false;
        ImGui::End();
        ImGui::PopStyleVar();
        if (wantFocus) {
            g_ui.requestLuaAutomationFocus = false;
        }
        return;
    }

    if (wantFocus) {
        ImGui::SetWindowFocus();
        g_ui.requestLuaAutomationFocus = false;
    }

    static int s_tabSel = 0;
    static bool s_pendingSelectScriptsTab = false;
    if (g_ui.requestScriptingEditorTabFocus) {
        s_tabSel = 0;
        s_pendingSelectScriptsTab = true;
        g_ui.requestScriptingEditorTabFocus = false;
    }

    constexpr float kSplitGrabH = 6.0f;
    constexpr float kMinLogH = 72.0f;
    constexpr float kMinScriptH = 80.0f;
    constexpr float kToolsLogH = 56.0f;
    const float splitStackTotalH = ImGui::GetContentRegionAvail().y;

    const bool onToolsTab = (s_tabSel == 1);
    if (onToolsTab) {
        if (!onToolsTabLastFrame_) {
            scriptPaneHeightBeforeToolsTab_ = luaAutomationScriptPaneUserPx_;
            if (luaAutomationScriptPaneUserPx_ <= 0.0f) {
                luaAutomationScriptPaneUserPx_ =
                    (std::max)(kMinScriptH, splitStackTotalH - kToolsLogH - kSplitGrabH);
            }
        }
        onToolsTabLastFrame_ = true;
    } else {
        if (onToolsTabLastFrame_) {
            luaAutomationScriptPaneUserPx_ = scriptPaneHeightBeforeToolsTab_;
        }
        onToolsTabLastFrame_ = false;
        const float maxScriptHScripts = (std::max)(kMinScriptH, splitStackTotalH - kMinLogH - kSplitGrabH);
        if (luaAutomationScriptPaneUserPx_ <= 0.f) {
            luaAutomationScriptPaneUserPx_ =
                (std::min)(maxScriptHScripts, (std::max)(kMinScriptH, splitStackTotalH * 0.42f));
        }
    }
    const float minLogH = onToolsTab ? kToolsLogH : kMinLogH;
    const float maxScriptH = (std::max)(kMinScriptH, splitStackTotalH - minLogH - kSplitGrabH);
    luaAutomationScriptPaneHeightPx_ =
        (std::min)(maxScriptH, (std::max)(kMinScriptH, luaAutomationScriptPaneUserPx_));

    ImGui::BeginChild("##lua_script_pane", ImVec2(-1.0f, luaAutomationScriptPaneHeightPx_), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("##lua_main_tabs", ImGuiTabBarFlags_None)) {
        ImGuiTabItemFlags scriptFlags = ImGuiTabItemFlags_None;
        if (s_pendingSelectScriptsTab) {
            scriptFlags |= ImGuiTabItemFlags_SetSelected;
            s_pendingSelectScriptsTab = false;
        }
        if (ImGui::BeginTabItem("Scripts", nullptr, scriptFlags)) {
            s_tabSel = 0;
            RefreshScriptList(app);
            onScriptsTabLastFrame_ = true;

            const std::string curName =
                scriptList_.empty() ? std::string("(none)") : scriptList_[static_cast<size_t>(selectedScriptIndex_)];
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("File");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            bool openUnsavedSwitchAfterCombo = false;
            if (ImGui::BeginCombo("##script_pick", curName.c_str())) {
                for (int i = 0; i < static_cast<int>(scriptList_.size()); ++i) {
                    const bool sel = (i == selectedScriptIndex_);
                    if (ImGui::Selectable(scriptList_[static_cast<size_t>(i)].c_str(), sel)) {
                        if (i != selectedScriptIndex_) {
                            if (luaEditor_.GetText() != diskSnapshot_) {
                                pendingScriptIndex_ = i;
                                // OpenPopup from inside BeginCombo is unreliable (modal vs combo popup stack).
                                // Defer until after EndCombo so BeginPopupModal runs same frame after OpenPopup.
                                openUnsavedSwitchAfterCombo = true;
                            } else {
                                selectedScriptIndex_ = i;
                                std::string e;
                                (void)LoadSelectedScriptIntoEditor(app, e);
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
                        selectedScriptIndex_ = pendingScriptIndex_;
                        (void)LoadSelectedScriptIntoEditor(app, e);
                    } else {
                        g_ui.gridEditError = e;
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard", ImVec2(120, 0))) {
                    selectedScriptIndex_ = pendingScriptIndex_;
                    std::string e;
                    (void)LoadSelectedScriptIntoEditor(app, e);
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
                (void)LoadSelectedScriptIntoEditor(app, e);
                g_ui.gridEditSuccess = "Reloaded " + curName;
            }

            const bool isHooks = IsHooksFile(curName);
            const bool isAutomation = (curName == "Automation.lua");
            if (isHooks) {
                ImGui::SameLine();
                if (ImGui::Button("Run", ImVec2(70, 0))) {
                    LOG_TRACE("LuaConsole: reloading SmatchetHooks.lua from disk");
                    ReloadSmatchetHooksSetupScript(app);
                    g_ui.gridEditSuccess = "Hooks reloaded";
                    ClearErrorMarkers();
                }
            } else {
                ImGui::SameLine();
                if (isAutomation) {
                    if (ImGui::Button("Run on selected", ImVec2(150, 0))) {
                        std::string e;
                        if (SaveCurrentScript(app, e)) {
                            std::vector<std::string> selectedIds;
                            const auto snap = app.GetActiveTicketsSnapshot();
                            const auto& tickets = *snap;
                            for (int r : g_ui.gridState.RectSel.Rows) {
                                if (r >= 0 && static_cast<size_t>(r) < g_ui.cachedSortedIndices.size()) {
                                    const size_t originalIdx = g_ui.cachedSortedIndices[static_cast<size_t>(r)];
                                    if (originalIdx < tickets.size()) {
                                        selectedIds.push_back(tickets[originalIdx].id);
                                    }
                                }
                            }
                            if (g_ui.gridState.RectSel.Active) {
                                const int minR = g_ui.gridState.RectSel.MinRow();
                                const int maxR = g_ui.gridState.RectSel.MaxRow();
                                for (int r = minR; r <= maxR; ++r) {
                                    if (r >= 0 && static_cast<size_t>(r) < g_ui.cachedSortedIndices.size()) {
                                        const size_t originalIdx = g_ui.cachedSortedIndices[static_cast<size_t>(r)];
                                        if (originalIdx < tickets.size()) {
                                            selectedIds.push_back(tickets[originalIdx].id);
                                        }
                                    }
                                }
                            }
                            std::sort(selectedIds.begin(), selectedIds.end());
                            selectedIds.erase(std::unique(selectedIds.begin(), selectedIds.end()), selectedIds.end());
                            LOG_TRACE("LuaConsole: Run %s on %zu selected issue(s)", curName.c_str(), selectedIds.size());
                            app.RunAutoScript(curName, selectedIds);
                            ClearErrorMarkers();
                        } else {
                            g_ui.gridEditError = e;
                        }
                    }
                } else {
                    ImGui::SameLine();
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
            }

            ImGui::Spacing();
            const float editorH = (std::max)(80.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing());
            luaEditor_.Render("##lua_text_editor", ImVec2(-1.0f, editorH), false);

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Space)) {
                ImGui::OpenPopup("lua_ac");
            }
            DrawAutocompletePopup();

            ImGui::Spacing();
            ImGui::TextDisabled("Ctrl+Space: autocomplete. Lua errors from Run may mark lines when a :line: is present.");

            ImGui::EndTabItem();
        } else {
            onScriptsTabLastFrame_ = false;
        }

        if (ImGui::BeginTabItem("Tools & Actions")) {
            s_tabSel = 1;
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
                ImGui::TextWrapped("Global actions are defined in SmatchetHooks.lua using 'ui.register_global_action(name, callback)'.");
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
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::InvisibleButton("##lua_script_log_split", ImVec2(ImGui::GetContentRegionAvail().x, kSplitGrabH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float next = luaAutomationScriptPaneHeightPx_ + ImGui::GetIO().MouseDelta.y;
        const float localMinLogH = onToolsTab ? kToolsLogH : kMinLogH;
        const float localMaxScriptH = (std::max)(kMinScriptH, splitStackTotalH - localMinLogH - kSplitGrabH);
        const float clamped = (std::min)(localMaxScriptH, (std::max)(kMinScriptH, next));
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

    ImGui::End();
    ImGui::PopStyleVar();

    app.DrawLuaWindows();
}
