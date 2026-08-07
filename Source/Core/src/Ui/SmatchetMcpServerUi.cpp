#include "SmatchetMcpServerUi.h"

#if defined(SMATCHET_WITH_MCP)

#include "AppController.h"
#include "ConfigManager.h"
#include "McpServerStatus.h"
#include "PluginHost.h"
#include "McpServerInfoTextPure.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

ImVec4 BlendRgba(const ImVec4& a, const ImVec4& b, float t) {
    const float u = (std::max)(0.f, (std::min)(1.f, t));
    return ImVec4(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, a.w + (b.w - a.w) * u);
}

/** Read-only multiline so the user can drag-select and Ctrl+C (plain Text/BulletText is not selectable). */
void DrawCopyableMultiline(const char* id, const std::string& text, float heightPx, std::vector<char>& scratch) {
    const size_t need = text.size() + 1u;
    if (scratch.size() < need) {
        scratch.resize(need + 1024u);
    }
    if (!text.empty()) {
        std::memcpy(scratch.data(), text.data(), text.size());
    }
    scratch[text.size()] = '\0';
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_ReadOnly;
    ImGui::InputTextMultiline(id, scratch.data(), static_cast<int>(scratch.size()), ImVec2(-1.0f, heightPx), flags);
}

void SaveMcpWindowLayoutDebounced(UiDrawSession& d) {
    static std::chrono::steady_clock::time_point s_lastWrite{};
    static float s_infoH = std::numeric_limits<float>::quiet_NaN();
    static float s_actH = 0.f;

    const auto differs = [](float a, float b) { return std::fabs(a - b) > 0.75f; };

    const bool haveSnap = !std::isnan(s_infoH);
    const bool diff = !haveSnap || differs(d.cfg.McpServerInfoPanelHeightPx, s_infoH) ||
                      differs(d.cfg.McpServerActivityPanelHeightPx, s_actH);
    if (!diff) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (haveSnap && now - s_lastWrite < std::chrono::milliseconds(350)) {
        return;
    }
    s_lastWrite = now;
    s_infoH = d.cfg.McpServerInfoPanelHeightPx;
    s_actH = d.cfg.McpServerActivityPanelHeightPx;
    ConfigManager::Save(d.cfg);
}

// SMATCHET_DEVIATION(rule=duplication; reason=the MCP and Lua-console window-layout helpers are
// long-standing structural twins (debounced save, clamp-to-work-area, prepare/repair); the dock
// -liveness guard only made the delta gate re-notice them. Unifying would make Source/Plugins/
// LuaConsole depend on Source/Core/src/Ui window-layout internals, which AGENTS.md calls out as
// the CRITICAL kind of DRY refactor — coupling independent subsystems; owner=ui-host;
// revisit=2026-12-31)
ImVec2 ClampMcpWindowPos(const ImVec2& pos, const ImVec2& size) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        return pos;
    }
    const float maxX = (std::max)(vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - size.x);
    const float maxY = (std::max)(vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - size.y);
    return ImVec2((std::max)(vp->WorkPos.x, (std::min)(pos.x, maxX)),
                  (std::max)(vp->WorkPos.y, (std::min)(pos.y, maxY)));
}

