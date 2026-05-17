#include "SmatchetAiAssistantUi.h"

#if defined(SMATCHET_WITH_AI)

#include "AiAssistantController.h"
#include "AiTypes.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"

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

// Debounced ConfigManager save tracker — width drags fire IsItemDeactivatedAfterEdit on
// release, so we only persist on commit. Open/close toggles persist immediately.
struct PanelPersistState {
    bool pendingWidthSave = false;
    std::chrono::steady_clock::time_point pendingWidthSaveAt{};
};

PanelPersistState& Persist() {
    static PanelPersistState s;
    return s;
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
        ConfigManager::Save(d.cfg);
    }
}

void PersistWidthDebounced(UiDrawSession& d) {
    // Width drag committed (mouse released) — persist immediately. The "debounced"
    // name reflects the contract: only commit on release, never every frame mid-drag.
    if (std::fabs(d.cfg.AssistantPanelWidth - d.assistantPanelWidthLive) > 0.5f) {
        d.cfg.AssistantPanelWidth = d.assistantPanelWidthLive;
        ConfigManager::Save(d.cfg);
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

void DrawInputAndButtons(AppController& app, UiDrawSession& d) {
    // Use a process-static char buffer for InputTextMultiline. Mirroring through a
    // std::string per-frame keeps the rest of the codepaths (Send-button snapshot,
    // Lua glue) free of ImGui-specific resizable callbacks. Pre-seeded from
    // d.assistantInputBuf the first time the panel renders so Lua-supplied text
    // (Phase E) survives a panel reopen.
    static std::array<char, kInputBufCap> s_inputCharBuf{};
    static bool s_seeded = false;
    if (!s_seeded) {
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
            ctrl->Submit(turnGen, std::move(snapshot), {});
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

void SmatchetDrawAiAssistantPanel(AppController& app, UiDrawSession& d) {
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
    DrawHistoryArea(app, d, avail.y);
    DrawErrorStrip(d);
    DrawInputAndButtons(app, d);
    DrawResizeGrip(d, workSize.y);

    ImGui::End();

    // Persist toggle changes (close X click, View-menu toggle externally) on the same
    // tick so the open/closed state survives an app crash mid-frame.
    PersistOpenStateImmediate(d);
}

#endif // SMATCHET_WITH_AI
