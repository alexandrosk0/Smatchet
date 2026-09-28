#include "LookupPayloadsPure.h"

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smatchet {
namespace lookup {

namespace {

// A roster row is an array of flat objects: depth 2 exactly, which also rules out a depth bomb in a
// tampered row. ParseBounded counts each user as 6 nodes (the object and its 5 values; keys are free).
constexpr int kUsersMaxDepth = 2;
constexpr std::size_t kUsersMaxNodes = 6u * kMaxStoredUsers + 1u;

std::string StringMember(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

std::string SerializeUsers(const std::vector<TrackerUser>& users) {
    nlohmann::json arr = nlohmann::json::array();
    const std::size_t count = std::min(users.size(), kMaxStoredUsers);
    for (std::size_t i = 0; i < count; ++i) {
        const TrackerUser& user = users[i];
        arr.push_back(nlohmann::json::object({{"accountId", user.AccountId},
                                              {"displayName", user.DisplayName},
                                              {"email", user.EmailAddress},
                                              {"accountType", user.AccountType},
                                              {"active", user.Active}}));
    }
    // Names and emails are tracker-supplied text; replace invalid UTF-8 instead of throwing.
    std::string out = arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (out.size() > kMaxUsersPayloadBytes) {
        return std::string();
    }
    return out;
}

bool ParseUsers(const std::string& json, std::vector<TrackerUser>& out) {
    out.clear();
    std::string err;
    const nlohmann::json parsed =
        smatchet::json_safe::ParseBounded(json, err, kMaxUsersPayloadBytes, kUsersMaxDepth, kUsersMaxNodes);
    if (!err.empty() || !parsed.is_array()) {
        return false;
    }
    out.reserve(parsed.size());
    for (const nlohmann::json& entry : parsed) {
        if (!entry.is_object()) {
            continue;
        }
        TrackerUser user;
        user.AccountId = StringMember(entry, "accountId");
        if (user.AccountId.empty()) {
            continue;
        }
        user.DisplayName = StringMember(entry, "displayName");
        user.EmailAddress = StringMember(entry, "email");
        user.AccountType = StringMember(entry, "accountType");
        const auto active = entry.find("active");
        user.Active = active == entry.end() || !active->is_boolean() || active->get<bool>();
        out.push_back(std::move(user));
    }
    return true;
}

std::string SerializeEditPermissions(const std::unordered_map<std::string, bool>& fieldCanEdit) {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& kv : fieldCanEdit) {
        obj[kv.first] = kv.second;
    }
    return obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool ParseEditPermissions(const std::string& json, std::unordered_map<std::string, bool>& out) {
    out.clear();
    std::string err;
    const nlohmann::json parsed = smatchet::json_safe::ParseBounded(json, err);
    if (!err.empty() || !parsed.is_object()) {
        return false;
    }
    out.reserve(parsed.size());
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        if (it.value().is_boolean()) {
            out[it.key()] = it.value().get<bool>();
        }
    }
    return true;
}

} // namespace lookup
} // namespace smatchet
