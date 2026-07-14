// SmatchetAutocompleteUi pure list-navigation / completion-resolution helpers —
// see SmatchetAutocompleteUi_detail.h for the contract.

#include "SmatchetAutocompleteUi_detail.h"

#include <algorithm>
#include <cctype>
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

bool ValueNeedsQuotesForId(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    return std::any_of(s.begin(), s.end(), [](unsigned char ch) {
        const bool ok = std::isalnum(ch) != 0 || ch == '_' || ch == '.' || ch == '-';
        return !ok || ch == '"' || ch == '\\';
    });
}

std::string QuotedJqlStyle(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
    return out;
}

std::string InsertTokenForUserAccountId(const std::string& accountId) {
    if (ValueNeedsQuotesForId(accountId)) {
        return QuotedJqlStyle(accountId);
    }
    return accountId;
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
