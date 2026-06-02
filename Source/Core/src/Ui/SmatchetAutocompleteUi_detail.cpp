// SmatchetAutocompleteUi pure list-navigation / completion-resolution helpers —
// see SmatchetAutocompleteUi_detail.h for the contract.

#include "SmatchetAutocompleteUi_detail.h"

#include <algorithm>

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

} // namespace SmatchetAutocompleteDetail
