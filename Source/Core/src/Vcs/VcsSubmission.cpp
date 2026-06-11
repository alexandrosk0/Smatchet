#include "Vcs/VcsSubmission.h"

namespace Vcs {

namespace {

bool IsP4DateShape(const std::string& d) {
    // YYYY/MM/DD — exactly 10 chars, slashes at 4 and 7, digits elsewhere.
    if (d.size() != 10 || d[4] != '/' || d[7] != '/') {
        return false;
    }
    for (size_t i = 0; i < d.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (d[i] < '0' || d[i] > '9') {
            return false;
        }
    }
    return true;
}

} // namespace

std::string P4UserFromEmail(const std::string& email) {
    const size_t at = email.find('@');
    if (at == std::string::npos) {
        return email;
    }
    return email.substr(0, at);
}

VcsSubmission FromP4Change(const P4ChangeSummary& change) {
    VcsSubmission s;
    s.Source = VcsSource::P4;
    s.Id = change.Changelist;
    s.Timestamp = change.Date;
    if (IsP4DateShape(s.Timestamp)) {
        s.Timestamp[4] = '-';
        s.Timestamp[7] = '-';
    }
    s.Author = change.User;
    s.FirstLine = change.Description;
    return s;
}

bool KeepOnOrAfterCutoff(const std::string& timestamp, const std::string& cutoffIsoDate) {
    if (timestamp.size() < 10) {
        return true;
    }
    return timestamp.compare(0, 10, cutoffIsoDate) >= 0;
}

std::vector<VcsSubmission> MergeFeedsNewestFirst(const std::vector<VcsSubmission>& a,
                                                 const std::vector<VcsSubmission>& b) {
    std::vector<VcsSubmission> merged;
    merged.reserve(a.size() + b.size());
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        // Newest first; on ties take from `a` (stable left preference).
        if (a[i].Timestamp >= b[j].Timestamp) {
            merged.push_back(a[i]);
            ++i;
        } else {
            merged.push_back(b[j]);
            ++j;
        }
    }
    for (; i < a.size(); ++i) {
        merged.push_back(a[i]);
    }
    for (; j < b.size(); ++j) {
        merged.push_back(b[j]);
    }
    return merged;
}

} // namespace Vcs
