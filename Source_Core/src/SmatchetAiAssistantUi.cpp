#include "SmatchetAiAssistantUi.h"

#if defined(SMATCHET_WITH_AI)

#include "AiAssistantController.h"
#include "AiClientFactory.h"
#include "AiContextBuilder.h"
#include "AiModelCatalog.h"
#include "AiTypes.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "DictationInsertionRouter.h"
#include "Logger.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetUiSession.h"
#include "SpreadsheetState.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr float kInputRowsTall = 4.0f;
constexpr int kInputBufCap = 8 * 1024;

// Process-static char buffer for the chat input. Hoisted to namespace scope
// (originally a function-static inside DrawInputAndButtons) so the panel-level
// dictation register / unregister can address it without exposing a getter.
// Lifetime is for the life of the process; explicit clearing happens on Send
// and on panel close.
std::array<char, kInputBufCap> s_inputCharBuf{};
bool s_inputCharBufSeeded = false;

// UX pillar 2: ConfigManager::Save performs a synchronous JSON encode + atomic
// file replace; even when the user is only toggling a checkbox the resulting
// disk write can easily breach the 6.94 ms per-frame UI budget on a slow disk.
// Push the save to a detached worker thread. The TrackerConfig snapshot is
// captured by value so the worker is independent of the UI-thread cfg state.
// Each save is small (a few KB), and the rate of saves is bounded by user
// input frequency, so spawning a fresh thread per save is acceptable for the
// scenarios that hit this path (panel toggle, checkbox click, swap button).
// Move to a single coalescing worker if profiling shows churn.
void ScheduleConfigSaveDetached(const TrackerConfig& cfg) {
    TrackerConfig snapshot = cfg;
    std::thread([snapshot]() {
        try {
            ConfigManager::Save(snapshot);
        } catch (...) {
            // Save logs its own diagnostics; swallow exceptions so a detached
            // worker exit doesn't terminate the process.
        }
    }).detach();
}

void HydrateFromConfigOnce(UiDrawSession& d) {
    static bool s_hydrated = false;
    if (s_hydrated) {
        return;
    }
    s_hydrated = true;
    d.assistantPanelOpen = d.cfg.AssistantPanelOpen;
    if (d.assistantInputBuf.capacity() < static_cast<size_t>(kInputBufCap)) {
        d.assistantInputBuf.reserve(kInputBufCap);
    }
}

void PersistOpenStateImmediate(UiDrawSession& d) {
    if (d.cfg.AssistantPanelOpen != d.assistantPanelOpen) {
        d.cfg.AssistantPanelOpen = d.assistantPanelOpen;
        ScheduleConfigSaveDetached(d.cfg);
    }
}