void PrepareMcpWindowLayout(UiDrawSession& d) {
    // 0 when the bottom panel no longer exists — see EnsureDockSlotAlive. Docking to it then
    // would mint an orphan node instead of failing, so the write is skipped and the window
    // stays visibly floating.
    const ImGuiID slot = SmatchetDockNodeIds::EnsureDockSlotAlive(SmatchetDockNodeIds::kBottomPanel);
    const bool docked = slot != 0;
    // The pending entry is only consumed when there is a live slot to honour it; on a dead slot
    // it is left in place so the redock still fires once the node comes back. Same policy as
    // PrepareLuaWindowLayout and repairTopLevelWindow — all three latches agree.
    const bool requested = d.pendingReDockWindows.count("mcp") > 0 || d.layoutForceDefaultsFrames > 0;
    if (docked) {
        if (requested) {
            d.pendingReDockWindows.erase("mcp");
            ImGui::SetNextWindowDockID(slot, ImGuiCond_Always);
        } else if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
            ImGui::SetNextWindowDockID(slot, ImGuiCond_FirstUseEver);
        }
    }
    // The forced dock owns the geometry; an Always pos write alongside it sets PosUndock and
    // cancels the dock outright (taking the node with it if this was its last tab). Only the
    // floating fallback may force a rect.
    const bool forceRect = requested && !docked;
    // Structural twin of PrepareLuaWindowLayout's default-rect/clamp block; see the note above
    // ClampMcpWindowPos for why unifying the two would couple independent subsystems.
    // SMATCHET_DEVIATION(rule=duplication; reason=twin of PrepareLuaWindowLayout; owner=ui-host; revisit=2026-12-31)
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        ImGui::SetNextWindowSize(ImVec2(480.0f, 520.0f), ImGuiCond_FirstUseEver);
        return;
    }
    const ImVec2 size((std::min)(520.0f, (std::max)(420.0f, vp->WorkSize.x * 0.34f)),
                      (std::min)(640.0f, (std::max)(420.0f, vp->WorkSize.y * 0.70f)));
    const ImVec2 pos(vp->WorkPos.x + vp->WorkSize.x - size.x - 24.0f, vp->WorkPos.y + 48.0f);
    const ImGuiCond cond = forceRect ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ClampMcpWindowPos(pos, size), cond);
    ImGui::SetNextWindowSize(size, cond);
}

void RepairMcpWindowLayout(UiDrawSession& d) {
    if (ImGui::IsWindowDocked()) {
        return;
    }
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
        d.pendingReDockWindows.insert("mcp");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        return;
    }
    ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 pos = ImGui::GetWindowPos();
    const bool bad = size.x < 360.0f || size.y < 320.0f || pos.x > vp->WorkPos.x + vp->WorkSize.x - 96.0f ||
                     pos.y > vp->WorkPos.y + vp->WorkSize.y - 72.0f || pos.x + size.x < vp->WorkPos.x + 96.0f ||
                     pos.y + size.y < vp->WorkPos.y + 72.0f || d.layoutForceDefaultsFrames > 0;
    if (!bad) {
        return;
    }
    size.x = (std::min)((std::max)(size.x, 420.0f), vp->WorkSize.x * 0.92f);
    size.y = (std::min)((std::max)(size.y, 420.0f), vp->WorkSize.y * 0.88f);
    ImGui::SetWindowPos(
        ClampMcpWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 24.0f, vp->WorkPos.y + 48.0f), size),
        ImGuiCond_Always);
    ImGui::SetWindowSize(size, ImGuiCond_Always);
}

} // namespace

