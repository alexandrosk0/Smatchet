// Ctrl+Shift+P command palette modal.
// See docs/plans/shipped/command-system-plan.md §"Command Palette — Ctrl+Shift+P modal".

#include "Commands/CommandPaletteUi.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/CommandRegistry.h"
#include "Commands/FuzzyMatch.h"
#include "Config/KeybindingsConfig.h"
#include "DictationInsertionRouter.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

// Registering a history callback makes the filter input own Up/Down while it is active.
// Without it, keyboard navigation takes the first Down, moves focus from the input onto
// the list and deactivates the input, so whatever is typed next is dropped. With it the
// caret stays in the query while handleListNavigation moves the highlight.
int KeepArrowKeysInFilterInput(ImGuiInputTextCallbackData*) { return 0; }

} // namespace

void CommandPaletteUi::Open() {
    open_ = true;
    focusFilterPending_ = true;
    selected_ = 0;
    scrollToSelected_ = true;
    dispatchError_.clear();
    argFormStep_ = -1;
    argFocusPending_ = false;
    categoryPrefix_.clear();
    categoryLabel_.clear();
    std::memset(filterBuf_, 0, sizeof(filterBuf_));
    // DR27: drop the list built for the previous session so the first Draw after
    // opening rebuilds from the now-empty filter buffer. Without this a reopened
    // palette could show — and dispatch — a stale filtered list (e.g. the
    // `view.toggle.` set from a prior ui.open_view) while its input box reads empty.
    filtered_.clear();
    // Register the filter buffer with the dictation router for the duration
    // of the palette being open. The palette's ::ImGui::InputText call below
    // bypasses the SmatchetLocalizedImGui wrapper (raw ImGui by design — the
    // palette label is its own translation surface), so this explicit
    // register / unregister is the only path that wires dictation up here.
    g_dictationRouter.RegisterInputText(filterBuf_, sizeof(filterBuf_), nullptr);
}

void CommandPaletteUi::Close() {
    open_ = false;
    dispatchError_.clear();
    argFormStep_ = -1;
    argFocusPending_ = false;
    argFormBufs_.clear();
    categoryPrefix_.clear();
    categoryLabel_.clear();
    g_dictationRouter.UnregisterInputText(filterBuf_);
}

void CommandPaletteUi::SetCategoryPrefix(const char* prefix, const char* label) {
    categoryPrefix_ = prefix != nullptr ? prefix : "";
    categoryLabel_ = label != nullptr ? label : "";
    filtered_.clear(); // force a rebuild under the new scope on the next Draw
    selected_ = 0;
    scrollToSelected_ = true;
    dispatchError_.clear();
}

void CommandPaletteUi::SetFilterText(const char* query) {
    if (query == nullptr) {
        filterBuf_[0] = '\0';
    } else {
        std::strncpy(filterBuf_, query, sizeof(filterBuf_) - 1U);
        filterBuf_[sizeof(filterBuf_) - 1U] = '\0';
    }
    // The buffer changed behind the input widget, so Draw sees no edit: drop the list
    // built for the old query (Draw rebuilds an empty one) along with what described it.
    filtered_.clear();
    selected_ = 0;
    scrollToSelected_ = true;
    dispatchError_.clear();
}

void CommandPaletteUi::rebuildFiltered(AppController& app) {
    const std::string q = filterBuf_;
    // Snapshot the command table once. All() takes a lock, deep-copies, and sorts
    // the whole registry, so the empty-query branch below must reuse this snapshot
    // rather than re-fetching it once per recent (was up to 9 All() calls per
    // rebuild — CPP_CODE_AUDIT.md #33 perf sub-item).
    std::vector<Command> all = app.Commands().All();
    if (!categoryPrefix_.empty()) {
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [this](const Command& c) {
                                     return c.Name.compare(0U, categoryPrefix_.size(), categoryPrefix_) != 0;
                                 }),
                  all.end());
    }
    if (q.empty()) {
        // Empty query: show recent commands first, then all sorted by name.
        filtered_.clear();
        const std::vector<std::string> recents = app.Commands().Recents(8);
        for (const std::string& r : recents) {
            for (const Command& c : all) {
                if (c.Name == r) {
                    filtered_.push_back(c);
                    break;
                }
            }
        }
        for (const Command& c : all) {
            bool alreadyIn = false;
            for (const Command& f : filtered_) {
                if (f.Name == c.Name) {
                    alreadyIn = true;
                    break;
                }
            }
            if (!alreadyIn)
                filtered_.push_back(c);
        }
    } else {
        // Fuzzy-score each command against the query.
        struct Scored {
            int score;
            Command cmd;
        };
        std::vector<Scored> scored;
        scored.reserve(all.size());
        for (const Command& c : all) {
            const int s = FuzzyScore(q, c.Title + " " + c.Name + " " + c.Summary);
            if (s > 0)
                scored.push_back({s, c});
        }
        std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
            if (a.score != b.score)
                return a.score > b.score;
            return a.cmd.Name < b.cmd.Name;
        });
        filtered_.clear();
        filtered_.reserve(scored.size());
        for (auto& s : scored)
            filtered_.push_back(std::move(s.cmd));
    }
    // Clamp selection.
    if (selected_ < 0)
        selected_ = 0;
    if (filtered_.empty()) {
        selected_ = 0;
    } else if (selected_ >= static_cast<int>(filtered_.size())) {
        selected_ = static_cast<int>(filtered_.size()) - 1;
    }
}

