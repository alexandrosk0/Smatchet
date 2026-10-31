#include <doctest/doctest.h>

#include "Ui/CachedProjectsPure.h"

#include <string>
#include <vector>

namespace {

struct Entry {
    std::string projectKey;
    std::string backend;
    std::string endpoint;
};

Entry E(const char* project, const char* backend = "Jira", const char* endpoint = "acme.atlassian.net") {
    return Entry{project, backend, endpoint};
}

} // namespace

TEST_CASE("RemoveForgottenProjects: a read that predates a Forget does not bring the entry back") {
    std::vector<Entry> read = {E("FOO"), E("BAR"), E("BAZ")}; // the list a worker read before the click
    const std::vector<Entry> forgotten = {E("BAR")};
    CHECK(smatchet::ui::RemoveForgottenProjects(read, forgotten) == 1u);
    REQUIRE(read.size() == 2u);
    CHECK(read[0].projectKey == "FOO");
    CHECK(read[1].projectKey == "BAZ");
}

TEST_CASE("RemoveForgottenProjects: only the exact project, backend and endpoint is dropped") {
    std::vector<Entry> read = {E("FOO", "Jira", "acme.atlassian.net"), E("FOO", "Jira", "other.atlassian.net"),
                               E("FOO", "Plane", "acme.atlassian.net")};
    const std::vector<Entry> forgotten = {E("FOO", "Jira", "acme.atlassian.net")};
    CHECK(smatchet::ui::RemoveForgottenProjects(read, forgotten) == 1u);
    REQUIRE(read.size() == 2u);
    CHECK(read[0].endpoint == "other.atlassian.net");
    CHECK(read[1].backend == "Plane");
}

TEST_CASE("RemoveForgottenProjects: nothing queued, or nothing matching, changes nothing") {
    std::vector<Entry> read = {E("FOO"), E("BAR")};
    CHECK(smatchet::ui::RemoveForgottenProjects(read, std::vector<Entry>()) == 0u);
    CHECK(smatchet::ui::RemoveForgottenProjects(read, std::vector<Entry>{E("NOPE")}) == 0u);
    CHECK(read.size() == 2u);
}

TEST_CASE("RemoveForgottenProjects: several queued Forgets are all applied") {
    std::vector<Entry> read = {E("A"), E("B"), E("C"), E("D")};
    CHECK(smatchet::ui::RemoveForgottenProjects(read, std::vector<Entry>{E("B"), E("D")}) == 2u);
    REQUIRE(read.size() == 2u);
    CHECK(read[0].projectKey == "A");
    CHECK(read[1].projectKey == "C");
}
