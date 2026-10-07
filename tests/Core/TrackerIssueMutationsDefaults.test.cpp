// ITrackerIssueMutations::UpdateField's default body — the set-replace single-field edit the
// payload-building backends share (Jira, Plane and Linear inherit it; GitHub guards commit keys
// and then calls it): build the one-field payload, send it through UpdateIssueFields, and return
// a payload error without making the request.

#include "ITrackerIssueMutations.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

// Implements only the two pure virtuals, so UpdateField runs the interface default.
class RecordingMutations : public ITrackerIssueMutations {
  public:
    bool PayloadOk = true;
    TrackerError UpdateResult = TrackerError::Ok();
    int BuildCalls = 0;
    int UpdateCalls = 0;
    std::string UpdatedIssueId;
    nlohmann::json UpdatedFields;

    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override {
        ++UpdateCalls;
        UpdatedIssueId = issueId;
        UpdatedFields = fields;
        return UpdateResult;
    }

    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override {
        ++BuildCalls;
        if (!PayloadOk) {
            return Result<nlohmann::json, TrackerError>::Err(
                TrackerErrorInvalidRequest("Field not supported for update: " + field.Id));
        }
        nlohmann::json payload = nlohmann::json::object();
        payload[field.Id] = values;
        return Result<nlohmann::json, TrackerError>::Ok(std::move(payload));
    }
};

// GitHub's shape: a guard that rejects some ids up front, then the shared default.
class GuardedMutations : public RecordingMutations {
  public:
    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override {
        if (issueId.compare(0, 7, "commit:") == 0) {
            return TrackerErrorInvalidRequest("commits are read-only");
        }
        return ITrackerIssueMutations::UpdateField(issueId, field, values);
    }
};

TrackerField MakeField(const char* id) {
    TrackerField f;
    f.Id = id;
    f.Name = id;
    return f;
}

} // namespace

TEST_SUITE("TrackerIssueMutationsDefaults") {

    TEST_CASE("UpdateField sends the built one-field payload through UpdateIssueFields") {
        RecordingMutations m;
        const TrackerError err = m.UpdateField("SMT-1", MakeField("labels"), {"bug", "p1"});
        CHECK(err.IsOk());
        CHECK(m.BuildCalls == 1);
        REQUIRE(m.UpdateCalls == 1);
        CHECK(m.UpdatedIssueId == "SMT-1");
        nlohmann::json expected = nlohmann::json::object();
        expected["labels"] = std::vector<std::string>{"bug", "p1"};
        CHECK(m.UpdatedFields == expected);
    }

    TEST_CASE("UpdateField returns the payload error without sending a request") {
        RecordingMutations m;
        m.PayloadOk = false;
        const TrackerError err = m.UpdateField("SMT-1", MakeField("resolution"), {"Done"});
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail == "Field not supported for update: resolution");
        CHECK(m.BuildCalls == 1);
        CHECK(m.UpdateCalls == 0);
    }

    TEST_CASE("UpdateField keeps the error kind UpdateIssueFields classified") {
        RecordingMutations m;
        m.UpdateResult = TrackerErrorTransport("tracker unreachable");
        const TrackerError err = m.UpdateField("SMT-2", MakeField("summary"), {"New title"});
        CHECK(err.Kind == TrackerErrorKind::Transport);
        CHECK(err.Detail == "tracker unreachable");
        CHECK(m.UpdateCalls == 1);
    }

    TEST_CASE("an override that guards and then calls the default routes through it once") {
        GuardedMutations m;
        const TrackerError rejected = m.UpdateField("commit:abc123", MakeField("labels"), {"x"});
        CHECK(rejected.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(m.BuildCalls == 0);
        CHECK(m.UpdateCalls == 0);

        const TrackerError ok = m.UpdateField("SMT-3", MakeField("labels"), {"x"});
        CHECK(ok.IsOk());
        CHECK(m.BuildCalls == 1);
        CHECK(m.UpdateCalls == 1);
        CHECK(m.UpdatedIssueId == "SMT-3");
    }
}
