// Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — pure-logic helpers
// extracted from PlaneIssueSearch.cpp. No cpr, no Logger, no PlaneClient
// instance state. See PlaneIssueMappingPure.h for the contract.

#include "PlaneIssueMappingPure.h"

#include "PlaneJsonFieldPure.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace smatchet {
namespace plane {

namespace {

using smatchet::plane_pure::JsonFieldToString;

void TrimAsciiWs(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.erase(0, 1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.pop_back();
    }
}

std::string PlaneWorkItemTitleForDisplay(const nlohmann::json& issue) {
    std::string title = JsonFieldToString(issue, "name");
    TrimAsciiWs(title);
    return title;
}

// Resolve a value that Plane returns under one of three nesting shapes:
// `<base>_detail.<innerKey>` (object), `<base>.<innerKey>` (object), or the
// bare scalar `<base>`. Used for status, sprint (cycle), and issuetype.
std::string NestedDetailValue(const nlohmann::json& issue, const char* detailKey, const char* baseKey,
                              const char* innerKey) {
    if (issue.contains(detailKey) && issue[detailKey].is_object()) {
        return JsonFieldToString(issue[detailKey], innerKey);
    }
    if (issue.contains(baseKey) && issue[baseKey].is_object()) {
        return JsonFieldToString(issue[baseKey], innerKey);
    }
    return JsonFieldToString(issue, baseKey);
}

// Assignee — id from `assignees[0]`, display name from `assignee_details[0].display_name`
// or fall back to the `users` lookup vector. Mirrors the production fetcher exactly.
std::string ResolvePlaneAssigneeName(const nlohmann::json& issue, const std::vector<UserDisplayLookup>& users) {
    std::string assigneeId;
    if (issue.contains("assignees") && issue["assignees"].is_array() && !issue["assignees"].empty()) {
        const auto& first = issue["assignees"][0];
        if (first.is_object()) {
            assigneeId = JsonFieldToString(first, "id");
        } else if (first.is_string()) {
            assigneeId = first.get<std::string>();
        }
    }
    std::string assigneeName = assigneeId;
    if (issue.contains("assignee_details") && issue["assignee_details"].is_array() &&
        !issue["assignee_details"].empty()) {
        const auto& first = issue["assignee_details"][0];
        if (first.is_object() && first.contains("display_name")) {
            assigneeName = JsonFieldToString(first, "display_name");
        }
    }
    if (assigneeName == assigneeId && !assigneeId.empty()) {
        auto uIt = std::find_if(users.begin(), users.end(),
                                [&](const UserDisplayLookup& u) { return u.AccountId == assigneeId; });
        if (uIt != users.end()) {
            assigneeName = uIt->DisplayName;
        }
    }
    return assigneeName;
}

// Labels — comma-separated display names. Prefer label_details, fall back to labels.
std::string ResolvePlaneLabelsString(const nlohmann::json& issue) {
    std::string labelStr;
    if (issue.contains("label_details") && issue["label_details"].is_array()) {
        for (const auto& lbl : issue["label_details"]) {
            if (!labelStr.empty())
                labelStr += ", ";
            std::string ln = JsonFieldToString(lbl, "name");
            if (ln.empty())
                ln = JsonFieldToString(lbl, "id");
            labelStr += ln;
        }
    } else if (issue.contains("labels") && issue["labels"].is_array()) {
        for (const auto& lbl : issue["labels"]) {
            if (!labelStr.empty())
                labelStr += ", ";
            if (lbl.is_object()) {
                std::string ln = JsonFieldToString(lbl, "name");
                if (ln.empty())
                    ln = JsonFieldToString(lbl, "id");
                labelStr += ln;
            } else if (lbl.is_string()) {
                labelStr += lbl.get<std::string>();
            }
        }
    }
    return labelStr;
}

// Identity + key fields: uuid, visual key, summary.
void MapPlaneIdentityFields(const nlohmann::json& issue, const std::string& projectIdentifier, CachedTicket& ticket) {
    const std::string uuid = JsonFieldToString(issue, "id");
    const std::string seqId = JsonFieldToString(issue, "sequence_id");

    std::string visualKey;
    if (!projectIdentifier.empty() && !seqId.empty()) {
        visualKey = projectIdentifier + "-" + seqId;
    } else if (!seqId.empty()) {
        visualKey = "#" + seqId;
    } else {
        visualKey = uuid;
    }

    ticket.id = visualKey;
    ticket.fieldValues["uuid"] = uuid;
    ticket.fieldValues["key"] = visualKey;
    ticket.fieldValues["summary"] = PlaneWorkItemTitleForDisplay(issue);
}

// Description (stripped + rich HTML round-trip) and created/updated timestamps.
void MapPlaneDescriptionAndTimestamps(const nlohmann::json& issue, CachedTicket& ticket) {
    ticket.fieldValues["created"] = JsonFieldToString(issue, "created_at");
    ticket.fieldValues["updated"] = JsonFieldToString(issue, "updated_at");
    ticket.fieldValues["description"] = JsonFieldToString(issue, "description_stripped");

    // Preserve original HTML so the long-text modal editor can round-trip via
    // Markdown without destroying formatting on save.
    const std::string descHtml = JsonFieldToString(issue, "description_html");
    if (!descHtml.empty()) {
        ticket.fieldRichValues["description"] = descHtml;
    }
}

} // namespace

CachedTicket MapPlaneWorkItemJsonToCachedTicket(const nlohmann::json& issue, const std::string& projectIdentifier,
                                                const std::vector<UserDisplayLookup>& users) {
    CachedTicket ticket;

    MapPlaneIdentityFields(issue, projectIdentifier, ticket);
    ticket.fieldValues["status"] = NestedDetailValue(issue, "state_detail", "state", "id");
    ticket.fieldValues["assignee"] = ResolvePlaneAssigneeName(issue, users);
    ticket.fieldValues["priority"] = JsonFieldToString(issue, "priority");
    ticket.fieldValues["sprint"] = NestedDetailValue(issue, "cycle_details", "cycle", "id");
    ticket.fieldValues["labels"] = ResolvePlaneLabelsString(issue);
    MapPlaneDescriptionAndTimestamps(issue, ticket);
    ticket.fieldValues["issuetype"] = NestedDetailValue(issue, "type_detail", "type", "name");

    return ticket;
}

std::vector<CachedTicket>
MapPlaneWorkItemsArrayToCachedTickets(const nlohmann::json& results, const std::string& projectIdentifier,
                                      const std::vector<UserDisplayLookup>& users,
                                      std::unordered_map<std::string, std::string>* outKeyToId) {
    std::vector<CachedTicket> out;
    if (!results.is_array()) {
        return out;
    }
    out.reserve(results.size());
    for (const auto& issue : results) {
        try {
            CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, projectIdentifier, users);
            if (t.id.empty()) {
                continue;
            }
            if (outKeyToId) {
                const auto uuidIt = t.fieldValues.find("uuid");
                if (uuidIt != t.fieldValues.end()) {
                    (*outKeyToId)[t.id] = uuidIt->second;
                }
            }
            out.push_back(std::move(t));
        } catch (const std::exception&) {
            // Silently skip — production fetcher logs at WARN; pure helper stays logger-free.
            continue;
        } catch (...) {
            continue;
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> BuildPlaneAuthHeaders(const std::string& planeApiKey) {
    std::unordered_map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["x-api-key"] = planeApiKey;
    return headers;
}

std::string NextPaginationCursor(const nlohmann::json& listPayload) {
    if (!listPayload.is_object()) {
        return std::string();
    }
    bool more = false;
    if (listPayload.contains("next_page_results") && listPayload["next_page_results"].is_boolean()) {
        more = listPayload["next_page_results"].get<bool>();
    }
    if (!more) {
        return std::string();
    }
    if (listPayload.contains("next_cursor") && listPayload["next_cursor"].is_string()) {
        return listPayload["next_cursor"].get<std::string>();
    }
    return std::string();
}

std::string ExtractKeyFromPlaneQuery(const std::string& planeQueryJson) {
    if (planeQueryJson.empty()) {
        return std::string();
    }
    try {
        // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=the Plane structured-query blob is app-serialised program-internal bytes, not tracker-response ingress; owner=security-audit; revisit=2026-12-31)
        const nlohmann::json j = nlohmann::json::parse(planeQueryJson);
        if (j.is_object()) {
            auto it = j.find("key");
            if (it != j.end() && it->is_string()) {
                return it->get<std::string>();
            }
        }
    } catch (const std::exception&) {
        return std::string();
    }
    return std::string();
}

namespace {

// A canonical Plane work-item UUID (8-4-4-4-12 hex, lowercase or upper). We must NEVER treat a
// bare UUID as a visual key: its final dash-segment is 12 hex chars that can, by chance, be all
// decimal digits — which would otherwise slip past the "digit suffix" test below and send a
// bogus `sequence_id__in` value. Detecting UUID shape lets us abandon the server filter cleanly.
bool LooksLikePlaneUuid(const std::string& s) {
    if (s.size() != 36) {
        return false;
    }
    for (std::string::size_type i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string BuildPlaneSequenceIdInFilter(const std::vector<std::string>& issueKeys) {
    if (issueKeys.empty()) {
        return std::string();
    }
    std::string out;
    std::unordered_set<std::string> seen;
    seen.reserve(issueKeys.size());
    for (const std::string& key : issueKeys) {
        // The sequence_id is the numeric suffix after the last '-' (e.g. `SMT-123` → `123`).
        // A bare UUID, a key with no '-', an empty suffix, an empty identifier prefix, or a
        // non-digit suffix all mean "cannot map to a sequence_id" — abandon the whole filter
        // so the caller does the unfiltered sweep rather than server-side-excluding an issue
        // we still need. UUIDs are rejected explicitly (their last segment can be all digits).
        if (LooksLikePlaneUuid(key)) {
            return std::string();
        }
        const std::string::size_type dash = key.rfind('-');
        if (dash == std::string::npos || dash == 0 || dash + 1 >= key.size()) {
            return std::string();
        }
        const std::string suffix = key.substr(dash + 1);
        bool allDigits = true;
        for (char c : suffix) {
            if (c < '0' || c > '9') {
                allDigits = false;
                break;
            }
        }
        if (!allDigits) {
            return std::string();
        }
        if (seen.insert(suffix).second) {
            if (!out.empty()) {
                out += ',';
            }
            out += suffix;
        }
    }
    return out;
}

} // namespace plane
} // namespace smatchet
