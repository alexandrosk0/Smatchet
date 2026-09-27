// Pure-logic coverage of the error mapping shared by the AppController collaboration read delegators
// (Source/Core/include/Tracker/CollaborationPreconditionPure.h): the payload passes through, an error
// surfaces its Detail, and an empty Detail still reads as a failure. AppController.cpp is not part of
// the doctest link rig, so the mapping is pinned at this header-only seam. No .cpp to link.

#include "Tracker/CollaborationPreconditionPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::collab::CollaborationResultToResult;

TEST_CASE("CollaborationResultToResult: an Ok payload passes through by move") {
    // The read-side flip surfaces the backend payload unchanged; the vector must arrive intact.
    Result<std::vector<std::string>> r = CollaborationResultToResult<std::vector<std::string>>(
        Result<std::vector<std::string>, TrackerError>::Ok(std::vector<std::string>{"alice", "bob"}));
    REQUIRE(r.has_value());
    REQUIRE(r.value().size() == 2);
    CHECK(r.value()[0] == "alice");
    CHECK(r.value()[1] == "bob");
}

TEST_CASE("CollaborationResultToResult: an error surfaces its Detail verbatim") {
    // The historical bool+outError read contract handed the caller exactly err.Detail.
    Result<std::vector<std::string>> server = CollaborationResultToResult<std::vector<std::string>>(
        Result<std::vector<std::string>, TrackerError>::Err(TrackerErrorServer("Server error 500", 500)));
    REQUIRE_FALSE(server.has_value());
    CHECK(server.error() == "Server error 500");

    Result<std::vector<std::string>> invalid = CollaborationResultToResult<std::vector<std::string>>(
        Result<std::vector<std::string>, TrackerError>::Err(TrackerErrorInvalidRequest("bad query")));
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == "bad query");
}

TEST_CASE("CollaborationResultToResult: an error with an empty Detail gets a non-empty message") {
    // A payload fetch that fails with no Detail is still a failure — never misread as an empty-Ok
    // list, and never surfaced to a caller that treats an empty message as success.
    Result<int> r = CollaborationResultToResult<int>(Result<int, TrackerError>::Err(TrackerErrorInvalidRequest("")));
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
    CHECK(r.error() == std::string(smatchet::collab::CollaborationErrorFallbackMessage()));
}
