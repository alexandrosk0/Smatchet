#include "SmatchetStatusBarUi.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"
#include "SmatchetThemeIds.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <cstdio>
#include <string>

namespace {

static const char* ThemeIdName(ThemeId t) {
    switch (t) {
    case ThemeId::ModernDark:
        return "Modern Dark";
    case ThemeId::Vs2022Dark:
        return "VS 2022 Dark";
    case ThemeId::Vs2022Light:
        return "VS 2022 Light";
    case ThemeId::HighContrast:
        return "High Contrast";
    case ThemeId::NortonCommander:
        return "Norton Commander";
    case ThemeId::SmatchetDark:
    default:
        return "Smatchet Dark";
    }
}

static const char* ConnectivityLabel(AppController& app) {
    using State = AppController::TrackerConnectivityState;
    switch (app.GetLastTrackerConnectivityState()) {
    case State::AuthenticatedReachable:
        return "online";
    case State::ReachableAuthOrConfigError:
        return "auth error";
    case State::TransportDown:
        return "offline";
    case State::ServiceUnavailable:
        return "unavailable";
    case State::Unknown:
    default:
        return "unknown";
    }
}

} // namespace

void DrawStatusBar(AppController& app, const UiDrawSession& d) {
    const float barH = ::ImGui::GetFrameHeight();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (!::ImGui::BeginViewportSideBar("##StatusBar", ::ImGui::GetMainViewport(), ImGuiDir_Down, barH, flags)) {
        ::ImGui::End();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 0.0f));

    // Left side ---------------------------------------------------------------

    // Backend chip: Jira or Plane
    const std::string& rawType = d.cfg.TrackerType;
    const std::string trackerType = rawType.size() > 64u ? rawType.substr(0u, 64u) : rawType;
    const char* backend = trackerType.empty() ? "?" : trackerType.c_str();
    ImGui::TextUnformatted(backend);

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // Online / offline indicator
    const char* connLabel = ConnectivityLabel(app);
    ImGui::TextUnformatted(connLabel);

    // Queued-ops count (pending creates + pending field edits).
    // Field-edit count comes from d.cachedPendingFieldEditCount (refreshed once per frame
    // in SmatchetUI after TickOfflineFieldEdits) to avoid a SQLite SELECT on the render thread.
    {
        const size_t queuedOps = app.GetPendingCreateCount() + static_cast<size_t>(d.cachedPendingFieldEditCount);
        if (queuedOps > 0) {
            ImGui::SameLine();
            ImGui::TextUnformatted("|");
            ImGui::SameLine();
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d queued", static_cast<int>(queuedOps));
            ImGui::TextUnformatted(buf);
        }
    }

    // In-flight edit dot
    if (d.hasInFlightEdit) {
        ImGui::SameLine();
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::TextUnformatted("*");
        ImGui::PopStyleColor();
    }

    // Right side — font size, theme name, FPS ----------------------------------
    {
        char rightBuf[128];
        const float fps = ::ImGui::GetIO().Framerate;
        const char* themeName = ThemeIdName(d.cfg.Theme);
        std::snprintf(rightBuf, sizeof(rightBuf), "%dpt  %s  %.0f fps", d.cfg.FontSizePt, themeName,
                      static_cast<double>(fps));

        const float textW = ::ImGui::CalcTextSize(rightBuf).x;
        const float contentMaxX = ::ImGui::GetWindowContentRegionMax().x;
        const float xPos = contentMaxX - textW - 4.0f;
        if (xPos > ::ImGui::GetCursorPosX()) {
            ::ImGui::SetCursorPosX(xPos);
        } else {
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(rightBuf);
    }

    ImGui::PopStyleVar();
    ::ImGui::End();
}
