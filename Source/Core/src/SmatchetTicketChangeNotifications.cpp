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

} // namespace

void NotifyTicketChanges(const std::vector<smatchet::TicketChangeSummary>& changes) {
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
    SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.tickets", "Tickets"), body, ToastType::Info,
                                          5000);
}
