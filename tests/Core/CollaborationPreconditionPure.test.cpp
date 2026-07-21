// Pure-logic coverage of the collaboration preflight + error mapping shared by the AppController
// collaboration delegators (AddIssueWatcher / FetchIssueWatchers / FetchIssueVotes / AddWorklog /
// comment posts). AppController.cpp is not part of the doctest link rig (it drags the UI/Lua/MCP
// chain), so the guard ORDERING and the exact user-facing strings a delegator must preserve can
// only be pinned at this header-only seam (Source/Core/include/Tracker/CollaborationPreconditionPure.h).
// These are the invariants the pending #21 bool+outError → VoidResult flip of those delegators must
// not change. No .cpp to link — the helpers are inline.

#include "Tracker/CollaborationPreconditionPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::collab::ClassifyCollaborationPrecondition;
using smatchet::collab::CollaborationErrorToVoidResult;
using smatchet::collab::CollaborationResultToResult;

TEST_CASE("ClassifyCollaborationPrecondition: all preconditions satisfied returns VoidOk") {
    // Mutation path, not read-only, backend + collaboration both present.
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/false, /*requireWritable=*/true,
                                                           /*hasBackend=*/true, /*hasCollaboration=*/true);
    CHECK(r.has_value());
}

TEST_CASE("ClassifyCollaborationPrecondition: a read-only fetch is NOT gated by read-only mode") {
    // requireWritable=false (a fetch) → read-only mode is irrelevant; only backend/collaboration gate it.
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/true, /*requireWritable=*/false,
                                                           /*hasBackend=*/true, /*hasCollaboration=*/true);
    CHECK(r.has_value());
}

TEST_CASE("ClassifyCollaborationPrecondition: a mutation in read-only mode is blocked") {
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/true, /*requireWritable=*/true,
                                                           /*hasBackend=*/true, /*hasCollaboration=*/true);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "Read-only mode is enabled in Preferences.");
}

TEST_CASE("ClassifyCollaborationPrecondition: read-only is reported BEFORE a missing backend (ordering)") {
    // Load-bearing ordering: a read-only user triggering a mutation must see the actionable
    // read-only message, never the timing-dependent "backend not initialized" — even when both hold.
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/true, /*requireWritable=*/true,
                                                           /*hasBackend=*/false, /*hasCollaboration=*/false);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "Read-only mode is enabled in Preferences.");
}

TEST_CASE("ClassifyCollaborationPrecondition: a missing backend is reported when not read-only") {
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/false, /*requireWritable=*/true,
                                                           /*hasBackend=*/false, /*hasCollaboration=*/false);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "Tracker backend is not initialized.");
}

TEST_CASE("ClassifyCollaborationPrecondition: a caller-supplied backend-absent message overrides the default") {
    // Some delegators historically worded this "Jira backend is not initialized."; passing the
    // message keeps their exact user-facing text when they flip onto this seam.
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/false, /*requireWritable=*/true,
                                                           /*hasBackend=*/false, /*hasCollaboration=*/false,
                                                           "Jira backend is not initialized.");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "Jira backend is not initialized.");
}

TEST_CASE("ClassifyCollaborationPrecondition: the custom backend-absent message does not leak past the backend gate") {
    // A read-only mutation must still report read-only even when a custom backend-absent message is
    // supplied — the override only replaces the backend-missing branch, not the ordering.
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/true, /*requireWritable=*/true,
                                                           /*hasBackend=*/false, /*hasCollaboration=*/false,
                                                           "Jira backend is not initialized.");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "Read-only mode is enabled in Preferences.");
}

TEST_CASE("ClassifyCollaborationPrecondition: backend present but no collaboration capability") {
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/false, /*requireWritable=*/true,
                                                           /*hasBackend=*/true, /*hasCollaboration=*/false);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "Tracker backend does not support collaboration features.");
}

TEST_CASE("ClassifyCollaborationPrecondition: backend-missing is reported before collaboration-missing") {
    // hasCollaboration is only meaningful once a backend exists; a null backend must win.
    const VoidResult r = ClassifyCollaborationPrecondition(/*readOnlyMode=*/false, /*requireWritable=*/false,
                                                           /*hasBackend=*/false, /*hasCollaboration=*/false);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "Tracker backend is not initialized.");
}

TEST_CASE("CollaborationErrorToVoidResult: an Ok TrackerError maps to VoidOk") {
    const VoidResult r = CollaborationErrorToVoidResult(TrackerError::Ok());
    CHECK(r.has_value());
}

TEST_CASE("CollaborationErrorToVoidResult: an error surfaces its Detail verbatim") {
    // The historical bool+outError contract handed the caller exactly err.Detail.
    const VoidResult server = CollaborationErrorToVoidResult(TrackerErrorServer("Server error 500", 500));
    REQUIRE_FALSE(server.has_value());
    CHECK(server.error() == "Server error 500");

    const VoidResult invalid = CollaborationErrorToVoidResult(TrackerErrorInvalidRequest("bad watcher payload"));
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == "bad watcher payload");
}

TEST_CASE("CollaborationErrorToVoidResult: an error with an empty Detail still maps to a (empty) Err") {
    // A failure with no Detail is still a failure — it must not be misread as success.
    TrackerError e = TrackerErrorInvalidRequest("");
    const VoidResult r = CollaborationErrorToVoidResult(e);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().empty());
}

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

TEST_CASE("CollaborationResultToResult: an error with an empty Detail still maps to a (empty) Err") {
    // A payload fetch that fails with no Detail is still a failure — never misread as an empty-Ok list.
    Result<int> r = CollaborationResultToResult<int>(Result<int, TrackerError>::Err(TrackerErrorInvalidRequest("")));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().empty());
}
