#include <doctest/doctest.h>

#include "JqlProjectScope.h"

#include <string>

TEST_CASE("JqlProjectScope::HasProjectScope detects project clauses") {
    using JqlProjectScope::HasProjectScope;

    CHECK(HasProjectScope("project = FOO"));
    CHECK(HasProjectScope("project=FOO"));
    CHECK(HasProjectScope("PROJECT = FOO AND status = Open"));
    CHECK(HasProjectScope("status = Open AND project = FOO"));
    CHECK(HasProjectScope("project in (FOO, BAR)"));

    CHECK_FALSE(HasProjectScope(""));
    CHECK_FALSE(HasProjectScope("status = Open"));
    CHECK_FALSE(HasProjectScope("assignee = currentUser()"));
    CHECK_FALSE(HasProjectScope("subproject = FOO"));
}

TEST_CASE("JqlProjectScope::ExtractSingleProject returns key for single-project JQL") {
    using JqlProjectScope::ExtractSingleProject;

    CHECK(ExtractSingleProject("project = FOO") == "FOO");
    CHECK(ExtractSingleProject("PROJECT = bar AND status = Open") == "bar");
    CHECK(ExtractSingleProject("project = \"FOO BAR\"") == "FOO BAR");
}

TEST_CASE("JqlProjectScope::ExtractSingleProject returns empty for ambiguous JQL") {
    using JqlProjectScope::ExtractSingleProject;

    CHECK(ExtractSingleProject("") == "");
    CHECK(ExtractSingleProject("status = Open") == "");
    CHECK(ExtractSingleProject("project in (FOO, BAR)") == "");
}

TEST_CASE("JqlProjectScope::SetProjectClause replaces existing scope") {
    using JqlProjectScope::SetProjectClause;

    CHECK(SetProjectClause("project = OLD", "NEW") == "project = NEW");
    CHECK(SetProjectClause("project = OLD AND status = Open", "NEW")
          == "project = NEW AND status = Open");
    CHECK(SetProjectClause("status = Open AND project = OLD", "NEW")
          == "status = Open AND project = NEW");
    CHECK(SetProjectClause("project in (FOO, BAR)", "NEW") == "project = NEW");
}

TEST_CASE("JqlProjectScope::SetProjectClause prepends when no scope exists") {
    using JqlProjectScope::SetProjectClause;

    CHECK(SetProjectClause("status = Open", "NEW") == "project = NEW AND status = Open");
    CHECK(SetProjectClause("", "NEW") == "project = NEW");
}

TEST_CASE("JqlProjectScope::SetProjectClause returns input when projectKey is empty") {
    using JqlProjectScope::SetProjectClause;

    CHECK(SetProjectClause("project = FOO AND status = Open", "")
          == "project = FOO AND status = Open");
    CHECK(SetProjectClause("status = Open", "") == "status = Open");
}