void SmatchetDrawMcpServerPanel(const AppController& app, const TrackerConfig& cfgOnDisk, UiDrawSession& d) {
    static std::vector<char> s_infoScratch;
    static std::vector<char> s_logScratch;

    ImGui::TextUnformatted("Select text below and use Ctrl+C to copy.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Recent actions");
    const std::vector<std::string> log = app.CopyMcpActivityLog();
    std::string logText;
    if (log.empty()) {
        logText = "No MCP activity yet.\n";
    } else {
        logText.reserve(log.size() * 64u);
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            const std::string& lineEntry = *it;
            logText += lineEntry;
            logText.push_back('\n');
        }
    }
    float actH = d.cfg.McpServerActivityPanelHeightPx;
    actH = (std::max)(80.0f, (std::min)(480.0f, actH));
    d.cfg.McpServerActivityPanelHeightPx = actH;

    const std::uint64_t trafficEp = app.GetMcpHttpTrafficEpoch();
    static std::uint64_t s_lastTrafficEp = 0;
    static std::chrono::steady_clock::time_point s_activityFlashUntil{};
    const auto nowFlash = std::chrono::steady_clock::now();
    if (trafficEp != s_lastTrafficEp) {
        if (trafficEp != 0) {
            s_activityFlashUntil = nowFlash + std::chrono::seconds(1);
        }
        s_lastTrafficEp = trafficEp;
    }
    float flashStrength = 0.f;
    if (nowFlash < s_activityFlashUntil) {
        const auto msLeft =
            std::chrono::duration_cast<std::chrono::milliseconds>(s_activityFlashUntil - nowFlash).count();
        flashStrength = static_cast<float>(msLeft) / 1000.f;
    }
    const int stylePush = (flashStrength > 0.001f) ? 2 : 0;
    if (stylePush != 0) {
        const ImVec4 frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        const ImVec4 warmFill(0.52f, 0.44f, 0.14f, 1.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, BlendRgba(frameBg, warmFill, flashStrength * 0.55f));
        const ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
        const ImVec4 warmBorder(0.95f, 0.78f, 0.22f, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Border, BlendRgba(border, warmBorder, flashStrength * 0.85f));
    }
    DrawCopyableMultiline("##mcp_activity_log", logText, actH, s_logScratch);
    if (stylePush != 0) {
        ImGui::PopStyleColor(stylePush);
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Diagnostics text assembly is pure (no ImGui) — built in McpServerInfoTextPure.h so the
    // config/runtime branch matrix is bucket-A testable. Resolve the runtime status snapshot
    // here; nullptr signals "no runtime plugin host registered".
    const ::PluginHost* ph = app.RuntimePluginHost();
    McpServerStatus runtimeStatus;
    const McpServerStatus* statusPtr = nullptr;
    if (ph != nullptr) {
        runtimeStatus = ph->GetMcpServerStatus();
        statusPtr = &runtimeStatus;
    }
    const std::string info = smatchet::mcp_ui::BuildMcpServerInfoText(cfgOnDisk, statusPtr);

    const float line = ImGui::GetTextLineHeightWithSpacing();
    float infoH = d.cfg.McpServerInfoPanelHeightPx;
    if (infoH < 80.0f) {
        infoH = line * 18.0f;
    }
    DrawCopyableMultiline("##mcp_server_info", info, infoH, s_infoScratch);

    ImGui::Spacing();
    const float splitGrabH = 7.0f;
    ImGui::InvisibleButton("##mcp_panel_split", ImVec2(ImGui::GetContentRegionAvail().x, splitGrabH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float next = d.cfg.McpServerInfoPanelHeightPx;
        if (next < 80.0f) {
            next = infoH;
        }
        next += ImGui::GetIO().MouseDelta.y;
        next = (std::max)(120.0f, (std::min)(900.0f, next));
        if (std::fabs(next - d.cfg.McpServerInfoPanelHeightPx) > 0.5f) {
            d.cfg.McpServerInfoPanelHeightPx = next;
            SaveMcpWindowLayoutDebounced(d);
        }
    }

    ImGui::TextDisabled("Edit MCP fields in Preferences → Integrations.");
}

void SmatchetDrawMcpServerWindow(const AppController& app, UiDrawSession& d) {
    if (!d.showMcpServerWindow) {
        return;
    }

    PrepareMcpWindowLayout(d);

    const bool wantFocus = d.requestMcpServerFocus;
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("MCP Server", &d.showMcpServerWindow)) {
        ImGui::End();
        if (wantFocus) {
            d.requestMcpServerFocus = false;
        }
        return;
    }
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestMcpServerFocus = false;
    }
    RepairMcpWindowLayout(d);

    SmatchetDrawMcpServerPanel(app, d.cfg, d);

    SaveMcpWindowLayoutDebounced(d);

    ImGui::End();
}

#endif // SMATCHET_WITH_MCP
