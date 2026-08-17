// Customizable icon toolbar — render, click dispatch, right-click menus, and the
// Customize Toolbar editor. See docs/plans/shipped/customizable-icon-toolbar.md.

#include "Ui/SmatchetToolbarUi.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Config/ConfigManager.h"
#include "Config/KeybindingsConfig.h" // BoundHotkeyDisplay — surface bound combo on button tooltips
#include "ConfigSaveWorker.h"
#include "ITrackerConnectivity.h"
#include "IconsFontAwesome6.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetToolbarUi_detail.h"
#include "Ui/SmatchetImGuiFonts.h"
#include "Ui/SmatchetUiSession.h"
#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar (pins the bar below the menu)

namespace {

// DR23 — std::string-backed InputText so an editor field (LuaCode / ArgsJson / CommandId / Tooltip)
// is never rounded through a fixed char buffer that silently truncates on the first keystroke.
// Mirrors misc/cpp/imgui_stdlib.h: the buffer IS the std::string's storage and CallbackResize grows
// it as the user types past capacity. The mutable pointer is taken via a c_str cast, matching
// imgui_stdlib, so it stays valid under the project's C++14 build where the non-const data overload
// is unavailable.
int InputTextStdStringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = const_cast<char*>(str->c_str());
    }
    return 0;
}

bool InputTextStdString(const char* label, std::string& str, ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputText(label, const_cast<char*>(str.c_str()), str.capacity() + 1, flags, InputTextStdStringResize,
                            &str);
}

bool InputTextMultilineStdString(const char* label, std::string& str, const ImVec2& size,
                                 ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(label, const_cast<char*>(str.c_str()), str.capacity() + 1, size, flags,
                                     InputTextStdStringResize, &str);
}

bool IsUiPseudoCommand(const std::string& id) {
    return id == "ui.command_palette" || id == "ui.settings" || id == "ui.toolbar_customize";
}

std::string GlyphFor(const ToolbarButton& b) {
    return b.IconGlyph.empty() ? SmatchetToolbarIconGlyph(b.IconName) : b.IconGlyph;
}

bool ButtonEnabled(AppController& app, const ToolbarButton& b) {
    if (b.Kind == ToolbarButtonKind::Lua) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        return true;
#else
        return false;
#endif
    }
    if (b.Kind == ToolbarButtonKind::Command) {
        if (b.CommandId.empty()) {
            return false;
        }
        return IsUiPseudoCommand(b.CommandId) || app.Commands().HasExact(b.CommandId);
    }
    return true; // separators are inert
}

} // namespace

void SmatchetToolbarUi::OpenEditor() { requestEditorOpen_ = true; }

void SmatchetToolbarUi::Draw(AppController& app, TrackerConfig& cfg) {
    if (cfg.Toolbar.Visible) {
        RenderBar(app, cfg);
    }
    RenderEditor(app, cfg);
}

void SmatchetToolbarUi::DispatchButton(AppController& app, TrackerConfig& cfg, const ToolbarButton& b) {
    if (b.Kind == ToolbarButtonKind::Separator) {
        return;
    }
    if (b.Kind == ToolbarButtonKind::Lua) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        std::string err;
        std::string summary;
        if (!app.ExecuteLuaConsoleSnippet(b.LuaCode, err, summary)) {
            LOG_WARN("Toolbar Lua button failed: %s", err.c_str());
        }
#else
        LOG_WARN("Toolbar Lua button ignored: Lua automation not built in this configuration");
#endif
        return;
    }

    // ui.* pseudo-actions are handled in the UI layer (no registry command).
    if (b.CommandId == "ui.toolbar_customize") {
        requestEditorOpen_ = true;
        return;
    }
    if (b.CommandId == "ui.command_palette") {
        g_ui.requestCommandPaletteOpen = true;
        return;
    }
    if (b.CommandId == "ui.settings") {
        cfg.ShowPreferencesWindow = true;
        return;
    }

    // SMATCHET_DEVIATION(rule=duplication; reason=parity w/ SmatchetUI.cpp; owner=cpp-audit; revisit=2026-09-30)
    nlohmann::json args = nlohmann::json::object();
    if (!b.ArgsJson.empty()) {
        std::string parseErr;
        args = smatchet::json_safe::ParseBounded(b.ArgsJson, parseErr);
        if (!parseErr.empty()) { // invalid args JSON → dispatch with empty object
            LOG_WARN("Toolbar: bad args JSON for command \"%s\": %s", b.CommandId.c_str(), parseErr.c_str());
            args = nlohmann::json::object();
        }
    }
    smatchet::cmd::CommandContext ctx;
    ctx.ScenarioHost = &app;
    ctx.Threading = &app;
    ctx.Source = smatchet::cmd::CommandSource::Internal;
    const smatchet::cmd::CommandResult r = app.Commands().Dispatch(b.CommandId, args, ctx);
    if (!r.Ok) {
        LOG_WARN("Toolbar command '%s' failed", b.CommandId.c_str());
    }
}

