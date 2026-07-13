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
    // Viewing the center clears the status-bar unread-error badge — this history IS the
    // error trail the badge points at.
    mgr.MarkHistorySeen();
    // ... and dismisses the live toast stack (P2-M8): the errors the sticky toasts were
    // holding on screen are now safely in view below, so the user shouldn't have to click
    // each toast individually after reading them here. Open-edge only — toasts raised
    // WHILE the center is open still appear.
    if (::ImGui::IsWindowAppearing()) {
        mgr.DismissAllLive();
        // An armed-but-unconfirmed "Clear all" must not survive close/reopen — a stale
        // primed Confirm button is exactly the misclick this two-step flow guards against.
        ::ImGui::GetStateStorage()->SetBool(::ImGui::GetID("##clear_all_armed"), false);
    }
    const std::vector<ToastHistoryEntry>& history = mgr.History();

    // Two-step Clear all (P2-L2): this history is the only record of faded errors — a
    // single misclick must not destroy it. Arm state lives in the window's ImGui storage.
    bool clearRequested = false;
    {
        ImGuiStorage* storage = ::ImGui::GetStateStorage();
        const ImGuiID armId = ::ImGui::GetID("##clear_all_armed");
        const bool armed = storage->GetBool(armId, false);
        if (!armed) {
            if (ImGui::Button("Clear all")) {
                storage->SetBool(armId, true);
            }
        } else {
            if (ImGui::Button("Confirm clear all")) {
                clearRequested = true;
                storage->SetBool(armId, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) {
                storage->SetBool(armId, false);
            }
        }
    }
    ImGui::SameLine();
    // Raw (::) sink: Format already returns finished text. Singular/plural are separate catalog
    // keys so translations are not forced into English append-"s" grammar.
    ::ImGui::TextDisabled("%s", history.size() == 1
                                    ? SmatchetLocalization::Format("notifCenter.count_one", "%d notification",
                                                                   static_cast<int>(history.size()))
                                    : SmatchetLocalization::Format("notifCenter.count_many", "%d notifications",
                                                                   static_cast<int>(history.size())));
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
            const bool actionable = static_cast<bool>(e.RowAction);
            // Actionable rows get a leading chevron so they read as clickable while
            // scanning, not only via the hover cursor (P2-L2).
            const std::string header = std::string(actionable ? "\xE2\x80\xBA " : "  ") + "[" +
                                       smatchet::FormatClockHMS(unixSec) + "] " +
                                       smatchet::ToastTypeShortLabel(e.Type) + "  " + e.Title;
            // SelectableRaw: the row header is data (timestamp + type + toast title) — skip the
            // per-row TranslateSource pass the aliased Selectable would pay (200-row cap, no clipper).
            if (ImGui::SelectableRaw(header.c_str()) && actionable) {
                pendingAction = e.RowAction;
            }
            if (actionable && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (!e.Message.empty()) {
                ImGui::Indent();
                ::ImGui::TextWrapped("%s", e.Message.c_str());
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
