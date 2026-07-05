#include "SmatchetNotificationCenterUi.h"

#include "NotificationCenterPure.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#include "SmatchetLocalizedImGui.h"
// Routes the window title / button / empty-state literals below through the localization
// wrapper; data-bearing calls (row header Selectable, "%s" TextWrapped) pass through
// untranslated because only exact catalog-English matches rewrite.
#define ImGui SmatchetLocalizedImGui

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// SmatchetDrawNotificationCenterWindow — see the header. Lists the toast manager's bounded history
// newest-first. A clicked row's action is captured and run only after the window is closed for the
// frame, so an action that pushes a toast or clears history cannot invalidate the vector reference
// the row loop walks. The window's visibility bool drives the title-bar close; the menu, command,
// and toast-click open it elsewhere.
void SmatchetDrawNotificationCenterWindow(UiDrawSession& d) {
    if (!d.showNotificationCenterWindow) {
        return;
    }

    const bool wantFocus = d.requestNotificationCenterFocus;
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Notifications", &d.showNotificationCenterWindow)) {
        ImGui::End();
        if (wantFocus) {
            d.requestNotificationCenterFocus = false;
        }
        return;
    }
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestNotificationCenterFocus = false;
    }

    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    const std::vector<ToastHistoryEntry>& history = mgr.History();

    bool clearRequested = false;
    if (ImGui::Button("Clear all")) {
        clearRequested = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s",
                        SmatchetLocalization::Format("notifCenter.count", "%d notification%s",
                                                     static_cast<int>(history.size()), history.size() == 1 ? "" : "s"));
    ImGui::Separator();

    // Deferred until after the row loop + End(): an action may mutate the history ring.
    ToastRowAction pendingAction;

    if (history.empty()) {
        ImGui::TextDisabled("No notifications yet.");
    } else {
        ImGui::BeginChild("notif_list", ImVec2(0.0f, 0.0f), false);
        // Newest-first: walk the bounded ring (oldest-first) in reverse.
        for (std::size_t i = history.size(); i-- > 0;) {
            const ToastHistoryEntry& e = history[i];
            const std::int64_t unixSec =
                std::chrono::duration_cast<std::chrono::seconds>(e.CreatedAt.time_since_epoch()).count();
            ImGui::PushID(static_cast<int>(i));
            const std::string header = std::string("[") + smatchet::FormatClockHMS(unixSec) + "] " +
                                       smatchet::ToastTypeShortLabel(e.Type) + "  " + e.Title;
            const bool actionable = static_cast<bool>(e.RowAction);
            if (ImGui::Selectable(header.c_str()) && actionable) {
                pendingAction = e.RowAction;
            }
            if (actionable && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (!e.Message.empty()) {
                ImGui::Indent();
                ImGui::TextWrapped("%s", e.Message.c_str());
                ImGui::Unindent();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::End();

    // Run the side effects only after the window is closed for this frame — the history
    // reference above is no longer read, so a row action that pushes/clears toasts is safe.
    if (clearRequested) {
        mgr.ClearHistory();
    }
    if (pendingAction) {
        pendingAction();
    }
}