void SmatchetToolbarUi::RefreshTrackerAppendCache(AppController& app) {
    std::string key;
    const ITrackerBackend* be = app.GetTrackerBackend();
    if (be) {
        key = ConfigManager::NormalizeViewsBackendKey(be->Connectivity().GetTrackerType());
    }
    if (trackerAppendCacheLoaded_ && key == trackerAppendCacheKey_) {
        return; // active backend unchanged — the cached append is still current
    }
    if (trackerAppendLoadInFlight_ && key == trackerAppendLoadInFlightKey_) {
        return; // a load for this exact key is already running; wait for it to land
    }
    // #611 site #7: this runs every RenderBar frame; the persistent-views disk read used to happen
    // here, ON the UI thread, on the first frame after a backend switch (Pillar-2 sub-100ms-but-
    // real block). Move it to a worker: mark in-flight, load off-thread, apply on the UI thread.
    trackerAppendLoadInFlightKey_ = key;
    if (key.empty()) {
        // No active backend → empty append; no disk read needed, settle synchronously.
        trackerAppendCache_.clear();
        trackerAppendCacheKey_ = key;
        trackerAppendCacheLoaded_ = true;
        trackerAppendLoadInFlight_ = false;
        return;
    }
    trackerAppendLoadInFlight_ = true;
    // Lifetime: SmatchetToolbarUi + AppController are app-lifetime; ~AppController joins all
    // LaunchBackgroundTask workers before member teardown, and PostToMainThread no-ops after
    // BeginShutdown() — so capturing `this`/`&app` is safe (same contract as the editor-Save path).
    app.LaunchBackgroundTask([this, &app, key]() {
        const PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
        std::vector<ToolbarButton> append;
        const auto it = disk.Backends.find(key);
        if (it != disk.Backends.end()) {
            append = it->second.ToolbarAppend;
        }
        app.PostToMainThread([this, key, append]() {
            // Apply only if this load is still the current one — a newer backend switch (or a forced
            // reload that re-kicked under a different key) supersedes an older in-flight result.
            if (trackerAppendLoadInFlightKey_ != key) {
                return; // superseded — discard this stale snapshot
            }
            trackerAppendCache_ = append;
            trackerAppendCacheKey_ = key;
            trackerAppendCacheLoaded_ = true;
            trackerAppendLoadInFlight_ = false;
        });
    });
}

namespace {

// Single source of the Customize Toolbar popup title: OpenPopup and BeginPopupModal must build
// the byte-identical (translated) string or the popup ID never matches and the editor won't open.
const char* ToolbarEditorPopupTitle() {
    return SmatchetLocalization::WindowTitle("toolbar.editor.title", "Customize Toolbar", "SmatchetToolbarEditor");
}

// Visible label for a toolbar button: the FA glyph when the icon font is loaded,
// otherwise the first whole WORD of the tooltip (or command id) - a 2-byte prefix
// produced unreadable "Ne"/"vi" stubs and could bisect a UTF-8 code point (P2-L4).
std::string ToolbarButtonVisibleLabel(bool iconFontLoaded, const std::string& glyph, const ToolbarButton& b) {
    if (iconFontLoaded && !glyph.empty()) {
        return glyph;
    }
    const std::string& src = !b.Tooltip.empty() ? b.Tooltip : b.CommandId;
    const std::size_t wordEnd = src.find_first_of(" .");
    std::string label = wordEnd == std::string::npos ? src : src.substr(0, wordEnd);
    if (label.empty()) {
        label = "?";
    }
    return label;
}

} // namespace

