#include "Sync/TicketChangeDiffPure.h"

#include "SalientRosterResolve.h" // ResolveSalientRoster decl + TrackerField/TrackerFieldFamily

#include <unordered_map>
#include <unordered_set>

namespace smatchet {

namespace {

// A canonical salient role and how to recognise the catalog field(s) that carry it. Match is
// tried family → schemaSystem → exact (lowercased) name, in that precedence: the structural
// family and the Jira system id are precise; the exact-name fallback resolves non-Jira
// backends whose field name equals the role without risking the false positives a substring
// match would draw from custom fields (e.g. a "Priority Score" custom field).
struct SalientRoleSpec {
    const char* label;         // toast label, e.g. "Status"
    TrackerFieldFamily family; // structural match; Unknown = skip the family test
    const char* schemaSystem;  // Jira system id (lowercased compare); nullptr = skip
    const char* nameExact;     // exact lowercased field-name fallback; nullptr = skip
};

// Canonical roster order. A field binds to the FIRST role it matches (see the bound-set in
// ResolveSalientRoster), so the priority here also breaks the rare two-role tie.
const SalientRoleSpec kSalientRoles[] = {
    {"Status", TrackerFieldFamily::Status, "status", "status"},
    {"Assignee", TrackerFieldFamily::Unknown, "assignee", "assignee"},
    {"Priority", TrackerFieldFamily::Unknown, "priority", "priority"},
    {"Summary", TrackerFieldFamily::Unknown, "summary", "summary"},
    {"DueDate", TrackerFieldFamily::Unknown, "duedate", "due date"},
    {"Sprint", TrackerFieldFamily::Sprint, nullptr, "sprint"},
    {"Labels", TrackerFieldFamily::Labels, "labels", "labels"},
    {"Components", TrackerFieldFamily::Unknown, "components", "components"},
};

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (std::size_t i = 0; i < out.size(); ++i) {
        char& c = out[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool FieldMatchesRole(const TrackerField& f, const SalientRoleSpec& spec) {
    if (spec.family != TrackerFieldFamily::Unknown && f.Family == spec.family) {
        return true;
    }
    if (spec.schemaSystem != nullptr && ToLowerAscii(f.SchemaSystem) == spec.schemaSystem) {
        return true;
    }
    if (spec.nameExact != nullptr && ToLowerAscii(f.Name) == spec.nameExact) {
        return true;
    }
    return false;
}

} // namespace

std::vector<SalientFieldRole> ResolveSalientRoster(const std::vector<TrackerField>& fields) {
    std::vector<SalientFieldRole> roster;
    std::unordered_set<std::string> bound; // field ids already assigned to a role
    for (const SalientRoleSpec& spec : kSalientRoles) {
        for (const TrackerField& f : fields) {
            if (f.Id.empty() || bound.count(f.Id) != 0) {
                continue;
            }
            if (FieldMatchesRole(f, spec)) {
                SalientFieldRole role;
                role.fieldId = f.Id;
                role.label = spec.label;
                roster.push_back(role);
                bound.insert(f.Id);
            }
        }
    }
    return roster;
}

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
