#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstddef>
#include "imgui.h"
#include "ToastHistoryPure.h" // ToastType, ToastRowAction, ToastHistoryEntry, PushBoundedHistory

struct ToastNotification {
    std::string Title;
    std::string Message;
    ToastType Type = ToastType::Info;
    std::chrono::steady_clock::time_point Expiry;
    float FadeIn = 0.0f;      // 0..1
    bool Sticky = false;      // never auto-expires; requires an explicit dismiss (Error toasts)
    ToastRowAction RowAction; // optional; the history entry's action (transient click opens the center)
};

class SmatchetToastManager {
  public:
    static SmatchetToastManager& Instance();

    void Push(const std::string& title, const std::string& message, ToastType type = ToastType::Info,
              int durationMs = 4000);
    // Overload carrying the per-entry Notification Center row action; otherwise identical.
    void Push(const std::string& title, const std::string& message, ToastType type, int durationMs,
              ToastRowAction rowAction);
    void Render();

    // Bounded, session-only history of every toast raised (newest last). Drawn newest-first
    // by the Notification Center (S3); each entry keeps its RowAction.
    const std::vector<ToastHistoryEntry>& History() const { return m_history; }
    void ClearHistory() { m_history.clear(); }

    // Live (on-screen) toast count. Bounded by kMaxLiveToasts — a burst of failures (one
    // sticky error toast each) must not stack past the top of the viewport; the oldest
    // live toast is dropped instead (its history entry + the unread-error badge persist).
    std::size_t LiveCount() const { return m_toasts.size(); }
    // Dismiss every live toast at once (history untouched).
    void DismissAllLive() { m_toasts.clear(); }
    static const std::size_t kMaxLiveToasts = 6;

    // Open-center request: set when a transient toast is clicked; the UI polls + consumes it
    // once per open. Kept on the manager (not a global) so every Push surface shares it.
    void RequestOpenCenter() { m_openCenterRequested = true; }
    bool ConsumeOpenCenterRequest();

    // Unread-error trail: incremented on every Error push, cleared when the Notification
    // Center renders (MarkHistorySeen). Drives the persistent status-bar badge so a
    // dismissed/faded error toast still leaves a visible signal.
    int UnreadErrorCount() const { return m_unreadErrorCount; }
    void MarkHistorySeen() { m_unreadErrorCount = 0; }

  private:
    std::vector<ToastNotification> m_toasts;
    std::vector<ToastHistoryEntry> m_history;
    bool m_openCenterRequested = false;
    int m_unreadErrorCount = 0;
    static const std::size_t kHistoryCap = 200;
};
