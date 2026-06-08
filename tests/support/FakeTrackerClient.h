#ifndef SMATCHET_TESTS_FAKE_TRACKER_CLIENT_H
#define SMATCHET_TESTS_FAKE_TRACKER_CLIENT_H

// FakeTrackerClient — scripted in-memory implementation of ITrackerBackend for the doctest rig.
// No HTTP, no SQLite, no threads — every method either:
//   * returns a pre-scripted response (`OnCreateIssue(...).ReturnKey("ABC-1")`), or
//   * fails with a pre-scripted error (`OnUpdateIssueFields(...).Fail("Server error 500")`), or
//   * falls back to a sensible default (empty result, success without side-effects).
//
// Every call is recorded so tests can assert "PUT called once with this payload" without
// needing a separate spy layer.
//
// The class lives in tests/support/ and is header-only so test TUs can include it without
// CMake glue beyond `target_include_directories(... tests/support)`.

#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smatchet_tests {

/// One scripted result for an arbitrary call. Either succeeds with `Value` or fails by
/// setting `Error` (non-empty). Number-keyed (status_code) ignored for the high-level
/// ITrackerBackend surface — used only by call recordings to drive richer tests.
struct ScriptedReply {
    bool Ok = true;
    std::string Error;
    /// For BuildXPayload / FetchFieldCatalog / etc — the JSON payload returned.
    nlohmann::json Payload = nlohmann::json::object();
    /// For CreateIssue — the issue key to return on success (empty when Ok=false).
    std::string IssueKey;
};

struct ScriptedFetchResult {
    std::vector<CachedTicket> Tickets;
    bool FullSyncCompleted = true;
    std::string FetchError;
    std::string Warning;
};

struct CreateIssueCall {
    nlohmann::json Fields;
};

struct UpdateIssueFieldsCall {
    std::string IssueId;
    nlohmann::json Fields;
};

struct AddIssueToSprintCall {
    std::string IssueKey;
    std::string SprintId;
};

struct AttachFilesCall {
    std::string IssueKey;
    std::vector<std::string> Paths;
};

struct UpdateFieldCall {
    std::string IssueId;
    std::string FieldId;
    std::vector<std::string> Values;
};

