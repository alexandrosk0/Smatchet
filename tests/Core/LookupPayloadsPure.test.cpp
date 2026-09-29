// LookupPayloadsPure — the user-roster, project-list and edit-permission payloads of the offline
// lookup rows (Quality Pillar 6). Pure: no I/O.

#include "LookupPayloadsPure.h"

#include <doctest/doctest.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace {

TrackerUser User(const std::string& id, const std::string& name) {
    TrackerUser user;
    user.AccountId = id;
    user.DisplayName = name;
    user.EmailAddress = name + "@example.test";
    user.AccountType = "atlassian";
    return user;
}

} // namespace

TEST_SUITE("LookupPayloadsPure") {

    TEST_CASE("a user roster round-trips every member") {
        TrackerUser inactive = User("acc-2", "Bob");
        inactive.Active = false;
        const std::string json = smatchet::lookup::SerializeUsers({User("acc-1", "Ana"), inactive});

        std::vector<TrackerUser> parsed;
        REQUIRE(smatchet::lookup::ParseUsers(json, parsed));
        REQUIRE(parsed.size() == 2u);
        CHECK(parsed[0].AccountId == "acc-1");
        CHECK(parsed[0].DisplayName == "Ana");
        CHECK(parsed[0].EmailAddress == "Ana@example.test");
        CHECK(parsed[0].AccountType == "atlassian");
        CHECK(parsed[0].Active);
        CHECK_FALSE(parsed[1].Active);
    }

    TEST_CASE("roster entries without an account id are skipped; mistyped members read as absent") {
        std::vector<TrackerUser> parsed;
        REQUIRE(smatchet::lookup::ParseUsers(
            R"([{"displayName":"No id"},{"accountId":"acc-1","displayName":5,"active":"yes"},7])", parsed));
        REQUIRE(parsed.size() == 1u);
        CHECK(parsed[0].AccountId == "acc-1");
        CHECK(parsed[0].DisplayName.empty());
        CHECK(parsed[0].Active); // a non-bool `active` keeps the default
    }

    TEST_CASE("an unreadable roster row is rejected") {
        std::vector<TrackerUser> parsed{User("acc-1", "Ana")};
        CHECK_FALSE(smatchet::lookup::ParseUsers("{", parsed));
        CHECK(parsed.empty());
        CHECK_FALSE(smatchet::lookup::ParseUsers(R"({"accountId":"acc-1"})", parsed));
        // A roster row is exactly two levels deep; anything nested deeper is refused.
        CHECK_FALSE(smatchet::lookup::ParseUsers(R"([{"accountId":"acc-1","extra":{"x":1}}])", parsed));
    }

    TEST_CASE("the roster is capped at the stored-user bound and stays parseable") {
        std::vector<TrackerUser> users;
        users.reserve(smatchet::lookup::kMaxStoredUsers + 3);
        for (std::size_t i = 0; i < smatchet::lookup::kMaxStoredUsers + 3; ++i) {
            users.push_back(User("a" + std::to_string(i), "U" + std::to_string(i)));
        }
        const std::string json = smatchet::lookup::SerializeUsers(users);
        REQUIRE_FALSE(json.empty());

        std::vector<TrackerUser> parsed;
        REQUIRE(smatchet::lookup::ParseUsers(json, parsed));
        CHECK(parsed.size() == smatchet::lookup::kMaxStoredUsers);
    }

    TEST_CASE("a roster too large to store serializes to nothing") {
        TrackerUser huge = User("acc-1", std::string(smatchet::lookup::kMaxUsersPayloadBytes, 'x'));
        CHECK(smatchet::lookup::SerializeUsers({huge}).empty());
    }

    TEST_CASE("a project list round-trips; entries with neither id nor key are skipped") {
        RemoteProject jira;
        jira.id = "10000";
        jira.key = "OFF";
        jira.displayName = "Offline First";
        RemoteProject plane; // Plane rows carry a UUID and may have no identifier
        plane.id = "5f0c-uuid";
        plane.displayName = "Plane Project";
        std::vector<RemoteProject> parsed;
        REQUIRE(smatchet::lookup::ParseProjects(smatchet::lookup::SerializeProjects({jira, plane}), parsed));
        REQUIRE(parsed.size() == 2u);
        CHECK(parsed[0].id == "10000");
        CHECK(parsed[0].key == "OFF");
        CHECK(parsed[0].displayName == "Offline First");
        CHECK(parsed[1].id == "5f0c-uuid");
        CHECK(parsed[1].key.empty());

        REQUIRE(smatchet::lookup::ParseProjects(R"([{"name":"No id or key"},{"key":"K","name":3},"x"])", parsed));
        REQUIRE(parsed.size() == 1u);
        CHECK(parsed[0].key == "K");
        CHECK(parsed[0].displayName.empty());
    }

    TEST_CASE("an unreadable or oversized project list is rejected, and a capped one still parses") {
        std::vector<RemoteProject> parsed(1);
        CHECK_FALSE(smatchet::lookup::ParseProjects("[", parsed));
        CHECK(parsed.empty());
        CHECK_FALSE(smatchet::lookup::ParseProjects(R"({"key":"K"})", parsed));
        CHECK_FALSE(smatchet::lookup::ParseProjects(R"([{"key":"K","nested":[1]}])", parsed));

        std::vector<RemoteProject> many(smatchet::lookup::kMaxStoredProjects + 2);
        for (std::size_t i = 0; i < many.size(); ++i) {
            many[i].key = "P" + std::to_string(i);
        }
        const std::string capped = smatchet::lookup::SerializeProjects(many);
        REQUIRE(smatchet::lookup::ParseProjects(capped, parsed));
        CHECK(parsed.size() == smatchet::lookup::kMaxStoredProjects);

        RemoteProject huge;
        huge.key = std::string(smatchet::lookup::kMaxProjectsPayloadBytes, 'k');
        CHECK(smatchet::lookup::SerializeProjects({huge}).empty());
    }

    TEST_CASE("edit permissions round-trip; non-bool members are skipped") {
        const std::string json = smatchet::lookup::SerializeEditPermissions({{"summary", true}, {"labels", false}});
        std::unordered_map<std::string, bool> parsed;
        REQUIRE(smatchet::lookup::ParseEditPermissions(json, parsed));
        REQUIRE(parsed.size() == 2u);
        CHECK(parsed.at("summary"));
        CHECK_FALSE(parsed.at("labels"));

        REQUIRE(smatchet::lookup::ParseEditPermissions(R"({"summary":true,"labels":"no"})", parsed));
        CHECK(parsed.size() == 1u);
        CHECK_FALSE(smatchet::lookup::ParseEditPermissions("[]", parsed));
        CHECK(parsed.empty());
    }
}
