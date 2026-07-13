#include "SmatchetStatusBarUi.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SmatchetThemeIds.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <cctype>
#include <cstddef>
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
    case ThemeId::ImGuiDefaultDark:
        return "Classic Bright";
    case ThemeId::SmatchetDark:
        return "Smatchet Dark";
    default:
        return "Classic Bright";
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

// Hover explanation for the terse connectivity chip: what the state means and how to
// recover from it. The chip label stays short; the tooltip carries the detail.
static const char* ConnectivityTooltip(AppController& app) {
    using State = AppController::TrackerConnectivityState;
    switch (app.GetLastTrackerConnectivityState()) {
    case State::AuthenticatedReachable:
        return "Connected and authenticated to the tracker backend.";
    case State::ReachableAuthOrConfigError:
        return "The backend is reachable but rejected the credentials or configuration.\n"
               "Check the API token and connection settings in Preferences > Tracker.";
    case State::TransportDown:
        return "No network route to the backend. Edits are queued locally and sync\n"
               "automatically when the connection returns.";
    case State::ServiceUnavailable:
        return "The backend service is temporarily unavailable. Smatchet keeps retrying\n"
               "automatically; queued edits sync once it recovers.";
    case State::Unknown:
    default:
        return "Connectivity has not been probed yet.";
    }
}

// Friendly display name for the persistent backend chip: the raw config value is a
// lowercase type string ("jira") — map it to the product name shown elsewhere in the UI.
static std::string BackendDisplayName(const std::string& rawType) {
    std::string t = rawType.size() > 64u ? rawType.substr(0u, 64u) : rawType;
    std::string lower = t;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));
    }
    if (lower == "jira")
        return "Jira";
    if (lower == "plane")
        return "Plane";
    if (lower == "github")
        return "GitHub";
    if (lower == "linear")
        return "Linear";
    if (!t.empty()) {
        t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    }
    return t;
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

    // Backend chip: friendly product name, never the raw lowercase type string.
    const std::string backendName = BackendDisplayName(d.cfg.TrackerType);
    ImGui::TextUnformatted(backendName.empty() ? "Not connected" : backendName.c_str());
    if (::ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Active tracker backend. Change it in Preferences > Tracker.");
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // Online / offline indicator
    const char* connLabel = ConnectivityLabel(app);
    ImGui::TextUnformatted(connLabel);
    if (::ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", ConnectivityTooltip(app));
    }

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
            if (::ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Offline changes (new issues and field edits) waiting to sync.\n"
                                  "They are sent automatically when the backend is reachable.");
            }
        }
    }

    // In-flight edit indicator — worded, not a bare asterisk.
    if (d.hasInFlightEdit) {
        ImGui::SameLine();
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::TextUnformatted("Saving...");
        ImGui::PopStyleColor();
        if (::ImGui::IsItemHovered()) {
            ImGui::SetTooltip("An edit is being written to the backend.");
        }
    }

    // Unread-error badge: persistent trail for error toasts (which are otherwise easy to
    // miss). Click opens the Notification Center; cleared when the center is viewed.
    {
        const int unreadErrors = SmatchetToastManager::Instance().UnreadErrorCount();
        if (unreadErrors > 0) {
            ImGui::SameLine();
            ImGui::TextUnformatted("|");
            ImGui::SameLine();
            char errBuf[48];
            std::snprintf(errBuf, sizeof(errBuf), unreadErrors == 1 ? "%d error" : "%d errors", unreadErrors);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
            ImGui::TextUnformatted(errBuf);
            ImGui::PopStyleColor();
            if (::ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Recent errors — click to open Notifications.");
                if (::ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    SmatchetToastManager::Instance().RequestOpenCenter();
                }
            }
        }
    }

    // Right side — font size + theme name. FPS is a developer metric and lives in the
    // Performance window (View > Performance), not the always-on status bar.
    {
        char rightBuf[128];
        const char* themeName = ThemeIdName(d.cfg.Theme);
        std::snprintf(rightBuf, sizeof(rightBuf), "%dpt  %s", d.cfg.FontSizePt, themeName);

        const float textW = ::ImGui::CalcTextSize(rightBuf).x;
        const float contentMaxX = ::ImGui::GetWindowContentRegionMax().x;
        const float xPos = contentMaxX - textW - 4.0f;
        if (xPos > ::ImGui::GetCursorPosX()) {
            ::ImGui::SetCursorPosX(xPos);
        } else {
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(rightBuf);
        if (::ImGui::IsItemHovered()) {
            ImGui::SetTooltip("UI font size and active theme — change them under View > Appearance.\n"
                              "Frame-rate metrics live in the Performance window.");
        }
    }

    ImGui::PopStyleVar();
    ::ImGui::End();
}
