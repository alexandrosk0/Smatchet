#include "SmatchetAiAssistantUi.h"

#if defined(SMATCHET_WITH_AI)

#include "AiAssistantController.h"
#include "AiContextBuilder.h"
#include "AiTypes.h"
#include "AppController.h"
#include "ConfigManager.h"
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

constexpr float kMinPanelWidth = 280.0f;
constexpr float kMaxPanelWidthAbs = 720.0f;
constexpr float kMaxPanelFraction = 0.45f;
constexpr float kResizeGripWidth = 4.0f;
constexpr float kInputRowsTall = 4.0f;
constexpr int kInputBufCap = 8 * 1024;

float ClampedWidth(float requested, float workSizeX) {
    const float maxW = (std::min)(kMaxPanelWidthAbs, workSizeX * kMaxPanelFraction);
    return (std::max)(kMinPanelWidth, (std::min)(requested, maxW));
}

// UX pillar 2: ConfigManager::Save performs a synchronous JSON encode + atomic
// file replace; even when the user is only toggling a checkbox the resulting
// disk write can easily breach the 6.94 ms per-frame UI budget on a slow disk.
// Push the save to a detached worker thread. The TrackerConfig snapshot is
// captured by value so the worker is independent of the UI-thread cfg state.
// Each save is small (a few KB), and the rate of saves is bounded by user
// input frequency, so spawning a fresh thread per save is acceptable for the
// scenarios that hit this path (panel toggle, checkbox click, width-drag
// release). Move to a single coalescing worker if profiling shows churn.
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
    d.assistantPanelWidthLive = d.cfg.AssistantPanelWidth;
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

