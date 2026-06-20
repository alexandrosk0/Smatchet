#include "Sync/TicketChangeDiffPure.h"

#include <unordered_map>

namespace smatchet {

bool IsSalientChangeField(const std::string& fieldId, const std::vector<SalientFieldRole>& roster) {
    for (const SalientFieldRole& role : roster) {
        if (role.fieldId == fieldId) {
            return true;
        }
    }
    return false;
}

std::vector<TicketChangeSummary> DiffChangedTickets(const std::vector<CachedTicket>& prev,
                                                    const std::vector<CachedTicket>& next,
                                                    const std::vector<SalientFieldRole>& roster) {
    std::vector<TicketChangeSummary> changes;

    // Index prev by id for O(1) lookup as we walk next in order.
    std::unordered_map<std::string, const CachedTicket*> prevById;
    prevById.reserve(prev.size());
    for (const CachedTicket& t : prev) {
        prevById[t.id] = &t;
    }

    for (const CachedTicket& cur : next) {
        const auto it = prevById.find(cur.id);
        if (it == prevById.end()) {
            continue; // new key — membership-handled by the caller, not a field diff
        }
        const CachedTicket& before = *it->second;
        for (const SalientFieldRole& role : roster) {
            const std::string& from = before.GetFieldValueRef(role.fieldId);
            const std::string& to = cur.GetFieldValueRef(role.fieldId);
            if (from == to) {
                continue;
            }
            TicketChangeSummary summary;
            summary.kind = TicketChangeKind::Modified;
            summary.issueId = cur.id;
            summary.changedFieldLabel = role.label;
            summary.fromValue = from;
            summary.toValue = to;
            changes.push_back(summary);
        }
    }
    return changes;
}

namespace {

// One human-readable line for a single change.
std::string FormatLine(const TicketChangeSummary& c) {
    switch (c.kind) {
    case TicketChangeKind::Added:
        return c.issueId + " added to view";
    case TicketChangeKind::LeftView:
        return c.issueId + " left view";
    case TicketChangeKind::Deleted:
        return c.issueId + " deleted";
    case TicketChangeKind::Modified:
    default:
        break;
    }

    std::string line = c.issueId + " " + c.changedFieldLabel + ": ";
    if (c.fromValue.empty() && !c.toValue.empty()) {
        line += "set to " + c.toValue;
    } else if (!c.fromValue.empty() && c.toValue.empty()) {
        line += "cleared";
    } else {
        line += c.fromValue + " -> " + c.toValue;
    }
    if (!c.author.empty()) {
        line += " (by " + c.author + ")";
    }
    return line;
}

} // namespace

std::string FormatTicketChangeToast(const std::vector<TicketChangeSummary>& changes, int cap) {
    if (changes.empty()) {
        return std::string();
    }
    const std::size_t shown = static_cast<std::size_t>(cap > 0 ? cap : 1) < changes.size()
                                  ? static_cast<std::size_t>(cap > 0 ? cap : 1)
                                  : changes.size();

    std::string out;
    for (std::size_t i = 0; i < shown; ++i) {
        if (i > 0) {
            out += "\n";
        }
        out += FormatLine(changes[i]);
    }
    const std::size_t remaining = changes.size() - shown;
    if (remaining > 0) {
        out += "\n+" + std::to_string(remaining) + " more";
    }
    return out;
}

} // namespace smatchet