void CommandPaletteUi::dispatchSelected() {
    if (filtered_.empty() || selected_ < 0 || selected_ >= static_cast<int>(filtered_.size())) {
        return;
    }
    const Command& cmd = filtered_[selected_];

    // Check for required params without values.
    for (const ParamSpec& p : cmd.Params) {
        if (p.Required) {
            // Open arg-form mode.
            argFormCmd_ = cmd;
            argFormStep_ = 0;
            argFocusPending_ = true;
            dispatchError_.clear();
            argFormBufs_.clear();
            for (size_t i = 0; i < cmd.Params.size(); ++i) {
                argFormBufs_.emplace_back(256, '\0');
                // Seed from default if present.
                if (cmd.Params[i].Default && !cmd.Params[i].Default->is_null() && cmd.Params[i].Default->is_string()) {
                    const std::string def = cmd.Params[i].Default->get<std::string>();
                    const size_t len = (std::min)(def.size(), size_t(255));
                    std::memcpy(argFormBufs_.back().data(), def.c_str(), len);
                }
            }
            return;
        }
    }

    // No required params. LATCH for deferred dispatch — do NOT dispatch inline:
    // this runs inside the palette's Begin("##cmdpalette")/End() scope, and a
    // command that touches the ImGui window stack would corrupt it here (#1703).
    // Draw() drains the latch at top-of-frame, outside the Begin/End.
    pendingDispatchName_ = cmd.Name;
    pendingDispatchArgs_.reset(); // null = empty-object args
    // Destructive commands require Shift+Enter (already checked by caller).
    pendingDispatchDestructive_ = cmd.Destructive;
    hasPendingDispatch_ = true;
    dispatchError_.clear();
}

void CommandPaletteUi::drainPendingDispatch(AppController& app) {
    if (!hasPendingDispatch_) {
        return;
    }
    hasPendingDispatch_ = false;
    CommandContext ctx;
    ctx.ScenarioHost = &app;
    ctx.Threading = &app;
    ctx.Source = CommandSource::Palette;
    ctx.ConfirmedDestructive = pendingDispatchDestructive_;
    const nlohmann::json args = pendingDispatchArgs_ ? *pendingDispatchArgs_ : nlohmann::json::object();
    CommandResult r = app.Commands().Dispatch(pendingDispatchName_, args, ctx);
    pendingDispatchArgs_.reset();
    if (!r.Ok) {
        LOG_WARN("Palette: dispatch '%s' failed: %s", pendingDispatchName_.c_str(), r.Error.Message.c_str());
        // Stay open with whatever was typed: closing here would discard the argument
        // buffers and leave the log as the only trace of why nothing happened.
        std::string why = r.Error.Message.empty()
                              ? std::string(SmatchetLocalization::T("cmdpalette.run_failed_unknown",
                                                                    "The command failed without an error message."))
                              : r.Error.Message;
        if (!r.Error.Hint.empty()) {
            why += " ";
            why += r.Error.Hint;
        }
        dispatchError_ = SmatchetLocalization::Format("cmdpalette.run_failed", "Could not run: %s", why.c_str());
        // Enter deactivated the input it was pressed in; put the caret back.
        if (argFormStep_ >= 0) {
            argFocusPending_ = true;
        } else {
            focusFilterPending_ = true;
        }
        return;
    }
    // Persist recents so they survive restarts.
    app.Commands().SaveRecents();
    Close();
}

void CommandPaletteUi::drawDispatchError() const {
    if (dispatchError_.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, SmatchetTheme::GetActiveSemanticColors().ErrorText);
    ImGui::TextWrapped("%s", dispatchError_.c_str());
    ImGui::PopStyleColor();
}

