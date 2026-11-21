#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstddef>
#include "imgui.h"
#include "ToastHistoryPure.h" // ToastType, ToastRowAction, ToastHistoryEntry, PushBoundedHistory
#include "UndoLedgerPure.h"   // smatchet::undo::UndoLedger — the actions behind an Undo button
#include <cstdint>
#include <functional>

struct ToastNotification {
    std::string Title;
    std::string Message;
    ToastType Type = ToastType::Info;
    std::chrono::steady_clock::time_point Expiry;
    float FadeIn = 0.0f;              // 0..1
    bool Sticky = false;              // never auto-expires; requires an explicit dismiss (Error toasts)
    ToastRowAction RowAction;         // optional; the history entry's action (transient click opens the center)
    bool RunRowActionOnClick = false; // a click on the toast itself runs RowAction
    std::uint64_t UndoId = 0;         // nonzero: the toast has an Undo button for this ledger entry
};

class SmatchetToastManager {
  public:
    static SmatchetToastManager& Instance();

    void Push(const std::string& title, const std::string& message, ToastType type = ToastType::Info,
              int durationMs = 4000);
    // Overload carrying the per-entry Notification Center row action; otherwise identical.
    void Push(const std::string& title, const std::string& message, ToastType type, int durationMs,
              ToastRowAction rowAction);
    // As above, and a click on the toast itself runs `rowAction` too.
    void PushClickable(const std::string& title, const std::string& message, ToastType type, int durationMs,
                       ToastRowAction rowAction);
    // An action that was done at once and can be taken back while this toast is up. The toast shows an
    // Undo button and names Ctrl+Z. Undo (the button, or Ctrl+Z with no text field active and no modal
    // open) runs `undo`; any other way the toast goes away (it expires, is dismissed or pushed out at
    // the live cap, DismissAllLive, CommitPendingUndos at shutdown) runs `commit`. Exactly one of the two
    // runs, once, on the UI thread; either may be empty.
    // Returns the undo id, for CommitUndo.
    std::uint64_t PushUndoable(const std::string& title, const std::string& message, int durationMs,
                               std::function<void()> undo, std::function<void()> commit);
    // Settles one pending undo now as a commit and takes the Undo button off its toast (the toast stays
    // until it expires). For an action that must be final before something else happens: a chat clear
    // before the next message is stored. False when `undoId` already settled.
    bool CommitUndo(std::uint64_t undoId);
    // Undoes the newest undoable toast on screen (what Ctrl+Z does). False when there is none.
    bool UndoNewest();
    // Runs the commit of every pending undo now. Shutdown calls it while the services the commits write
    // through are still running; the toasts stay up without their Undo button.
    void CommitPendingUndos();
    // The end-of-frame half of Ctrl+Z (called from an ImGui RenderPre hook, not by other code): runs the
    // undo only when no text field (a plain input, or an editor widget that sets io.WantTextInput while
    // it is drawn) and no modal has the keyboard.
    void ResolveUndoShortcut();
    // The context the end-of-frame hook was installed in is being destroyed (an ImGui Shutdown hook calls
    // this): forget it, so a new context, which may reuse the address, gets the hook again.
    void ForgetUndoHookContext();
    // Undoable toasts on screen (tests).
    std::size_t LiveUndoableCount() const;
    void Render();

    // Bounded, session-only history of every toast raised (newest last). Drawn newest-first
    // by the Notification Center (S3); each entry keeps its RowAction.
    const std::vector<ToastHistoryEntry>& History() const { return m_history; }
    void ClearHistory() { m_history.clear(); }

    // Live (on-screen) toast count. Bounded by MaxLiveToasts() — a burst of failures (one
    // sticky error toast each) must not stack past the top of the viewport; the oldest
    // live toast is dropped instead (its history entry + the unread-error badge persist).
    std::size_t LiveCount() const { return m_toasts.size(); }
    // True while a toast with this title and message is on screen. It stops being true when the toast
    // expires, is dismissed, or is pushed out at the live cap; the history does not count.
    bool HasLive(const std::string& title, const std::string& message) const;
    // Dismiss every live toast at once (history untouched). Pending undos commit.
    void DismissAllLive();
    // Live-stack cap: limits::Get().ToastMaxLive (default 6). A function, not a constant, so a
    // changed config takes effect on the next Push.
    static std::size_t MaxLiveToasts();

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
    // The one push path. `undoId` is set before the toast joins the list: a pending undo committed by
    // the live-cap eviction may itself push a toast, so `m_toasts.back()` is not reliably this one after.
    void PushImpl(const std::string& title, const std::string& message, ToastType type, int durationMs,
                  ToastRowAction rowAction, bool runRowActionOnClick, std::uint64_t undoId);
    // Ctrl+Z: Render notes the press; ImGui's end-of-frame hook decides (ResolveUndoShortcut), once every
    // window of the frame has had its say on whether a text field has the keyboard.
    void HandleUndoShortcut();

    std::vector<ToastNotification> m_toasts;
    smatchet::undo::UndoLedger m_undo;
    bool m_undoKeyPressed = false;             // Ctrl+Z was pressed this frame; resolved at frame end
    const void* m_undoHookedContext = nullptr; // the ImGui context the end-of-frame hook is installed in
    std::vector<ToastHistoryEntry> m_history;
    bool m_openCenterRequested = false;
    int m_unreadErrorCount = 0;
    // History cap: limits::Get().ToastHistoryMax (default 200).
    static std::size_t HistoryCap();
};