void SmatchetToolbarUi::RenderBar(AppController& app, TrackerConfig& cfg) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float barH = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginViewportSideBar("##SmatchetToolbar", vp, ImGuiDir_Up, barH, flags)) {
        // The per-tracker append is loaded once per backend change (not per frame) to keep the
        // render path off the disk; right-click button menus still map only to the global list.
        RefreshTrackerAppendCache(app);
        const std::vector<ToolbarButton> eff = ResolveEffectiveToolbar(cfg.Toolbar, trackerAppendCache_);
        const bool fa = SmatchetAreFaIconsLoaded();

        // The effective bar only drops/merges separators — it never reorders real buttons —
        // so the k-th non-separator on screen is the k-th non-separator in cfg.Toolbar.Buttons.
        // realButtonSrc maps that ordinal back to the global index a right-click menu mutates.
        std::vector<int> realButtonSrc;
        realButtonSrc.reserve(cfg.Toolbar.Buttons.size());
        for (int s = 0; s < static_cast<int>(cfg.Toolbar.Buttons.size()); ++s) {
            if (cfg.Toolbar.Buttons[s].Kind != ToolbarButtonKind::Separator) {
                realButtonSrc.push_back(s);
            }
        }
        int realSeen = 0;

        for (std::size_t i = 0; i < eff.size(); ++i) {
            const ToolbarButton& b = eff[i];
            if (i != 0) {
                ImGui::SameLine();
            }
            if (b.Kind == ToolbarButtonKind::Separator) {
                ImGui::TextDisabled("|");
                continue;
            }

            const int src = (realSeen < static_cast<int>(realButtonSrc.size())) ? realButtonSrc[realSeen] : -1;
            ++realSeen;

            std::string label = ToolbarButtonVisibleLabel(fa, GlyphFor(b), b);
            label += "##tb";
            label += std::to_string(i);

            const bool enabled = ButtonEnabled(app, b);
            if (!enabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(label.c_str())) {
                DispatchButton(app, cfg, b);
            }
            if (!enabled) {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                std::string tip = b.Tooltip.empty() ? b.CommandId : b.Tooltip;
                if (b.Kind == ToolbarButtonKind::Command && !b.CommandId.empty()) {
                    // A hover tooltip has room for the whole alias set, unlike the menu
                    // and palette shortcut columns — so surface every combo here, resolved
                    // against the button's OWN args (what DispatchButton dispatches), never
                    // a hardcoded "{}": the args match is semantic and has no fallback.
                    const std::string combo =
                        smatchet::toolbar_editor::ToolbarButtonShortcutDisplay(cfg.Keybindings.Bindings, b);
                    if (!combo.empty()) {
                        tip += "  (";
                        tip += combo;
                        tip += ")";
                    }
                }
                if (b.Kind == ToolbarButtonKind::Command && !enabled) {
                    tip += "  (";
                    tip += SmatchetLocalization::T("toolbar.command_not_found", "command not found");
                    tip += ")";
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }

            // Per-button right-click menu. Opened manually (not BeginPopupContextItem) so it
            // works on a disabled button too — e.g. to delete one bound to a missing command.
            if (src >= 0) {
                std::string menuId = "##tbmenu";
                menuId += std::to_string(i);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
                    ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    ImGui::OpenPopup(menuId.c_str());
                }
                if (ImGui::BeginPopup(menuId.c_str())) {
                    RenderButtonContextMenu(cfg, src);
                    ImGui::EndPopup();
                }
            }
        }

        // Pillar 2 (#611): visible in-progress cue while the per-tracker append reload runs on a
        // worker (RefreshTrackerAppendCache → LoadPersistentViewsFromDisk off-thread). Without it
        // the bar silently shows the previous/empty append for the frame(s) until the load lands —
        // an invisible (sub-100ms-but-real) blocking gap pre-offload, now an invisible async gap.
        // The cue is submitted on the same frame the in-flight flag is true (cue-before-stall
        // ordering). The labelled-Button id is the bucket-E visible-cue test's findable anchor.
        if (trackerAppendLoadInFlight_) {
            ImGui::SameLine();
            ImGui::BeginDisabled();
            ImGui::SmallButton(ICON_FA_SPINNER "##tb-append-loading");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "%s", SmatchetLocalization::T("toolbar.append_loading", "Loading tracker toolbar buttons..."));
            }
        }

        // Right-click empty bar area → bar-level context menu (NoOpenOverItems defers to the
        // per-button menu above when the cursor is over a button).
        if (ImGui::BeginPopupContextWindow("##ToolbarCtx",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem(SmatchetLocalization::T("toolbar.menu.customize", "Customize Toolbar..."))) {
                requestEditorOpen_ = true;
            }
            if (ImGui::MenuItem(SmatchetLocalization::T("toolbar.menu.hide", "Hide Toolbar"))) {
                cfg.Toolbar.Visible = false;
                smatchet::config_save::EnqueueTrackerConfig(cfg);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void SmatchetToolbarUi::RenderButtonContextMenu(TrackerConfig& cfg, int src) {
    std::vector<ToolbarButton>& buttons = cfg.Toolbar.Buttons;
    if (src < 0 || src >= static_cast<int>(buttons.size())) {
        return;
    }
    const int count = static_cast<int>(buttons.size());

    // Edit... defers selection to the editor (no save here); the other ops mutate the global
    // button list in place — the same swap/insert/erase the Customize editor uses — then persist.
    if (ImGui::MenuItem(SmatchetLocalization::T("toolbar.menu.edit", "Edit..."))) {
        requestEditorOpen_ = true;
        requestEditSelect_ = src;
    }
    // Only command buttons map to a rebindable keybinding; Lua/separator buttons have no
    // command id to bind. Latches the request for SmatchetUI to open the shared quick-bind modal.
    const ToolbarButton& clicked = buttons[src];
    if (clicked.Kind == ToolbarButtonKind::Command && !clicked.CommandId.empty()) {
        if (ImGui::MenuItem(SmatchetLocalization::T("cmdpalette.set_shortcut", "Set shortcut..."))) {
            requestQuickBindCommandId_ = clicked.CommandId;
            requestQuickBindArgsJson_ = clicked.ArgsJson; // bind the button's real args, not "{}"
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(SmatchetLocalization::T("toolbar.menu.move_left", "Move Left"), nullptr, false, src > 0)) {
        std::swap(buttons[src], buttons[src - 1]);
        smatchet::config_save::EnqueueTrackerConfig(cfg);
    }
    if (ImGui::MenuItem(SmatchetLocalization::T("toolbar.menu.move_right", "Move Right"), nullptr, false,
                        src < count - 1)) {
        std::swap(buttons[src], buttons[src + 1]);
        smatchet::config_save::EnqueueTrackerConfig(cfg);
    }
    if (ImGui::MenuItem(SmatchetLocalization::T("toolbar.menu.insert_separator", "Insert Separator"))) {
        ToolbarButton sep;
        sep.Kind = ToolbarButtonKind::Separator;
        buttons.insert(buttons.begin() + src + 1, sep); // after the clicked button
        smatchet::config_save::EnqueueTrackerConfig(cfg);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(SmatchetLocalization::T("common.delete", "Delete"))) {
        buttons.erase(buttons.begin() + src);
        smatchet::config_save::EnqueueTrackerConfig(cfg);
    }
}

std::string SmatchetToolbarUi::TakeQuickBindRequest(std::string& argsJsonOut) {
    std::string id;
    id.swap(requestQuickBindCommandId_);
    if (!id.empty()) {
        argsJsonOut = requestQuickBindArgsJson_;
    }
    requestQuickBindArgsJson_ = "{}"; // reset the paired latch for the next request
    return id;
}

void SmatchetToolbarUi::SyncEditorOpenRequest(AppController& app, TrackerConfig& cfg) {
    if (!requestEditorOpen_) {
        return;
    }
    requestEditorOpen_ = false;
    editBuf_ = cfg.Toolbar;

    // Capture the active backend so Tracker scope edits the matching views bucket. The
    // right-click "Edit..." path preselects a global button, so the editor always opens in
    // Global scope; the user switches to Tracker scope explicitly.
    editScope_ = EditScope::Global;
    editHasTracker_ = false;
    editTrackerType_.clear();
    editTrackerKey_.clear();
    trackerEditBuf_.clear();
    const ITrackerBackend* be = app.GetTrackerBackend();
    if (be) {
        editTrackerType_ = be->Connectivity().GetTrackerType();
        editTrackerKey_ = ConfigManager::NormalizeViewsBackendKey(editTrackerType_);
        editHasTracker_ = true;
        // Pillar-2: intentionally synchronous — rare one-shot on explicit editor-open; sub-100ms single-file read,
        // async would force a loading state into a modal that opens fully populated. Guard warn here is acceptable
        // residue.
        const PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
        const auto it = disk.Backends.find(editTrackerKey_);
        if (it != disk.Backends.end()) {
            trackerEditBuf_ = it->second.ToolbarAppend;
        }
    }

    if (requestEditSelect_ >= 0 && requestEditSelect_ < static_cast<int>(editBuf_.Buttons.size())) {
        selected_ = requestEditSelect_;
    } else {
        selected_ = editBuf_.Buttons.empty() ? -1 : 0;
    }
    requestEditSelect_ = -1;
    cmdNames_.clear();
    const std::vector<smatchet::cmd::Command> all = app.Commands().All();
    cmdNames_.reserve(all.size());
    for (const smatchet::cmd::Command& c : all) {
        cmdNames_.push_back(c.Name);
    }
    cmdSearch_[0] = '\0';
    // WindowTitle appends "###SmatchetToolbarEditor" so the popup ID is stable across languages
    // (both OpenPopup and BeginPopupModal build the identical translated string).
    ImGui::OpenPopup(ToolbarEditorPopupTitle());
}

SmatchetToolbarUi::EditorCtx SmatchetToolbarUi::DrawEditorScopeSelector() {
    const bool fa = SmatchetAreFaIconsLoaded();

    // Scope selector. Global scope edits the shared toolbar; Tracker scope edits the active
    // backend append list, shown on the bar after the global set. Switching resets selection.
    ImGui::TextUnformatted(SmatchetLocalization::T("toolbar.editor.scope", "Scope:"));
    ImGui::SameLine();
    if (ImGui::RadioButton(SmatchetLocalization::T("toolbar.editor.scope_global", "Global"),
                           editScope_ == EditScope::Global) &&
        editScope_ != EditScope::Global) {
        editScope_ = EditScope::Global;
        selected_ = editBuf_.Buttons.empty() ? -1 : 0;
    }
    ImGui::SameLine();
    {
        const std::string trackerLabel =
            std::string(editHasTracker_
                            ? SmatchetLocalization::Format("toolbar.editor.scope_tracker", "Current tracker: %s",
                                                           editTrackerType_.c_str())
                            : SmatchetLocalization::T("toolbar.editor.scope_tracker_none", "Current tracker: (none)")) +
            "##scopeTracker";
        if (!editHasTracker_) {
            ImGui::BeginDisabled();
        }
        if (ImGui::RadioButton(trackerLabel.c_str(), editScope_ == EditScope::Tracker) &&
            editScope_ != EditScope::Tracker) {
            editScope_ = EditScope::Tracker;
            selected_ = trackerEditBuf_.empty() ? -1 : 0;
        }
        if (!editHasTracker_) {
            ImGui::EndDisabled();
        }
    }

    const bool trackerScope = editScope_ == EditScope::Tracker;
    std::vector<ToolbarButton>& buttons = trackerScope ? trackerEditBuf_ : editBuf_.Buttons;
    if (trackerScope) {
        ImGui::TextDisabled("%s", SmatchetLocalization::Format("toolbar.editor.scope_tracker_hint",
                                                               "Appended for %s, shown after the global toolbar.",
                                                               editTrackerType_.c_str()));
    } else {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("toolbar.editor.scope_global_hint",
                                                          "Global toolbar, shown for every tracker."));
    }
    ImGui::Separator();
    return EditorCtx{buttons, trackerScope, fa};
}

void SmatchetToolbarUi::DrawEditorActionRow(EditorCtx& ctx) {
    std::vector<ToolbarButton>& buttons = ctx.buttons;

    // Action row.
    if (ImGui::Button(SmatchetLocalization::T("toolbar.editor.add_command", "Add command"))) {
        ToolbarButton b;
        b.Kind = ToolbarButtonKind::Command;
        b.Tooltip = "New button";
        buttons.push_back(b);
        selected_ = static_cast<int>(buttons.size()) - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("toolbar.editor.add_lua", "Add Lua"))) {
        ToolbarButton b;
        b.Kind = ToolbarButtonKind::Lua;
        b.Tooltip = "New Lua button";
        buttons.push_back(b);
        selected_ = static_cast<int>(buttons.size()) - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("toolbar.editor.add_separator", "Add separator"))) {
        ToolbarButton b;
        b.Kind = ToolbarButtonKind::Separator;
        buttons.push_back(b);
        selected_ = static_cast<int>(buttons.size()) - 1;
    }

    const int count = static_cast<int>(buttons.size());
    const bool hasSel = selected_ >= 0 && selected_ < count;

    ImGui::SameLine();
    if (!hasSel) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(SmatchetLocalization::T("toolbar.editor.duplicate", "Duplicate")) && hasSel) {
        buttons.insert(buttons.begin() + selected_ + 1, buttons[selected_]);
        ++selected_;
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("common.delete", "Delete")) && hasSel) {
        buttons.erase(buttons.begin() + selected_);
        selected_ = buttons.empty() ? -1 : (std::min)(selected_, static_cast<int>(buttons.size()) - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("toolbar.editor.up", "Up")) && hasSel && selected_ > 0) {
        std::swap(buttons[selected_], buttons[selected_ - 1]);
        --selected_;
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("toolbar.editor.down", "Down")) && hasSel && selected_ < count - 1) {
        std::swap(buttons[selected_], buttons[selected_ + 1]);
        ++selected_;
    }
    if (!hasSel) {
        ImGui::EndDisabled();
    }

    ImGui::Separator();
}