void DrawHistoryArea(AppController& app, UiDrawSession& d, float availY) {
    (void)app;
    const float headerH = ImGui::GetTextLineHeightWithSpacing() * 1.4f;
    const float inputH = ImGui::GetFrameHeightWithSpacing() * kInputRowsTall +
                         ImGui::GetFrameHeightWithSpacing() * 1.2f; // input + buttons row
    const float errorStripH = d.assistantLastError.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 1.5f;
    const float bodyH = (std::max)(80.0f, availY - headerH - inputH - errorStripH);

    // Track whether the user is pinned to the tail BEFORE we render new content,
    // so streaming tokens auto-scroll without fighting a manual scroll-up.
    const bool wasAtTail = d.assistantAutoScrollAtTail;

    ImGui::BeginChild("##AiAssistantHistory", ImVec2(0.0f, bodyH), true);
    // Per-message rendering: role label + a SmallButton "Copy" that pushes the
    // full message text to the system clipboard via ImGui::SetClipboardText,
    // followed by a plain ImGui::TextWrapped of the content. Plain wrapped
    // text reflows with the panel width and never steals keyboard focus; the
    // Copy button is the single explicit copy mechanism (full-message,
    // exact byte copy regardless of soft-wrap).
    for (std::size_t i = 0; i < d.assistantHistory.size(); ++i) {
        const AiMessage& m = d.assistantHistory[i];
        const char* role = (m.Role == "user") ? "You" : "Assistant";
        ImGui::TextDisabled("%s", role);
        ImGui::SameLine();
        char btnId[48];
        std::snprintf(btnId, sizeof(btnId), "Copy##ai_msg_%zu", i);
        if (ImGui::SmallButton(btnId)) {
            ImGui::SetClipboardText(m.Content.c_str());
        }
        ImGui::SetItemTooltip("Copy message text to clipboard.");
        ImGui::TextWrapped("%s", m.Content.c_str());
        ImGui::Separator();
    }
    if (d.assistantInFlight && !d.assistantStreamBuf.empty()) {
        ImGui::TextDisabled("Assistant (streaming...)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy##ai_stream")) {
            ImGui::SetClipboardText(d.assistantStreamBuf.c_str());
        }
        ImGui::SetItemTooltip("Copy the in-flight partial response to clipboard.");
        ImGui::TextWrapped("%s", d.assistantStreamBuf.c_str());
    }

    // Tail tracking: update for next frame based on user's current scroll.
    const float scrollY = ImGui::GetScrollY();
    const float scrollMax = ImGui::GetScrollMaxY();
    d.assistantAutoScrollAtTail = (scrollMax <= 0.0f) || (scrollY >= scrollMax - 1.0f);
    if (wasAtTail) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void DrawErrorStrip(UiDrawSession& d) {
    if (d.assistantLastError.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
    ImGui::TextWrapped("%s", d.assistantLastError.c_str());
    ImGui::PopStyleColor();
}

void DrawContextBlockCheckboxes(UiDrawSession& d) {
    // Five-checkbox row driving the `cfg.AssistantContextBlock*` toggles shipped in
    // Phase A'. Changes persist to disk immediately so the next launch reflects the
    // user's pick; per-block defaults are all `true`.
    bool dirty = false;
    auto draw = [&](const char* label, bool& flag) {
        if (ImGui::Checkbox(label, &flag)) {
            dirty = true;
        }
    };
    draw("Selection##AiCtxSel", d.cfg.AssistantContextBlockSelection);
    ImGui::SameLine();
    draw("Visible##AiCtxVis", d.cfg.AssistantContextBlockVisibleRows);
    ImGui::SameLine();
    draw("Ticket##AiCtxTicket", d.cfg.AssistantContextBlockActiveTicket);
    ImGui::SameLine();
    draw("View##AiCtxView", d.cfg.AssistantContextBlockActiveView);
    ImGui::SameLine();
    draw("Audit##AiCtxAudit", d.cfg.AssistantContextBlockAuditTrail);
    if (dirty) {
        ScheduleConfigSaveDetached(d.cfg);
    }
}

std::vector<AiContextBlock> BuildSendContext(AppController& app, const UiDrawSession& d,
                                             const ViewDefinition* activeView) {
    AiContextBuilder::Inputs inputs;
    inputs.Tickets = app.GetActiveTicketsSnapshot();
    inputs.SortedIndices = &d.cachedSortedIndices;
    inputs.SelectedRows = &d.gridState.RectSel.Rows;
    inputs.ActiveIssueId = d.gridState.ActiveIssueId;
    inputs.ActiveView = activeView;
    // VisibleRows = the same sort-order list (capped internally at kRowsCap). Phase B
    // intentionally does not yet plumb the precise top-of-viewport range — using the
    // sorted-index list gives a deterministic "top N rows" approximation matching the
    // grid's natural rendering order.
    inputs.VisibleRows = &d.cachedSortedIndices;
    inputs.EnableSelection = d.cfg.AssistantContextBlockSelection;
    inputs.EnableVisibleRows = d.cfg.AssistantContextBlockVisibleRows;
    inputs.EnableActiveTicket = d.cfg.AssistantContextBlockActiveTicket;
    inputs.EnableActiveView = d.cfg.AssistantContextBlockActiveView;
    inputs.EnableAuditTrail = d.cfg.AssistantContextBlockAuditTrail;
    // UX pillar 2: BackendAuditTrail::ReadRecentEvents performs synchronous
    // SQLite + filesystem reads that must not run on the UI thread. The
    // sentinel body is replaced on the worker thread inside
    // AiAssistantController::RunRequest before the request is dispatched.
    inputs.DeferAuditTrailFetch = true;
    return AiContextBuilder::BuildAll(inputs);
}

// Returns true when the user submitted (Enter pressed without Ctrl). Sends are dispatched here so
// the keyboard path matches the Send button click; the input buffer is cleared + focus restored.
bool DrawInputAndButtons(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    // Char buffer for InputTextMultiline lives at namespace scope so the
    // panel-open / panel-close path can drive dictation register / unregister
    // without exposing a getter. Mirroring through a std::string per-frame
    // keeps the rest of the codepaths (Send-button snapshot, Lua glue) free
    // of ImGui-specific resizable callbacks. Pre-seeded from
    // d.assistantInputBuf the first time the panel renders so Lua-supplied
    // text (Phase E) survives a panel reopen.
    //
    // Re-seed when (a) first frame, or (b) the model-side `assistantInputBuf`
    // diverged from the char buffer (e.g. Lua glue poked it between frames,
    // or the panel was closed + reopened and external code edited the field).
    // Without this check, the char buffer kept the previous session's text
    // and Lua-supplied input on Phase E was silently lost.
    const std::size_t bufLen = std::strlen(s_inputCharBuf.data());
    const bool divergedFromModel = (d.assistantInputBuf.size() != bufLen) ||
                                   (std::memcmp(s_inputCharBuf.data(), d.assistantInputBuf.data(), bufLen) != 0);
    if (!s_inputCharBufSeeded || divergedFromModel) {
        s_inputCharBufSeeded = true;
        const size_t copy = (std::min)(d.assistantInputBuf.size(), s_inputCharBuf.size() - 1);
        std::memcpy(s_inputCharBuf.data(), d.assistantInputBuf.data(), copy);
        s_inputCharBuf[copy] = '\0';
    }

    const float inputH = ImGui::GetTextLineHeight() * kInputRowsTall;

    // Pillar 4 (aspirational keyboard-nav): Enter sends, Ctrl+Enter inserts newline.
    // ImGuiInputTextFlags_CtrlEnterForNewLine inverts the default multiline Enter
    // semantics so a bare Enter submits and Ctrl+Enter inserts a line break — see
    // imgui.h flag docs. EnterReturnsTrue makes the call return true on submit.
    const ImGuiInputTextFlags inputFlags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine;
    const bool enterSubmitted = ImGui::InputTextMultiline("##AiAssistantInput", s_inputCharBuf.data(),
                                                          s_inputCharBuf.size(), ImVec2(-1.0f, inputH), inputFlags);
    // Mirror char-buf back into the string field every frame so the Send-button click
    // below + the Lua glue see the latest value with no separate poke.
    d.assistantInputBuf.assign(s_inputCharBuf.data());

    AiAssistantController* ctrl = app.HasAiAssistantController() ? &app.GetAiAssistantController() : nullptr;

    const bool sendDisabled = d.assistantInFlight || d.assistantInputBuf.empty() || ctrl == nullptr;

    auto dispatchSend = [&]() {
        const uint64_t turnGen = ++d.assistantTurnGen;
        d.assistantInFlight = true;
        d.assistantStreamBuf.clear();
        d.assistantLastError.clear();
        AiMessage userMsg;
        userMsg.Role = "user";
        userMsg.Content = d.assistantInputBuf;
        d.assistantHistory.push_back(std::move(userMsg));
        std::string snapshot = d.assistantInputBuf;
        d.assistantInputBuf.clear();
        std::memset(s_inputCharBuf.data(), 0, s_inputCharBuf.size());
        if (ctrl) {
            // Phase C: build the 5-block auto-context snapshot on the UI thread (where
            // all the source state lives) and pass it through Submit. The controller
            // then concatenates these + the agents.md prefix on the worker before
            // calling IAiClient::SendStreaming. Disabled blocks contribute empty bodies
            // — the controller skips empty-body entries when emitting tag wrappers.
            std::vector<AiContextBlock> context = BuildSendContext(app, d, activeView);
            ctrl->Submit(turnGen, std::move(snapshot), std::move(context), d.assistantPerTurnModel,
                         d.assistantPerTurnEffort);
        }
        d.assistantAutoScrollAtTail = true;
    };

    bool submittedByKey = false;
    if (enterSubmitted && !sendDisabled) {
        dispatchSend();
        // Re-focus the same multiline input so the user can keep typing without
        // clicking back in. -1 targets the previously-submitted item.
        ImGui::SetKeyboardFocusHere(-1);
        submittedByKey = true;
    }

    if (sendDisabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send")) {
        dispatchSend();
    }
    if (sendDisabled) {
        ImGui::EndDisabled();
    }

    if (d.assistantInFlight) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (ctrl) {
                ctrl->Cancel();
            }
        }
    }

    // Compact help so the keybinding is discoverable on first view.
    ImGui::SameLine();
    ImGui::TextDisabled("Enter sends, Ctrl+Enter = newline");

    return submittedByKey;
}

} // namespace