void PersistWidthDebounced(UiDrawSession& d) {
    // Width drag committed (mouse released) — persist immediately. The "debounced"
    // name reflects the contract: only commit on release, never every frame mid-drag.
    if (std::fabs(d.cfg.AssistantPanelWidth - d.assistantPanelWidthLive) > 0.5f) {
        d.cfg.AssistantPanelWidth = d.assistantPanelWidthLive;
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
    for (const AiMessage& m : d.assistantHistory) {
        const char* role = (m.Role == "user") ? "You" : "Assistant";
        ImGui::TextDisabled("%s", role);
        ImGui::TextWrapped("%s", m.Content.c_str());
        ImGui::Separator();
    }
    if (d.assistantInFlight && !d.assistantStreamBuf.empty()) {
        ImGui::TextDisabled("Assistant (streaming...)");
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

void DrawInputAndButtons(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    // Use a process-static char buffer for InputTextMultiline. Mirroring through a
    // std::string per-frame keeps the rest of the codepaths (Send-button snapshot,
    // Lua glue) free of ImGui-specific resizable callbacks. Pre-seeded from
    // d.assistantInputBuf the first time the panel renders so Lua-supplied text
    // (Phase E) survives a panel reopen.
    static std::array<char, kInputBufCap> s_inputCharBuf{};
    static bool s_seeded = false;
    // Re-seed when (a) first frame, or (b) the model-side `assistantInputBuf`
    // diverged from the char buffer (e.g. Lua glue poked it between frames, or
    // the panel was closed + reopened and external code edited the field).
    // Without this check, the char buffer kept the previous session's text and
    // Lua-supplied input on Phase E was silently lost.
    const std::size_t bufLen = std::strlen(s_inputCharBuf.data());
    const bool divergedFromModel = (d.assistantInputBuf.size() != bufLen) ||
                                   (std::memcmp(s_inputCharBuf.data(), d.assistantInputBuf.data(), bufLen) != 0);
    if (!s_seeded || divergedFromModel) {
        s_seeded = true;
        const size_t copy = (std::min)(d.assistantInputBuf.size(), s_inputCharBuf.size() - 1);
        std::memcpy(s_inputCharBuf.data(), d.assistantInputBuf.data(), copy);
        s_inputCharBuf[copy] = '\0';
    }

    const float inputH = ImGui::GetTextLineHeight() * kInputRowsTall;
    ImGui::InputTextMultiline("##AiAssistantInput", s_inputCharBuf.data(), s_inputCharBuf.size(),
                              ImVec2(-1.0f, inputH));
    // Mirror char-buf back into the string field every frame so the Send-button click
    // below + the Lua glue see the latest value with no separate poke.
    d.assistantInputBuf.assign(s_inputCharBuf.data());

    AiAssistantController* ctrl = app.HasAiAssistantController() ? &app.GetAiAssistantController() : nullptr;

    const bool sendDisabled = d.assistantInFlight || d.assistantInputBuf.empty() || ctrl == nullptr;
    if (sendDisabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send")) {
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
            ctrl->Submit(turnGen, std::move(snapshot), std::move(context));
        }
        d.assistantAutoScrollAtTail = true;
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
}

void DrawResizeGrip(UiDrawSession& d, float panelHeight) {
    // Place a thin invisible button at the panel's LEFT edge. The panel is right-anchored,
    // so dragging this grip leftward grows the panel.
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##AiAssistantResizeGrip", ImVec2(kResizeGripWidth, panelHeight));
    if (ImGui::IsItemActive()) {
        // GetMouseDragDelta is relative to the drag start; using MouseDelta keeps the
        // calculation incremental and avoids snap-jumps when the cursor leaves the grip.
        const float dx = ImGui::GetIO().MouseDelta.x;
        // Right-anchored: leftward drag (negative dx in screen-x) grows the panel.
        d.assistantPanelWidthLive -= dx;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float workW = vp ? vp->WorkSize.x : 1920.0f;
        d.assistantPanelWidthLive = ClampedWidth(d.assistantPanelWidthLive, workW);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        PersistWidthDebounced(d);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
}

} // namespace

void SmatchetDrawAiAssistantPanel(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    HydrateFromConfigOnce(d);
    if (!d.assistantPanelOpen) {
        // Persist a closed state at most once per close event (idempotent if already false).
        PersistOpenStateImmediate(d);
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        return;
    }
    const ImVec2 workPos = vp->WorkPos;
    const ImVec2 workSize = vp->WorkSize;
    const float w = ClampedWidth(d.assistantPanelWidthLive, workSize.x);
    d.assistantPanelWidthLive = w; // clamp-back so a saved over-cap width doesn't persist

    ImGui::SetNextWindowPos(ImVec2(workPos.x + workSize.x - w, workPos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, workSize.y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);

    const ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (d.requestAssistantFocus) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("##SmatchetAssistantPanel", &d.assistantPanelOpen, kFlags)) {
        ImGui::End();
        if (d.requestAssistantFocus) {
            d.requestAssistantFocus = false;
        }
        PersistOpenStateImmediate(d);
        return;
    }
    if (d.requestAssistantFocus) {
        ImGui::SetWindowFocus();
        d.requestAssistantFocus = false;
    }

    // Header strip
    ImGui::TextUnformatted("Smatchet Assistant");
    ImGui::SameLine();
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
    // Right-align the close button.
    {
        const float btnW = ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - btnW);
        if (ImGui::SmallButton("X")) {
            d.assistantPanelOpen = false;
        }
    }
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // Reserve a row for the per-block context checkboxes drawn just above the input strip.
    const float checkboxRowH = ImGui::GetFrameHeightWithSpacing();
    DrawHistoryArea(app, d, avail.y - checkboxRowH);
    DrawErrorStrip(d);
    DrawContextBlockCheckboxes(d);
    DrawInputAndButtons(app, d, activeView);
    DrawResizeGrip(d, workSize.y);

    ImGui::End();

    // Persist toggle changes (close X click, View-menu toggle externally) on the same
    // tick so the open/closed state survives an app crash mid-frame.
    PersistOpenStateImmediate(d);
}

#endif // SMATCHET_WITH_AI