void SmatchetToolbarUi::DrawEditorButtonList(EditorCtx& ctx) {
    std::vector<ToolbarButton>& buttons = ctx.buttons;

    // Left: button list (drag-drop reorder, confined to the active scope).
    ImGui::BeginChild("##tblist", ImVec2(240.0f, -ImGui::GetFrameHeightWithSpacing()), true);
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        const std::string rowLabel = smatchet::toolbar_editor::EditorRowLabel(buttons[i], ctx.faLoaded);
        ImGui::PushID(i);
        if (ImGui::Selectable(rowLabel.c_str(), selected_ == i)) {
            selected_ = i;
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("SMATCHET_TB_BTN", &i, sizeof(int));
            ImGui::TextUnformatted(rowLabel.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SMATCHET_TB_BTN")) {
                const int src = *static_cast<const int*>(p->Data);
                if (src >= 0 && src < static_cast<int>(buttons.size()) && src != i) {
                    ToolbarButton moved = buttons[src];
                    buttons.erase(buttons.begin() + src);
                    const int dst = smatchet::toolbar_editor::DragDropDestIndex(src, i);
                    buttons.insert(buttons.begin() + dst, moved);
                    selected_ = dst;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void SmatchetToolbarUi::DrawEditorFieldEditor(EditorCtx& ctx) {
    std::vector<ToolbarButton>& buttons = ctx.buttons;
    const bool fa = ctx.faLoaded;

    // Right: field editor for the selected button.
    ImGui::BeginChild("##tbedit", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), true);
    if (selected_ >= 0 && selected_ < static_cast<int>(buttons.size())) {
        ToolbarButton& b = buttons[selected_];
        const char* kinds[] = {SmatchetLocalization::T("toolbar.kind.command", "Command"),
                               SmatchetLocalization::T("toolbar.kind.lua", "Lua"),
                               SmatchetLocalization::T("toolbar.kind.separator", "Separator")};
        int k = static_cast<int>(b.Kind);
        if (ImGui::Combo(SmatchetLocalization::T("toolbar.editor.kind", "Kind"), &k, kinds, 3)) {
            b.Kind = static_cast<ToolbarButtonKind>(k);
        }

        if (b.Kind != ToolbarButtonKind::Separator) {
            char nameBuf[64];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", b.IconName.c_str());
            if (ImGui::InputText(SmatchetLocalization::T("toolbar.editor.icon_name", "Icon name"), nameBuf,
                                 sizeof(nameBuf))) {
                b.IconName = nameBuf;
                b.IconGlyph = SmatchetToolbarIconGlyph(b.IconName);
            }
            ImGui::SameLine();
            if (ImGui::Button(SmatchetLocalization::T("toolbar.editor.pick", "Pick..."))) {
                iconPicker_.Open();
            }
            {
                std::string pn;
                std::string pg;
                if (iconPicker_.Draw(pn, pg)) {
                    b.IconName = pn;
                    b.IconGlyph = pg;
                }
            }
            const std::string g = GlyphFor(b);
            if (fa && !g.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted(g.c_str());
            }

            // DR23: std::string-backed — no truncation of a long tooltip.
            InputTextStdString(SmatchetLocalization::T("toolbar.editor.tooltip", "Tooltip"), b.Tooltip);
        }

        if (b.Kind == ToolbarButtonKind::Command) {
            // DR23: std::string-backed — no truncation of a long command id.
            InputTextStdString(SmatchetLocalization::T("toolbar.editor.command_id", "Command id"), b.CommandId);
            ImGui::TextDisabled("%s", SmatchetLocalization::T("toolbar.editor.pick_command", "Pick a command:"));
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##cmdsearch",
                                     SmatchetLocalization::T("toolbar.editor.filter_commands", "Filter commands..."),
                                     cmdSearch_, sizeof(cmdSearch_));
            std::string q(cmdSearch_);
            ImGui::BeginChild("##cmdlist", ImVec2(0.0f, 90.0f), true);
            for (const std::string& n : cmdNames_) {
                if (!q.empty() && n.find(q) == std::string::npos) {
                    continue;
                }
                if (ImGui::Selectable(n.c_str(), b.CommandId == n)) {
                    b.CommandId = n;
                }
            }
            ImGui::EndChild();

            // DR23: std::string-backed — no truncation of a long args-JSON payload.
            InputTextStdString(SmatchetLocalization::T("toolbar.editor.args_json", "Args (JSON)"), b.ArgsJson);
        } else if (b.Kind == ToolbarButtonKind::Lua) {
            // DR23: std::string-backed — no truncation of a >2 KB Lua script.
            InputTextMultilineStdString(SmatchetLocalization::T("toolbar.editor.lua_code", "Lua code"), b.LuaCode,
                                        ImVec2(-1.0f, 120.0f));
        } else {
            ImGui::TextDisabled("%s",
                                SmatchetLocalization::T("toolbar.editor.separator_hint", "Separator (no settings)."));
        }
    } else {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("toolbar.editor.select_hint", "Select or add a button."));
    }
    ImGui::EndChild();
}

void SmatchetToolbarUi::DrawEditorFooter(AppController& app, EditorCtx& ctx, TrackerConfig& cfg) {
    // Footer. "Show toolbar" is a global-only setting (per-tracker visibility deferred), so in
    // Tracker scope a hint anchors the line instead.
    if (ctx.trackerScope) {
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("toolbar.editor.visibility_hint", "Visibility follows the global scope."));
    } else {
        ImGui::Checkbox(SmatchetLocalization::T("toolbar.editor.show_toolbar", "Show toolbar"), &editBuf_.Visible);
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f);
    if (ImGui::Button(SmatchetLocalization::T("common.save", "Save"), ImVec2(90.0f, 0.0f))) {
        if (ctx.trackerScope && editHasTracker_) {
            // Pillar-2: offload the load-modify-save off the UI thread. The load + save stay
            // co-located on one task so they remain ordered (no intra-pair read-after-write
            // hazard) and only this backend's append list changes — its Views/ActiveViewId and
            // every other backend bucket are preserved. Capture the key + a BY-VALUE copy of the
            // edit buffer so no UI state is aliased across the worker thread. `&app`/`this` are
            // app-lifetime: ~AppController joins all LaunchBackgroundTask threads before member
            // destruction, and PostToMainThread no-ops after BeginShutdown(), so the
            // cache-invalidation hop is safe to drop on exit.
            const std::string trackerKey = editTrackerKey_;
            const std::vector<ToolbarButton> trackerAppend = trackerEditBuf_;
            app.LaunchBackgroundTask([&app, this, trackerKey, trackerAppend]() {
                PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
                disk.Backends[trackerKey].ToolbarAppend = trackerAppend;
                ConfigManager::SavePersistentViewsToDisk(disk);
                app.PostToMainThread([this]() { trackerAppendCacheLoaded_ = false; });
            });
        } else {
            cfg.Toolbar = editBuf_;
            smatchet::config_save::EnqueueTrackerConfig(cfg);
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("common.cancel", "Cancel"), ImVec2(90.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
}

void SmatchetToolbarUi::RenderEditor(AppController& app, TrackerConfig& cfg) {
    SyncEditorOpenRequest(app, cfg);

    ImGui::SetNextWindowSize(ImVec2(620.0f, 460.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(ToolbarEditorPopupTitle(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    EditorCtx ctx = DrawEditorScopeSelector();
    DrawEditorActionRow(ctx);
    DrawEditorButtonList(ctx);
    ImGui::SameLine();
    DrawEditorFieldEditor(ctx);
    DrawEditorFooter(app, ctx, cfg);
    ImGui::EndPopup();
}
