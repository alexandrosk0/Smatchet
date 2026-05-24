// Slice 2 of docs/design/autonomous-debugging-no-creds.md — pure-logic helpers
// extracted from PlaneIssueSearch.cpp. No cpr, no Logger, no PlaneClient
// instance state. See PlaneIssueMappingPure.h for the contract.

#include "PlaneIssueMappingPure.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace smatchet {
namespace plane {

namespace {

// Local copy of the production JsonFieldToString helper (defined in PlaneClient.cpp
// inside the smatchet::plane_detail namespace). Replicated here to keep this TU
// free of the production internal header (which pulls cpr via PlaneClient_Internal.h).
std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
        return std::string();
    const auto& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<std::int64_t>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? std::string("true") : std::string("false");
    return v.dump();
}

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

} // namespace

CachedTicket MapPlaneWorkItemJsonToCachedTicket(const nlohmann::json& issue, const std::string& projectIdentifier,
                                                const std::vector<UserDisplayLookup>& users) {
    CachedTicket ticket;

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

    // Status — three nesting shapes.
    if (issue.contains("state_detail") && issue["state_detail"].is_object()) {
        ticket.fieldValues["status"] = JsonFieldToString(issue["state_detail"], "id");
    } else if (issue.contains("state") && issue["state"].is_object()) {
        ticket.fieldValues["status"] = JsonFieldToString(issue["state"], "id");
    } else {
        ticket.fieldValues["status"] = JsonFieldToString(issue, "state");
    }

    // Assignee — id from `assignees[0]`, display name from `assignee_details[0].display_name`
    // or fall back to the `users` lookup vector. Mirrors the production fetcher exactly.
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
    ticket.fieldValues["assignee"] = assigneeName;

    ticket.fieldValues["priority"] = JsonFieldToString(issue, "priority");

    // Sprint (Plane "cycle").
    if (issue.contains("cycle_details") && issue["cycle_details"].is_object()) {
        ticket.fieldValues["sprint"] = JsonFieldToString(issue["cycle_details"], "id");
    } else if (issue.contains("cycle") && issue["cycle"].is_object()) {
        ticket.fieldValues["sprint"] = JsonFieldToString(issue["cycle"], "id");
    } else {
        ticket.fieldValues["sprint"] = JsonFieldToString(issue, "cycle");
    }

    // Labels — comma-separated display names. Prefer label_details, fall back to labels.
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
    ticket.fieldValues["labels"] = labelStr;

    ticket.fieldValues["created"] = JsonFieldToString(issue, "created_at");
    ticket.fieldValues["updated"] = JsonFieldToString(issue, "updated_at");
    ticket.fieldValues["description"] = JsonFieldToString(issue, "description_stripped");

    // Preserve original HTML so the long-text modal editor can round-trip via
    // Markdown without destroying formatting on save.
    const std::string descHtml = JsonFieldToString(issue, "description_html");
    if (!descHtml.empty()) {
        ticket.fieldRichValues["description"] = descHtml;
    }

    // Issue Type.
    if (issue.contains("type_detail") && issue["type_detail"].is_object()) {
        ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type_detail"], "name");
    } else if (issue.contains("type") && issue["type"].is_object()) {
        ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type"], "name");
    } else {
        ticket.fieldValues["issuetype"] = JsonFieldToString(issue, "type");
    }

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

} // namespace plane
} // namespace smatchet
