// SmatchetAutocompleteUi pure list-navigation / completion-resolution helpers —
// see SmatchetAutocompleteUi_detail.h for the contract.

#include "SmatchetAutocompleteUi_detail.h"

#include <algorithm>
#include <climits>
#include <string>

namespace SmatchetAutocompleteDetail {

AcpHistoryNav ResolveAcpHistoryNav(int n, int selected, bool isDown, bool isUp) {
    AcpHistoryNav out;
    if (n > 0) {
        out.selected = selected;
        if (isDown) {
            if (out.selected < 0) {
                out.selected = 0;
            } else {
                out.selected = (std::min)(n - 1, out.selected + 1);
            }
            out.scrollToSelected = true;
        } else if (isUp) {
            if (out.selected >= 0) {
                out.selected = (std::max)(0, out.selected - 1);
                out.scrollToSelected = true;
            }
        } else if (out.selected >= 0) {
            out.selected = (std::min)(out.selected, n - 1);
        }
    } else {
        out.selected = -1;
    }
    return out;
}

AcpCommitDecision ResolveAcpCommit(int n, int selected, bool enterDown, bool tabDown) {
    AcpCommitDecision out;
    out.selected = selected;
    if (n > 0) {
        if (out.selected >= 0) {
            out.selected = (std::min)(out.selected, n - 1);
        }
        if ((enterDown || tabDown) && out.selected >= 0) {
            out.queueApply = true;
        } else if (enterDown) {
            out.wantsApplyFromEnter = true;
        }
    } else {
        out.selected = -1;
        if (enterDown) {
            out.wantsApplyFromEnter = true;
        }
    }
    return out;
}

AcpPick ResolveAcpPick(int buildReplaceStart, int buildReplaceEnd, bool commitOnPick) {
    AcpPick out;
    out.replaceStart = commitOnPick ? 0 : buildReplaceStart;
    out.replaceEnd = commitOnPick ? INT_MAX : buildReplaceEnd;
    out.commitAfterReplace = commitOnPick;
    return out;
}

double ResolveUserSearchFireAt(double now, bool trackerOffline) {
    const double kDebounceSeconds = 0.22;
    return trackerOffline ? now : now + kDebounceSeconds;
}

AcpUserSearchStep ResolveUserSearchStep(double now, double fireAt, bool superseded, bool trackerOffline,
                                        bool inFlight) {
    if (fireAt <= 0.0 || now < fireAt) {
        return AcpUserSearchStep::Hold;
    }
    if (superseded) {
        return AcpUserSearchStep::Drop;
    }
    return (trackerOffline || inFlight) ? AcpUserSearchStep::Hold : AcpUserSearchStep::Send;
}

int StripCaretAnchorSentinel(std::string& text) {
    const std::string::size_type pos = text.find('\x7F');
    if (pos == std::string::npos) {
        return -1;
    }
    text.erase(pos, 1);
    return static_cast<int>(pos);
}

} // namespace SmatchetAutocompleteDetail
