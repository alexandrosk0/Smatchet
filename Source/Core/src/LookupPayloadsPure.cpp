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

// A stored list is an array of flat objects: depth 2 exactly, which also rules out a depth bomb in a
// tampered row. ParseBounded counts each entry as one node for the object plus one per value (keys are
// free), and one more for the array.
constexpr int kFlatArrayMaxDepth = 2;
constexpr std::size_t kUserFields = 5;
constexpr std::size_t kProjectFields = 3;

std::string StringMember(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// At most `maxItems` entries, each written by `toJson`; "" when the text would exceed `maxBytes`, so a
// stored row always parses back.
template <typename Item, typename ToJson>
std::string SerializeFlatArray(const std::vector<Item>& items, std::size_t maxItems, std::size_t maxBytes,
                               ToJson toJson) {
    nlohmann::json arr = nlohmann::json::array();
    const std::size_t count = std::min(items.size(), maxItems);
    for (std::size_t i = 0; i < count; ++i) {
        arr.push_back(toJson(items[i]));
    }
    // Names and emails are tracker-supplied text; replace invalid UTF-8 instead of throwing.
    std::string out = arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return out.size() > maxBytes ? std::string() : out;
}

// Inverse of SerializeFlatArray, bounded to what it can have written. `fromJson(entry, item)` returns
// false to skip an entry.
template <typename Item, typename FromJson>
bool ParseFlatArray(const std::string& json, std::size_t maxItems, std::size_t maxBytes, std::size_t fieldsPerItem,
                    std::vector<Item>& out, FromJson fromJson) {
    out.clear();
    std::string err;
    const nlohmann::json parsed = smatchet::json_safe::ParseBounded(json, err, maxBytes, kFlatArrayMaxDepth,
                                                                    (fieldsPerItem + 1u) * maxItems + 1u);
    if (!err.empty() || !parsed.is_array()) {
        return false;
    }
    out.reserve(parsed.size());
    for (const nlohmann::json& entry : parsed) {
        Item item;
        if (entry.is_object() && fromJson(entry, item)) {
            out.push_back(std::move(item));
        }
    }
    return true;
}

} // namespace

std::string SerializeUsers(const std::vector<TrackerUser>& users) {
    return SerializeFlatArray(users, kMaxStoredUsers, kMaxUsersPayloadBytes, [](const TrackerUser& user) {
        return nlohmann::json::object({{"accountId", user.AccountId},
                                       {"displayName", user.DisplayName},
                                       {"email", user.EmailAddress},
                                       {"accountType", user.AccountType},
                                       {"active", user.Active}});
    });
}

bool ParseUsers(const std::string& json, std::vector<TrackerUser>& out) {
    return ParseFlatArray(json, kMaxStoredUsers, kMaxUsersPayloadBytes, kUserFields, out,
                          [](const nlohmann::json& entry, TrackerUser& user) {
                              user.AccountId = StringMember(entry, "accountId");
                              if (user.AccountId.empty()) {
                                  return false;
                              }
                              user.DisplayName = StringMember(entry, "displayName");
                              user.EmailAddress = StringMember(entry, "email");
                              user.AccountType = StringMember(entry, "accountType");
                              const auto active = entry.find("active");
                              user.Active = active == entry.end() || !active->is_boolean() || active->get<bool>();
                              return true;
                          });
}

std::string SerializeProjects(const std::vector<RemoteProject>& projects) {
    return SerializeFlatArray(projects, kMaxStoredProjects, kMaxProjectsPayloadBytes, [](const RemoteProject& p) {
        return nlohmann::json::object({{"id", p.id}, {"key", p.key}, {"name", p.displayName}});
    });
}

bool ParseProjects(const std::string& json, std::vector<RemoteProject>& out) {
    return ParseFlatArray(json, kMaxStoredProjects, kMaxProjectsPayloadBytes, kProjectFields, out,
                          [](const nlohmann::json& entry, RemoteProject& p) {
                              p.id = StringMember(entry, "id");
                              p.key = StringMember(entry, "key");
                              p.displayName = StringMember(entry, "name");
                              return !p.id.empty() || !p.key.empty();
                          });
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
