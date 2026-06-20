#include "LinearIssueMappingPure.h"

#include <string>

namespace smatchet {
namespace linear {

namespace {

// Null/missing-safe string read: returns "" unless obj[key] is a string.
std::string JStr(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return std::string();
    }
    nlohmann::json::const_iterator it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

// Pointer to obj[key] when it is an object, else nullptr.
const nlohmann::json* JObj(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    nlohmann::json::const_iterator it = obj.find(key);
    if (it == obj.end() || !it->is_object()) {
        return nullptr;
    }
    return &(*it);
}

// A Linear actor's display string: prefer `displayName`, fall back to `name`.
std::string PersonName(const nlohmann::json& node, const char* relationKey) {
    const nlohmann::json* p = JObj(node, relationKey);
    if (p == nullptr) {
        return std::string();
    }
    const std::string display = JStr(*p, "displayName");
    if (!display.empty()) {
        return display;
    }
    return JStr(*p, "name");
}

// Comma-join `labels.nodes[].name` (blank names skipped).
std::string JoinLabelNames(const nlohmann::json& node) {
    const nlohmann::json* labels = JObj(node, "labels");
    if (labels == nullptr) {
        return std::string();
    }
    nlohmann::json::const_iterator nodesIt = labels->find("nodes");
    if (nodesIt == labels->end() || !nodesIt->is_array()) {
        return std::string();
    }
    std::string out;
    for (nlohmann::json::const_iterator l = nodesIt->begin(); l != nodesIt->end(); ++l) {
        const std::string name = JStr(*l, "name");
        if (name.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

// Priority display: prefer `priorityLabel`, else map the 0-4 integer scale.
std::string PriorityLabel(const nlohmann::json& node) {
    const std::string label = JStr(node, "priorityLabel");
    if (!label.empty()) {
        return label;
    }
    nlohmann::json::const_iterator it = node.find("priority");
    if (it != node.end() && it->is_number_integer()) {
        switch (it->get<int>()) {
            case 0:
                return "No priority";
            case 1:
                return "Urgent";
            case 2:
                return "High";
            case 3:
                return "Medium";
            case 4:
                return "Low";
            default:
                break;
        }
    }
    return std::string();
}

// Cycle display: prefer `name`, else "Cycle <number>".
std::string CycleLabel(const nlohmann::json& node) {
    const nlohmann::json* cycle = JObj(node, "cycle");
    if (cycle == nullptr) {
        return std::string();
    }
    const std::string name = JStr(*cycle, "name");
    if (!name.empty()) {
        return name;
    }
    nlohmann::json::const_iterator num = cycle->find("number");
    if (num != cycle->end() && num->is_number_integer()) {
        return std::string("Cycle ") + std::to_string(num->get<long long>());
    }
    return std::string();
}

} // namespace

CachedTicket MapLinearIssueNodeToCachedTicket(const nlohmann::json& node) {
    CachedTicket ticket;
    if (!node.is_object()) {
        return ticket;
    }

    ticket.id = JStr(node, "identifier");

    ticket.fieldValues["summary"] = JStr(node, "title");
    ticket.fieldValues["description"] = JStr(node, "description");

    const nlohmann::json* state = JObj(node, "state");
    ticket.fieldValues["status"] = (state != nullptr) ? JStr(*state, "name") : std::string();

    ticket.fieldValues["assignee"] = PersonName(node, "assignee");
    ticket.fieldValues["author"] = PersonName(node, "creator");
    ticket.fieldValues["labels"] = JoinLabelNames(node);

    ticket.fieldValues["created"] = JStr(node, "createdAt");
    ticket.fieldValues["updated"] = JStr(node, "updatedAt");

    ticket.fieldValues["priority"] = PriorityLabel(node);

    const nlohmann::json* project = JObj(node, "project");
    ticket.fieldValues["project"] = (project != nullptr) ? JStr(*project, "name") : std::string();

    const nlohmann::json* team = JObj(node, "team");
    ticket.fieldValues["team"] = (team != nullptr) ? JStr(*team, "key") : std::string();

    ticket.fieldValues["cycle"] = CycleLabel(node);

    // Persist the issue URL for browse links (empty when the API didn't return one,
    // so BuildBrowseUrl returns empty rather than guessing a broken workspace URL).
    ticket.fieldValues["url"] = JStr(node, "url");

    return ticket;
}

std::vector<CachedTicket> MapLinearIssueNodesToTickets(const nlohmann::json& nodes) {
    std::vector<CachedTicket> tickets;
    if (!nodes.is_array()) {
        return tickets;
    }
    tickets.reserve(nodes.size());
    for (nlohmann::json::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        tickets.push_back(MapLinearIssueNodeToCachedTicket(*it));
    }
    return tickets;
}

} // namespace linear
} // namespace smatchet