class FakeTrackerClient : public ITrackerBackend,
                          public ITrackerIssueReader,
                          public ITrackerConnectivity,
                          public ITrackerIssueMutations,
                          public ITrackerFieldCatalog,
                          public ITrackerCollaboration {
  public:
    FakeTrackerClient() = default;
    explicit FakeTrackerClient(std::string trackerType) : trackerType_(std::move(trackerType)) {}

    // --- ITrackerBackend accessors -----------------------------------------------------------
    ITrackerIssueReader& Reader() override { return *this; }
    ITrackerConnectivity& Connectivity() override { return *this; }
    ITrackerFieldCatalog* FieldCatalog() override { return nullptr; }
    ITrackerIssueMutations* Mutations() override { return this; }
    ITrackerCollaboration* Collaboration() override { return nullptr; }

    // --- Capability interface surface --------------------------------------------------------

    std::string GetTrackerType() const override { return trackerType_; }

    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& /*cfg*/) override {
        ++probeReachabilityCalls_;
        return reachabilityResult_;
    }

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const TrackerConfig* /*configOverride*/ = nullptr,
                                          const ViewsStore* /*viewsOverride*/ = nullptr,
                                          std::string* outFetchError = nullptr,
                                          std::string* outWarning = nullptr) override {
        ++fetchIssuesCalls_;
        if (!fetchQueue_.empty()) {
            ScriptedFetchResult r = std::move(fetchQueue_.front());
            fetchQueue_.pop_front();
            if (outFullSyncCompleted)
                *outFullSyncCompleted = r.FullSyncCompleted;
            if (outFetchError)
                *outFetchError = r.FetchError;
            if (outWarning)
                *outWarning = r.Warning;
            return std::move(r.Tickets);
        }
        if (outFullSyncCompleted)
            *outFullSyncCompleted = fetchFullSyncCompleted_;
        if (outFetchError)
            *outFetchError = fetchError_;
        if (outWarning)
            *outWarning = fetchWarning_;
        return fetchTickets_;
    }

    bool FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& issueKeys,
                            const ViewsStore& /*views*/, std::vector<CachedTicket>& outTickets,
                            std::string& outError) override {
        ++fetchIssuesForKeysCalls_;
        fetchIssuesForKeysLastKeys_ = issueKeys;
        if (!fetchIssuesForKeysOk_) {
            outError = fetchIssuesForKeysError_;
            return false;
        }
        outTickets = fetchIssuesForKeysTickets_;
        return true;
    }

    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override {
        UpdateIssueFieldsCall call;
        call.IssueId = issueId;
        call.Fields = fields;
        updateIssueFieldsCalls_.push_back(std::move(call));
        const ScriptedReply reply = NextOrDefault(updateIssueFieldsQueue_, defaultUpdateIssueFields_);
        if (!reply.Ok) {
            return TrackerErrorInvalidRequest(reply.Error);
        }
        return TrackerError::Ok();
    }

    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override {
        UpdateFieldCall call;
        call.IssueId = issueId;
        call.FieldId = field.Id;
        call.Values = values;
        updateFieldCalls_.push_back(std::move(call));
        const ScriptedReply reply = NextOrDefault(updateFieldQueue_, defaultUpdateField_);
        if (!reply.Ok) {
            return TrackerErrorInvalidRequest(reply.Error);
        }
        return TrackerError::Ok();
    }

    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& /*field*/,
                                                           const std::vector<std::string>& values) override {
        ++buildFieldPayloadCalls_;
        if (!buildFieldPayloadOk_) {
            return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(buildFieldPayloadError_));
        }
        // Default: stuff values as an array under "values" — tests rarely care about the exact
        // shape here because the production ITrackerBackend impls own the wire format.
        nlohmann::json arr = nlohmann::json::array();
        std::copy(values.begin(), values.end(), std::back_inserter(arr));
        return Result<nlohmann::json, TrackerError>::Ok(nlohmann::json{{"values", std::move(arr)}});
    }

    Result<nlohmann::json, TrackerError> BuildCreatePayload(const IssueDraft& /*draft*/,
                                                            const std::vector<TrackerField>& /*catalog*/) override {
        ++buildCreatePayloadCalls_;
        if (!buildCreatePayloadOk_) {
            return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(buildCreatePayloadError_));
        }
        return Result<nlohmann::json, TrackerError>::Ok(buildCreatePayloadResult_);
    }

    Result<nlohmann::json, TrackerError> BuildUpdatePayload(const IssueDraft& /*draft*/,
                                                            const std::vector<TrackerField>& /*catalog*/) override {
        ++buildUpdatePayloadCalls_;
        if (!buildUpdatePayloadOk_) {
            return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(buildUpdatePayloadError_));
        }
        return Result<nlohmann::json, TrackerError>::Ok(buildUpdatePayloadResult_);
    }

    std::string ResolveDisplayValue(const std::string& /*fieldId*/, const TrackerField* /*field*/,
                                    const std::string& value) const override {
        return value;
    }

    std::string CreateIssue(const nlohmann::json& fields, std::string& outError) override {
        CreateIssueCall call;
        call.Fields = fields;
        createIssueCalls_.push_back(std::move(call));
        const ScriptedReply reply = NextOrDefault(createIssueQueue_, defaultCreateIssue_);
        if (!reply.Ok) {
            outError = reply.Error;
            return std::string();
        }
        return reply.IssueKey;
    }

    bool AttachFilesToIssue(const std::string& issueKey, const std::vector<std::string>& absolutePaths,
                            std::vector<std::pair<std::string, std::string>>& outFailures,
                            std::string& outError) override {
        AttachFilesCall call;
        call.IssueKey = issueKey;
        call.Paths = absolutePaths;
        attachFilesCalls_.push_back(std::move(call));
        if (!attachFilesOk_) {
            outError = attachFilesError_;
            return false;
        }
        outFailures = attachFilesFailures_;
        return true;
    }

    TrackerError AddIssueToSprint(const std::string& issueKey, const std::string& sprintId) override {
        AddIssueToSprintCall call;
        call.IssueKey = issueKey;
        call.SprintId = sprintId;
        addIssueToSprintCalls_.push_back(std::move(call));
        const ScriptedReply reply = NextOrDefault(addIssueToSprintQueue_, defaultAddIssueToSprint_);
        if (!reply.Ok) {
            return TrackerErrorInvalidRequest(reply.Error);
        }
        return TrackerError::Ok();
    }

    // --- Scripting helpers (call recording) --------------------------------------------------

    std::size_t CreateIssueCallCount() const { return createIssueCalls_.size(); }
    const std::vector<CreateIssueCall>& CreateIssueCalls() const { return createIssueCalls_; }

    std::size_t UpdateIssueFieldsCallCount() const { return updateIssueFieldsCalls_.size(); }
    const std::vector<UpdateIssueFieldsCall>& UpdateIssueFieldsCalls() const { return updateIssueFieldsCalls_; }

    std::size_t UpdateFieldCallCount() const { return updateFieldCalls_.size(); }
    const std::vector<UpdateFieldCall>& UpdateFieldCalls() const { return updateFieldCalls_; }

    std::size_t AddIssueToSprintCallCount() const { return addIssueToSprintCalls_.size(); }
    const std::vector<AddIssueToSprintCall>& AddIssueToSprintCalls() const { return addIssueToSprintCalls_; }

    std::size_t AttachFilesCallCount() const { return attachFilesCalls_.size(); }
    const std::vector<AttachFilesCall>& AttachFilesCalls() const { return attachFilesCalls_; }

    std::size_t ProbeReachabilityCallCount() const { return probeReachabilityCalls_; }
    std::size_t FetchIssuesCallCount() const { return fetchIssuesCalls_; }
    std::size_t FetchIssuesForKeysCallCount() const { return fetchIssuesForKeysCalls_; }
    std::size_t BuildCreatePayloadCallCount() const { return buildCreatePayloadCalls_; }
    std::size_t BuildUpdatePayloadCallCount() const { return buildUpdatePayloadCalls_; }
    std::size_t BuildFieldPayloadCallCount() const { return buildFieldPayloadCalls_; }

    // --- Scripting helpers (response setup) --------------------------------------------------
    // Pattern: queues are consumed FIFO; once empty the `default*_` reply is used (so a test
    // that scripts 3 5xx-then-200 sequences leaves the default at 200 for any extra retries).

    void EnqueueCreateIssueSuccess(const std::string& issueKey) {
        ScriptedReply r;
        r.Ok = true;
        r.IssueKey = issueKey;
        createIssueQueue_.push_back(std::move(r));
    }
    void EnqueueCreateIssueFailure(const std::string& error) {
        ScriptedReply r;
        r.Ok = false;
        r.Error = error;
        createIssueQueue_.push_back(std::move(r));
    }
    void SetDefaultCreateIssueResult(bool ok, const std::string& issueKeyOrError) {
        defaultCreateIssue_.Ok = ok;
        if (ok) {
            defaultCreateIssue_.IssueKey = issueKeyOrError;
            defaultCreateIssue_.Error.clear();
        } else {
            defaultCreateIssue_.IssueKey.clear();
            defaultCreateIssue_.Error = issueKeyOrError;
        }
    }

    void EnqueueUpdateIssueFieldsSuccess() {
        ScriptedReply r;
        r.Ok = true;
        updateIssueFieldsQueue_.push_back(std::move(r));
    }
    void EnqueueUpdateIssueFieldsFailure(const std::string& error) {
        ScriptedReply r;
        r.Ok = false;
        r.Error = error;
        updateIssueFieldsQueue_.push_back(std::move(r));
    }
    void SetDefaultUpdateIssueFieldsResult(bool ok, const std::string& error = std::string()) {
        defaultUpdateIssueFields_.Ok = ok;
        defaultUpdateIssueFields_.Error = error;
    }

    void EnqueueUpdateFieldSuccess() {
        ScriptedReply r;
        r.Ok = true;
        updateFieldQueue_.push_back(std::move(r));
    }
    void EnqueueUpdateFieldFailure(const std::string& error) {
        ScriptedReply r;
        r.Ok = false;
        r.Error = error;
        updateFieldQueue_.push_back(std::move(r));
    }
    void SetDefaultUpdateFieldResult(bool ok, const std::string& error = std::string()) {
        defaultUpdateField_.Ok = ok;
        defaultUpdateField_.Error = error;
    }

    void EnqueueAddIssueToSprintSuccess() {
        ScriptedReply r;
        r.Ok = true;
        addIssueToSprintQueue_.push_back(std::move(r));
    }
    void EnqueueAddIssueToSprintFailure(const std::string& error) {
        ScriptedReply r;
        r.Ok = false;
        r.Error = error;
        addIssueToSprintQueue_.push_back(std::move(r));
    }
    void SetDefaultAddIssueToSprintResult(bool ok, const std::string& error = std::string()) {
        defaultAddIssueToSprint_.Ok = ok;
        defaultAddIssueToSprint_.Error = error;
    }

    void SetBuildCreatePayloadResult(bool ok, nlohmann::json payload, const std::string& error = std::string()) {
        buildCreatePayloadOk_ = ok;
        buildCreatePayloadResult_ = std::move(payload);
        buildCreatePayloadError_ = error;
    }
    void SetBuildUpdatePayloadResult(bool ok, nlohmann::json payload, const std::string& error = std::string()) {
        buildUpdatePayloadOk_ = ok;
        buildUpdatePayloadResult_ = std::move(payload);
        buildUpdatePayloadError_ = error;
    }
    void SetBuildFieldPayloadResult(bool ok, const std::string& error = std::string()) {
        buildFieldPayloadOk_ = ok;
        buildFieldPayloadError_ = error;
    }

    void SetAttachFilesResult(bool ok, std::vector<std::pair<std::string, std::string>> failures = {},
                              const std::string& error = std::string()) {
        attachFilesOk_ = ok;
        attachFilesFailures_ = std::move(failures);
        attachFilesError_ = error;
    }

    void SetFetchIssuesResult(std::vector<CachedTicket> tickets, bool fullSyncCompleted = true,
                              const std::string& fetchError = std::string(),
                              const std::string& warning = std::string()) {
        fetchTickets_ = std::move(tickets);
        fetchFullSyncCompleted_ = fullSyncCompleted;
        fetchError_ = fetchError;
        fetchWarning_ = warning;
    }

    // Enqueue a sequenced fetch result consumed FIFO; once the queue is drained FetchIssues
    // falls back to the static SetFetchIssuesResult values. Lets fixture-backed scenarios
    // script "first fetch succeeds, second returns transport error" without resetting state.
    void EnqueueFetchResult(std::vector<CachedTicket> tickets, bool fullSyncCompleted = true,
                            const std::string& fetchError = std::string(), const std::string& warning = std::string()) {
        ScriptedFetchResult r;
        r.Tickets = std::move(tickets);
        r.FullSyncCompleted = fullSyncCompleted;
        r.FetchError = fetchError;
        r.Warning = warning;
        fetchQueue_.push_back(std::move(r));
    }

    void SetFetchIssuesForKeysResult(bool ok, std::vector<CachedTicket> tickets,
                                     const std::string& error = std::string()) {
        fetchIssuesForKeysOk_ = ok;
        fetchIssuesForKeysTickets_ = std::move(tickets);
        fetchIssuesForKeysError_ = error;
    }
    const std::vector<std::string>& FetchIssuesForKeysLastKeys() const { return fetchIssuesForKeysLastKeys_; }

    void SetReachabilityResult(TrackerReachabilityProbeKind kind, const std::string& diagnostic = std::string()) {
        reachabilityResult_.Kind = kind;
        reachabilityResult_.Diagnostic = diagnostic;
    }

    // Whole-recorder reset (between test sub-cases that share a fixture).
    void ResetCalls() {
        createIssueCalls_.clear();
        updateIssueFieldsCalls_.clear();
        updateFieldCalls_.clear();
        addIssueToSprintCalls_.clear();
        attachFilesCalls_.clear();
        fetchQueue_.clear();
        probeReachabilityCalls_ = 0;
        fetchIssuesCalls_ = 0;
        fetchIssuesForKeysCalls_ = 0;
        buildCreatePayloadCalls_ = 0;
        buildUpdatePayloadCalls_ = 0;
        buildFieldPayloadCalls_ = 0;
    }

  private:
    static ScriptedReply NextOrDefault(std::deque<ScriptedReply>& queue, const ScriptedReply& fallback) {
        if (queue.empty())
            return fallback;
        ScriptedReply r = std::move(queue.front());
        queue.pop_front();
        return r;
    }

    std::string trackerType_ = "fake";

    // Reachability
    TrackerReachabilityProbeResult reachabilityResult_{TrackerReachabilityProbeKind::AuthenticatedReachable, ""};
    std::size_t probeReachabilityCalls_ = 0;

    // FetchIssues — static fallback + sequenced queue (FIFO, drained before fallback)
    std::deque<ScriptedFetchResult> fetchQueue_;
    std::vector<CachedTicket> fetchTickets_;
    bool fetchFullSyncCompleted_ = true;
    std::string fetchError_;
    std::string fetchWarning_;
    std::size_t fetchIssuesCalls_ = 0;

    // FetchIssuesForKeys
    bool fetchIssuesForKeysOk_ = true;
    std::vector<CachedTicket> fetchIssuesForKeysTickets_;
    std::string fetchIssuesForKeysError_;
    std::vector<std::string> fetchIssuesForKeysLastKeys_;
    std::size_t fetchIssuesForKeysCalls_ = 0;

    // CreateIssue
    std::deque<ScriptedReply> createIssueQueue_;
    ScriptedReply defaultCreateIssue_{true, "", nlohmann::json::object(), "FAKE-1"};
    std::vector<CreateIssueCall> createIssueCalls_;

    // UpdateIssueFields
    std::deque<ScriptedReply> updateIssueFieldsQueue_;
    ScriptedReply defaultUpdateIssueFields_{true, "", nlohmann::json::object(), ""};
    std::vector<UpdateIssueFieldsCall> updateIssueFieldsCalls_;

    // UpdateField
    std::deque<ScriptedReply> updateFieldQueue_;
    ScriptedReply defaultUpdateField_{true, "", nlohmann::json::object(), ""};
    std::vector<UpdateFieldCall> updateFieldCalls_;

    // AddIssueToSprint
    std::deque<ScriptedReply> addIssueToSprintQueue_;
    ScriptedReply defaultAddIssueToSprint_{true, "", nlohmann::json::object(), ""};
    std::vector<AddIssueToSprintCall> addIssueToSprintCalls_;

    // AttachFiles
    bool attachFilesOk_ = true;
    std::vector<std::pair<std::string, std::string>> attachFilesFailures_;
    std::string attachFilesError_;
    std::vector<AttachFilesCall> attachFilesCalls_;

    // BuildXPayload
    bool buildCreatePayloadOk_ = true;
    nlohmann::json buildCreatePayloadResult_ = nlohmann::json::object();
    std::string buildCreatePayloadError_;
    std::size_t buildCreatePayloadCalls_ = 0;

    bool buildUpdatePayloadOk_ = true;
    nlohmann::json buildUpdatePayloadResult_ = nlohmann::json::object();
    std::string buildUpdatePayloadError_;
    std::size_t buildUpdatePayloadCalls_ = 0;

    bool buildFieldPayloadOk_ = true;
    std::string buildFieldPayloadError_;
    std::size_t buildFieldPayloadCalls_ = 0;
};

} // namespace smatchet_tests

#endif