void CommandPaletteUi::drawArgForm() {
    ImGui::Separator();
    ImGui::TextUnformatted(SmatchetLocalization::T("cmdpalette.parameters", "Parameters:"));
    bool allFilled = true;
    for (size_t i = 0; i < argFormCmd_.Params.size(); ++i) {
        const ParamSpec& p = argFormCmd_.Params[i];
        ImGui::Text("%s%s:", p.Name.c_str(), p.Required ? " *" : "");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.f);
        const std::string label = "##arg" + std::to_string(i);
        if (i == 0 && argFocusPending_) {
            argFocusPending_ = false;
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputText(label.c_str(), argFormBufs_[i].data(), argFormBufs_[i].size())) {
            dispatchError_.clear(); // the message described the previous values
        }
        if (p.Required && argFormBufs_[i][0] == '\0') {
            allFilled = false;
        }
    }
    const bool canRun = allFilled;
    if (!canRun)
        ImGui::BeginDisabled();
    if (ImGui::Button(SmatchetLocalization::T("cmdpalette.run", "Run")) ||
        (canRun && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
        nlohmann::json args = nlohmann::json::object();
        for (size_t i = 0; i < argFormCmd_.Params.size(); ++i) {
            const ParamSpec& p = argFormCmd_.Params[i];
            const std::string val = argFormBufs_[i].data();
            if (!val.empty()) {
                args[p.Name] = val;
            }
        }
        // LATCH for deferred dispatch (#1703) — the "Run" button fires inside the
        // palette's Begin/End scope, same window-stack-corruption hazard as the
        // no-arg path. Draw() drains it at top-of-frame.
        pendingDispatchName_ = argFormCmd_.Name;
        pendingDispatchArgs_ = std::make_shared<nlohmann::json>(std::move(args));
        pendingDispatchDestructive_ = argFormCmd_.Destructive;
        hasPendingDispatch_ = true;
        dispatchError_.clear();
    }
    if (!canRun)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("common.cancel", "Cancel")) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        Close();
        return;
    }
    drawDispatchError();
}

void CommandPaletteUi::Draw(AppController& app) {
    // #1703 — drain any command latched by the PREVIOUS frame's dispatchSelected()
    // / arg-form "Run" here, at the TOP of Draw, BEFORE the palette's own
    // Begin("##cmdpalette"). A latched command that opens/closes a window or is
    // otherwise re-entrant then runs at a clean ImGui nesting level instead of
    // mid-Begin/End (the window-stack corruption that aborted the process). Runs
    // even when open_ is false — the dispatch itself is what closes the palette.
    drainPendingDispatch(app);

    // Open/close is driven by the rebindable ui.command_palette binding (default
    // Ctrl+Shift+P), dispatched in SmatchetUI::dispatchKeybindings; this method only
    // renders the palette window when open_ is set.
    if (!open_)
        return;

    // Center the modal.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600.f, 420.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("##cmdpalette", &open_, flags)) {
        ImGui::End();
        return;
    }

    // Auto-focus the filter input on first appearance, or whenever a caller
    // re-armed focus via FocusFilterInput(). SetWindowFocus() comes first: the
    // re-arm path exists precisely for the case where ANOTHER window holds focus
    // (the first-run Preferences surface opening after the palette did), and
    // focusing the item without focusing its window leaves the input inactive.
    if (ImGui::IsWindowAppearing() || focusFilterPending_) {
        focusFilterPending_ = false;
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere();
    }

    // Active category scope (e.g. "Open View..."): say so instead of leaking the id
    // prefix into the search box (P2-M7).
    if (!categoryLabel_.empty()) {
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::Format("cmdpalette.scope", "Showing %s commands only", categoryLabel_.c_str()));
    }

    // Filter text input.
    const bool filterChanged = ImGui::InputTextWithHint(
        "##cmdq", SmatchetLocalization::T("cmdpalette.hint", "Type a command or search..."), filterBuf_,
        sizeof(filterBuf_), ImGuiInputTextFlags_CallbackHistory, KeepArrowKeysInFilterInput);
    if (filterChanged) {
        // A new query re-ranks the list, so the old index now names an unrelated row:
        // go back to the best match, which is what Enter should run.
        selected_ = 0;
        scrollToSelected_ = true;
        dispatchError_.clear();
    }
    if (filterChanged || filtered_.empty()) {
        rebuildFiltered(app);
    }

    // Arg-form mode.
    if (argFormStep_ >= 0) {
        drawArgForm();
        ImGui::End();
        return;
    }

    ImGui::Separator();

    const ImGuiIO& io = ImGui::GetIO();
    handleListNavigation(io);

    drawCommandList(io);

    // Hint line.
    ImGui::Separator();
    drawDispatchError();
    if (!filtered_.empty() && filtered_[selected_].Destructive) {
        ImGui::TextColored(SmatchetTheme::Colors::PriorityHigh, "%s",
                           SmatchetLocalization::T("cmdpalette.destructive_hint",
                                                   "Destructive — hold Shift and click, or press Shift+Enter"));
    } else {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("cmdpalette.footer_hint",
                                                          "Enter to run · Esc to close · Up/Down to navigate"));
    }

    ImGui::End();
}