void SmatchetDrawAiAssistantPanel(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    HydrateFromConfigOnce(d);
    if (!d.assistantPanelOpen) {
        // Drop the chat-input registration with the dictation router so the
        // panel-closed state never receives transcribed text. The wrapper-level
        // hook would unregister on blur anyway, but the panel-level explicit
        // call is the belt to the wrapper's suspenders.
        g_dictationRouter.UnregisterInputText(s_inputCharBuf.data());
        // Persist a closed state at most once per close event (idempotent if already false).
        PersistOpenStateImmediate(d);
        return;
    }
    // Register the chat-input buffer with the dictation router for the
    // duration of the panel being open. Re-registering each frame is cheap
    // (router treats the buf pointer as an idempotent key).
    g_dictationRouter.RegisterInputText(s_inputCharBuf.data(), s_inputCharBuf.size(), nullptr);

    // Pillar 2 (UI never freezes): dock-integrated window — ImGui's dock manager
    // owns layout, sizing, and the resize/swap chrome. The panel attaches to the
    // primary side bar (left) by default and migrates to the secondary side bar
    // (right) when the user toggles the swap button. A pending-side request fires
    // SetNextWindowDockID with ImGuiCond_Always so the move actually takes effect;
    // otherwise FirstUseEver lets the user's saved imgui.ini state win.
    const ImGuiID primaryDockId = SmatchetDockNodeIds::kPrimarySideBar;
    const ImGuiID secondaryDockId = SmatchetDockNodeIds::kSecondarySideBar;
    const ImGuiID targetDockId = d.cfg.AssistantPanelOnSecondarySide ? secondaryDockId : primaryDockId;
    if (d.assistantPendingSideSwap) {
        ImGui::SetNextWindowDockID(targetDockId, ImGuiCond_Always);
        d.assistantPendingSideSwap = false;
    } else {
        ImGui::SetNextWindowDockID(targetDockId, ImGuiCond_FirstUseEver);
    }

    if (d.requestAssistantFocus) {
        ImGui::SetNextWindowFocus();
    }

    // The panel is now a dockable, resizable window — drop the floating-only flags
    // (NoDocking / NoSavedSettings / NoTitleBar / NoMove / NoResize). NoCollapse is
    // kept because dock-tab collapse fights the open/close persistence contract.
    const ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse;

    if (!ImGui::Begin("Smatchet Assistant", &d.assistantPanelOpen, kFlags)) {
        ImGui::End();
        if (d.requestAssistantFocus) {
            d.requestAssistantFocus = false;
        }
        // Panel hidden (collapsed / docked-tab inactive) — drop dictation registration.
        g_dictationRouter.UnregisterInputText(s_inputCharBuf.data());
        PersistOpenStateImmediate(d);
        return;
    }
    if (d.requestAssistantFocus) {
        ImGui::SetWindowFocus();
        d.requestAssistantFocus = false;
    }

    // Header strip: provider + swap-side toggle.
    {
        AiAssistantController* ctrl = app.HasAiAssistantController() ? &app.GetAiAssistantController() : nullptr;
        const std::string provider = ctrl ? ctrl->GetActiveProviderName() : std::string();
        if (!provider.empty()) {
            ImGui::TextDisabled("(%s)", provider.c_str());
        } else {
            ImGui::TextDisabled("(no provider)");
        }
    }
    ImGui::SameLine();
    {
        // The swap button label inverts to telegraph the destination (where the panel
        // WILL move). When currently on the left primary side, the label reads "Right"
        // because clicking moves it to the right. Tooltip clarifies the action.
        const bool onRight = d.cfg.AssistantPanelOnSecondarySide;
        const char* swapLabel = onRight ? "<- Left" : "Right ->";
        if (ImGui::SmallButton(swapLabel)) {
            d.cfg.AssistantPanelOnSecondarySide = !onRight;
            d.assistantPendingSideSwap = true;
            // Moving to the right side bar slot requires the slot to actually be
            // visible; otherwise the dock node may be empty and the new tab has
            // nowhere to land. Mirror the existing View-menu toggle pattern so
            // users don't get a vanishing panel.
            if (d.cfg.AssistantPanelOnSecondarySide && !d.cfg.ShowSecondarySideBar) {
                d.cfg.ShowSecondarySideBar = true;
            }
            ScheduleConfigSaveDetached(d.cfg);
        }
        ImGui::SetItemTooltip(onRight ? "Move panel to the left primary side bar."
                                      : "Move panel to the right secondary side bar.");
    }

    // --- Per-turn Model + Effort overrides (chat-window header row 2). ---
    //
    // Empty `assistantPerTurnModel` / `assistantPerTurnEffort` mean "use the
    // Preferences default for the active provider". The Combos write directly
    // into the session strings; the next Send picks them up via Submit.
    {
        // Cast cfg int → AiProvider enum; clamp out-of-range to OpenAi (same
        // pattern as AiPrefsValidator::ClampProvider — kept local since that
        // helper lives in an anonymous namespace).
        AiProvider activeProvider = AiProvider::OpenAi;
        switch (d.cfg.AiProviderKind) {
        case 1:
            activeProvider = AiProvider::Anthropic;
            break;
        case 2:
            activeProvider = AiProvider::OllamaOpenAiCompat;
            break;
        case 3:
            activeProvider = AiProvider::OllamaNative;
            break;
        case 0:
        default:
            activeProvider = AiProvider::OpenAi;
            break;
        }
        const std::vector<smatchet::ai::ModelOption> catalog = smatchet::ai::KnownModels(activeProvider);
        // Display list = [<default> sentinel, model 1, model 2, ...]. Picking the
        // sentinel clears the per-turn override.
        std::vector<std::string> displayStrings;
        displayStrings.reserve(catalog.size() + 1);
        displayStrings.push_back(std::string("<default model>"));
        std::transform(catalog.begin(), catalog.end(), std::back_inserter(displayStrings),
                       [](const smatchet::ai::ModelOption& m) { return m.DisplayName; });
        std::vector<const char*> displayPtrs;
        displayPtrs.reserve(displayStrings.size());
        std::transform(displayStrings.begin(), displayStrings.end(), std::back_inserter(displayPtrs),
                       [](const std::string& s) { return s.c_str(); });
        int comboIdx = 0;
        // When the saved per-turn override doesn't match any entry in the active
        // provider's catalog (e.g. user switched providers leaving a stale id, or
        // typed a custom name from another build), fall back to free-form input
        // so the UI accurately reflects what the next Send will actually use —
        // showing `<default model>` while a hidden non-catalog override is still
        // active is a major UX trap (CodeRabbit comment 3255682299).
        bool useFreeformModelInput = catalog.empty();
        if (!d.assistantPerTurnModel.empty() && !catalog.empty()) {
            auto it = std::find_if(catalog.begin(), catalog.end(),
                                   [&](const smatchet::ai::ModelOption& m) { return m.Id == d.assistantPerTurnModel; });
            if (it != catalog.end()) {
                comboIdx = 1 + static_cast<int>(std::distance(catalog.begin(), it));
            } else {
                useFreeformModelInput = true;
            }
        }
        ImGui::SetNextItemWidth(ImGui::GetTextLineHeight() * 12.0f);
        if (useFreeformModelInput) {
            // Provider has no published catalog (Ollama variants). Free-form
            // InputText sized to look like the Combo above.
            char modelBuf[256] = {};
            std::snprintf(modelBuf, sizeof(modelBuf), "%s", d.assistantPerTurnModel.c_str());
            if (ImGui::InputTextWithHint("##AiTurnModel", "<default model>", modelBuf, sizeof(modelBuf))) {
                d.assistantPerTurnModel = modelBuf;
            }
            ImGui::SetItemTooltip("Per-turn model override. Leave blank to use the Preferences-saved value for this "
                                  "provider.");
        } else {
            if (ImGui::Combo("##AiTurnModel", &comboIdx, displayPtrs.data(), static_cast<int>(displayPtrs.size()))) {
                if (comboIdx == 0) {
                    d.assistantPerTurnModel.clear();
                } else {
                    d.assistantPerTurnModel = catalog.at(static_cast<std::size_t>(comboIdx - 1)).Id;
                }
            }
            ImGui::SetItemTooltip("Per-turn model override. Pick <default model> to inherit the Preferences value.");
        }
        ImGui::SameLine();
        // Reasoning-effort Combo. Same 4-value enum as cfg.AiReasoningEffort.
        const char* kEffortLabels[] = {"<default effort>", "Low", "Medium", "High"};
        const char* kEffortIds[] = {"", "low", "medium", "high"};
        int effortIdx = 0;
        for (int i = 1; i < 4; ++i) {
            if (d.assistantPerTurnEffort == kEffortIds[i]) {
                effortIdx = i;
                break;
            }
        }
        ImGui::SetNextItemWidth(ImGui::GetTextLineHeight() * 10.0f);
        if (ImGui::Combo("##AiTurnEffort", &effortIdx, kEffortLabels, 4)) {
            d.assistantPerTurnEffort = kEffortIds[effortIdx];
        }
        ImGui::SetItemTooltip("Per-turn reasoning effort. Applied as the OpenAI `reasoning_effort` parameter; "
                              "providers that don't understand the param ignore it.");
    }
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // Reserve a row for the per-block context checkboxes drawn just above the input strip.
    const float checkboxRowH = ImGui::GetFrameHeightWithSpacing();
    DrawHistoryArea(app, d, avail.y - checkboxRowH);
    DrawErrorStrip(d);
    DrawContextBlockCheckboxes(d);
    DrawInputAndButtons(app, d, activeView);

    ImGui::End();

    // Persist toggle changes (close X click, View-menu toggle externally) on the same
    // tick so the open/closed state survives an app crash mid-frame.
    PersistOpenStateImmediate(d);
}

#endif // SMATCHET_WITH_AI
