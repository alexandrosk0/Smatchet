#include "SmatchetTicketChangeNotifications.h"

#include <algorithm>
#include <string>

#include "SmatchetLocalization.h"
#include "Sync/TicketChangeDiffPure.h"
#include "Ui/SmatchetToast.h"

namespace {

// Stable signature of a change batch: one "<id>|<kind>|<label>|<from>|<to>" token per change,
// sorted so order-of-detection does not defeat the de-dup. Two consecutive polls that surface
// the identical set of deltas produce the identical signature and the second is suppressed.
std::string ChangeBatchSignature(const std::vector<smatchet::TicketChangeSummary>& changes) {
    std::vector<std::string> tokens;
    tokens.reserve(changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const smatchet::TicketChangeSummary& c = changes[i];
        std::string token = c.issueId;
        token += '|';
        token += std::to_string(static_cast<int>(c.kind));
        token += '|';
        token += c.changedFieldLabel;
        token += '|';
        token += c.fromValue;
        token += '|';
        token += c.toValue;
        tokens.push_back(token);
    }
    std::sort(tokens.begin(), tokens.end());
    std::string sig;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        sig += tokens[i];
        sig += '\n';
    }
    return sig;
}

// UI-thread-only focus handler, registered by the UI layer at init. File-static so the row-action
// lambda below (created here) can reference it even though this TU has no g_ui / AppController.
TicketChangeFocusHandler& FocusHandlerRef() {
    static TicketChangeFocusHandler s_handler;
    return s_handler;
}

// The issue the Notification Center row should focus for a change batch: the first change that is
// not a Deletion (a deleted ticket has no grid row to focus). Empty if every change is a Deletion.
std::string PrimaryFocusableId(const std::vector<smatchet::TicketChangeSummary>& changes) {
    for (std::size_t i = 0; i < changes.size(); ++i) {
        if (changes[i].kind != smatchet::TicketChangeKind::Deleted && !changes[i].issueId.empty()) {
            return changes[i].issueId;
        }
    }
    return std::string();
}

} // namespace

void SetTicketChangeFocusHandler(TicketChangeFocusHandler handler) { FocusHandlerRef() = std::move(handler); }

void NotifyTicketChanges(const std::vector<smatchet::TicketChangeSummary>& changes, const std::string& paneId) {
    if (changes.empty()) {
        return;
    }
    // Function-static last-signature guard (UI thread only — see header). Suppress an identical
    // consecutive batch from an overlapping/retried poll.
    static std::string s_lastSignature;
    const std::string signature = ChangeBatchSignature(changes);
    if (signature == s_lastSignature) {
        return;
    }
    s_lastSignature = signature;

    const std::string body = smatchet::FormatTicketChangeToast(changes, 1);
    if (body.empty()) {
        return;
    }
    // Bind a row action that focuses the primary changed ticket in its pane when the Notification
    // Center row is clicked (item 11). Only when there is a focusable (non-deleted) target and the
    // pane is known; otherwise fall through to the plain 4-arg Push and the row stays informational.
    const std::string focusId = PrimaryFocusableId(changes);
    const std::string title = SmatchetLocalization::T("toast.tickets", "Tickets");
    if (!focusId.empty() && !paneId.empty()) {
        SmatchetToastManager::Instance().Push(title, body, ToastType::Info, 5000, [paneId, focusId]() {
            const TicketChangeFocusHandler& h = FocusHandlerRef();
            if (h) {
                h(paneId, focusId);
            }
        });
        return;
    }
    SmatchetToastManager::Instance().Push(title, body, ToastType::Info, 5000);
}