// Up/Down selection movement + Enter/Escape dispatch. Destructive commands require Shift+Enter.
// Split out of Draw for function-size compliance; runs inside the active palette Begin scope.
void CommandPaletteUi::handleListNavigation(const ImGuiIO& io) {
    const int before = selected_;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && selected_ > 0)
        --selected_;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && selected_ < static_cast<int>(filtered_.size()) - 1)
        ++selected_;
    if (selected_ != before) {
        scrollToSelected_ = true;
        dispatchError_.clear();
    }

    // Enter dispatch. Destructive requires Shift.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !filtered_.empty()) {
        const Command& sel = filtered_[selected_];
        if (!sel.Destructive || io.KeyShift) {
            dispatchSelected();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        Close();
    }
}

// Scrollable command-list child. Owns its own BeginChild/EndChild pair. Destructive rows render in
// the warning colour and require Shift to dispatch on click.
void CommandPaletteUi::drawCommandList(const ImGuiIO& io) {
    // The palette window has a fixed height, so a failure message takes its lines from the
    // list rather than pushing the key hint below the bottom edge.
    float listH = 300.f;
    if (!dispatchError_.empty()) {
        const float errorH =
            ImGui::CalcTextSize(dispatchError_.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x).y;
        listH = (std::max)(120.f, listH - errorH - ImGui::GetStyle().ItemSpacing.y);
    }
    ImGui::BeginChild("##cmdlist", ImVec2(0, listH), false);
    for (int i = 0; i < static_cast<int>(filtered_.size()); ++i) {
        const Command& c = filtered_[i];
        const bool isSelected = (i == selected_);

        // Full row width captured before the selectable (cursor is at the row's left edge) so the
        // bound-combo hint can be right-aligned over the selectable's empty right region.
        const float rowWidth = ImGui::GetContentRegionAvail().x;

        // Destructive rows drawn in warning colour.
        if (c.Destructive) {
            ImGui::PushStyleColor(ImGuiCol_Text, SmatchetTheme::Colors::PriorityHigh);
        }

        // Selectable row. Leads with the human Title (title-cased-slug fallback — the
        // registry's own contract says raw ids never reach a widget, P2-M7); the dotted
        // id is drawn dimmed after the text. The combo (if any) is drawn separately
        // right-aligned, not baked into the label, so the selectable id stays stable as
        // bindings change (##Name keeps it stable across Title edits too).
        const std::string primary = c.Title.empty() ? TitleCasedSlugFromCommandId(c.Name) : c.Title;
        const std::string rowLabel = primary + "  " + c.Summary + "###row_" + c.Name;
        if (ImGui::Selectable(rowLabel.c_str(), isSelected, ImGuiSelectableFlags_None, ImVec2(0, 0))) {
            if (selected_ != i) {
                dispatchError_.clear();
            }
            selected_ = i;
            if (!c.Destructive || io.KeyShift) {
                dispatchSelected();
            }
        }
        if (c.Destructive) {
            ImGui::PopStyleColor();
        }
        // Per-row right-click: latch a "Set shortcut…" request for SmatchetUI's quick-bind modal.
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(SmatchetLocalization::T("cmdpalette.set_shortcut", "Set shortcut..."))) {
                requestQuickBindCommandId_ = c.Name;
            }
            ImGui::EndPopup();
        }
        // Dotted command id, dimmed, inline after the label text (power users still see /
        // can learn the id; novices read the Title).
        const float labelW = ImGui::CalcTextSize(primary.c_str()).x + ImGui::CalcTextSize("  ").x * 2.0f +
                             ImGui::CalcTextSize(c.Summary.c_str()).x;
        ImGui::SameLine(labelW + ImGui::GetStyle().ItemSpacing.x * 2.0f);
        ImGui::TextDisabled("%s", c.Name.c_str());
        // Surface the bound combo, dimmed, right-aligned on the same row.
        const std::string combo = keybindings_ != nullptr ? BoundHotkeyDisplay(*keybindings_, c.Name) : std::string();
        if (!combo.empty()) {
            const float comboW = ImGui::CalcTextSize(combo.c_str()).x;
            ImGui::SameLine(rowWidth - comboW);
            ImGui::TextDisabled("%s", combo.c_str());
        }
        if (isSelected) {
            ImGui::SetItemDefaultFocus();
            // Only when the selection just moved: doing this every frame pins the list to
            // the selected row and makes the wheel and the scrollbar snap back.
            if (scrollToSelected_) {
                ImGui::SetScrollHereY(0.5f);
            }
        }
    }
    scrollToSelected_ = false;
    ImGui::EndChild();
}

std::string CommandPaletteUi::TakeQuickBindRequest() {
    std::string id;
    id.swap(requestQuickBindCommandId_);
    return id;
}

} // namespace cmd
} // namespace smatchet
