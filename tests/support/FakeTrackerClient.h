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
    // Structured twin — script realistic kinds so structured-classification consumers are
    // testable (retire-transport-error-text item 12). Kind None = unclassified legacy path.
    TrackerError FetchErrorStructured;
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
    // EditMetaCacheService (#975 / AppController-decomp Phase 1) reaches the editmeta + per-project
    // component surface through backend->FieldCatalog(). The fake implements ITrackerFieldCatalog
    // itself, so hand back `this`; FetchIssueEditMeta / FetchProjectComponents below are scriptable.
    ITrackerFieldCatalog* FieldCatalog() override { return this; }
    ITrackerIssueMutations* Mutations() override { return this; }
    ITrackerCollaboration* Collaboration() override { return nullptr; }
    ITrackerActivity* Activity() override { return nullptr; }

    // --- Capability interface surface --------------------------------------------------------

    std::string GetTrackerType() const override { return trackerType_; }

    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& /*cfg*/) override {
        ++probeReachabilityCalls_;
        return reachabilityResult_;
    }

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const TrackerConfig* /*configOverride*/ = nullptr,
                                          const ViewsStore* /*viewsOverride*/ = nullptr,
                                          std::string* outFetchError = nullptr, std::string* outWarning = nullptr,
                                          TrackerError* outFetchErrorStructured = nullptr) override {
        ++fetchIssuesCalls_;
        if (!fetchQueue_.empty()) {
            ScriptedFetchResult r = std::move(fetchQueue_.front());
            fetchQueue_.pop_front();
            if (outFullSyncCompleted)
                *outFullSyncCompleted = r.FullSyncCompleted;
            if (outFetchError)
                *outFetchError = r.FetchError;
            if (outFetchErrorStructured)
                *outFetchErrorStructured = r.FetchErrorStructured;
            if (outWarning)
                *outWarning = r.Warning;
            return std::move(r.Tickets);
        }
        if (outFullSyncCompleted)
            *outFullSyncCompleted = fetchFullSyncCompleted_;
        if (outFetchError)
            *outFetchError = fetchError_;
        if (outFetchErrorStructured)
            *outFetchErrorStructured = fetchErrorStructured_;
        if (outWarning)
            *outWarning = fetchWarning_;
        return fetchTickets_;
    }

    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& /*cfg*/,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& /*views*/) override {
        ++fetchIssuesForKeysCalls_;
        fetchIssuesForKeysLastKeys_ = issueKeys;
        if (!fetchIssuesForKeysOk_) {
            // A scripted structured error wins; the legacy string setter keeps its historical
            // InvalidRequest shape so untouched suites see identical behaviour.
            if (!fetchIssuesForKeysStructuredError_.IsOk()) {
                return Result<std::vector<CachedTicket>, TrackerError>::Err(fetchIssuesForKeysStructuredError_);
            }
            return Result<std::vector<CachedTicket>, TrackerError>::Err(
                TrackerErrorInvalidRequest(fetchIssuesForKeysError_));
        }
        return Result<std::vector<CachedTicket>, TrackerError>::Ok(fetchIssuesForKeysTickets_);
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

    // --- ITrackerFieldCatalog editmeta + per-project component surface ------------------------
    // Used by EditMetaCacheService (AppController-decomp Phase 1). FetchIssueEditMeta is keyed by
    // issueId with a static fallback; FetchProjectComponents is keyed by projectKey. Both record
    // every call so tests can assert "fetched once" warm-coalescing semantics.

    Result<std::unordered_map<std::string, bool>, TrackerError>
    FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& issueKeyOrId) override {
        ++fetchIssueEditMetaCalls_;
        fetchIssueEditMetaKeys_.push_back(issueKeyOrId);
        const auto perIssue = issueEditMetaByIssue_.find(issueKeyOrId);
        if (perIssue != issueEditMetaByIssue_.end()) {
            const ScriptedEditMeta& s = perIssue->second;
            if (!s.Ok) {
                return Result<std::unordered_map<std::string, bool>, TrackerError>::Err(
                    TrackerErrorInvalidRequest(s.Error));
            }
            return Result<std::unordered_map<std::string, bool>, TrackerError>::Ok(s.FieldCanEdit);
        }
        if (!issueEditMetaDefault_.Ok) {
            return Result<std::unordered_map<std::string, bool>, TrackerError>::Err(
                TrackerErrorInvalidRequest(issueEditMetaDefault_.Error));
        }
        return Result<std::unordered_map<std::string, bool>, TrackerError>::Ok(issueEditMetaDefault_.FieldCanEdit);
    }

    Result<TrackerProjectComponents, TrackerError> FetchProjectComponents(const TrackerConfig& /*cfg*/,
                                                                          const std::string& projectKey) override {
        ++fetchProjectComponentsCalls_;
        fetchProjectComponentsKeys_.push_back(projectKey);
        const auto it = projectComponentsByKey_.find(projectKey);
        if (it != projectComponentsByKey_.end()) {
            const ScriptedComponents& s = it->second;
            if (!s.Ok) {
                return Result<TrackerProjectComponents, TrackerError>::Err(TrackerErrorInvalidRequest(s.Error));
            }
            return Result<TrackerProjectComponents, TrackerError>::Ok(s.Value);
        }
        if (!projectComponentsDefaultOk_) {
            return Result<TrackerProjectComponents, TrackerError>::Err(
                TrackerErrorInvalidRequest(projectComponentsDefaultError_));
        }
        return Result<TrackerProjectComponents, TrackerError>::Err(
            TrackerErrorInvalidRequest("no scripted components for project"));
    }

    Result<std::string, TrackerError> CreateIssue(const nlohmann::json& fields) override {
        CreateIssueCall call;
        call.Fields = fields;
        createIssueCalls_.push_back(std::move(call));
        const ScriptedReply reply = NextOrDefault(createIssueQueue_, defaultCreateIssue_);
        if (!reply.Ok) {
            return Result<std::string, TrackerError>::Err(TrackerErrorInvalidRequest(reply.Error));
        }
        return Result<std::string, TrackerError>::Ok(reply.IssueKey);
    }

    Result<std::vector<std::pair<std::string, std::string>>, TrackerError>
    AttachFilesToIssue(const std::string& issueKey, const std::vector<std::string>& absolutePaths) override {
        AttachFilesCall call;
        call.IssueKey = issueKey;
        call.Paths = absolutePaths;
        attachFilesCalls_.push_back(std::move(call));
        if (!attachFilesOk_) {
            return Result<std::vector<std::pair<std::string, std::string>>, TrackerError>::Err(
                TrackerErrorInvalidRequest(attachFilesError_));
        }
        return Result<std::vector<std::pair<std::string, std::string>>, TrackerError>::Ok(attachFilesFailures_);
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

    std::size_t FetchIssueEditMetaCallCount() const { return fetchIssueEditMetaCalls_; }
    const std::vector<std::string>& FetchIssueEditMetaKeys() const { return fetchIssueEditMetaKeys_; }
    std::size_t FetchProjectComponentsCallCount() const { return fetchProjectComponentsCalls_; }
    const std::vector<std::string>& FetchProjectComponentsKeys() const { return fetchProjectComponentsKeys_; }

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
        fetchErrorStructured_ = TrackerError::Ok();
        fetchWarning_ = warning;
    }

    /// Script the static-fallback fetch failure with a realistic kind
    /// (retire-transport-error-text item 12); Detail doubles as the string error.
    void SetFetchIssuesError(TrackerError error) {
        fetchError_ = error.Detail;
        fetchErrorStructured_ = std::move(error);
        fetchFullSyncCompleted_ = false;
    }

    // Enqueue a sequenced fetch result consumed FIFO; once the queue is drained FetchIssues
    // falls back to the static SetFetchIssuesResult values. Lets fixture-backed scenarios
    // script "first fetch succeeds, second returns transport error" without resetting state.
    //
    // AUTO-STICKY: each enqueue also mirrors its values into the static fallback, so the LAST
    // enqueued result is re-served verbatim once the queue drains. Without this, an unscripted
    // re-fetch (one more FetchIssues than the test enqueued) fell back to the default
    // fetchTickets_={} + fetchFullSyncCompleted_=true pair — an empty FULL sync, which the
    // streaming-sync stale-pruner reads as "the server has zero tickets" and deletes the
    // grid the previous scripted fetch just populated. Sticky-on-last keeps that re-fetch
    // idempotent (same tickets, same full-sync flag) so it can never trigger stale-pruning.
    void EnqueueFetchResult(std::vector<CachedTicket> tickets, bool fullSyncCompleted = true,
                            const std::string& fetchError = std::string(), const std::string& warning = std::string()) {
        ScriptedFetchResult r;
        r.Tickets = tickets;
        r.FullSyncCompleted = fullSyncCompleted;
        r.FetchError = fetchError;
        r.Warning = warning;
        // Mirror into the static fallback BEFORE moving into the queue so the post-drain
        // re-fetch returns this (the latest) scripted result rather than the cold default.
        fetchTickets_ = std::move(tickets);
        fetchFullSyncCompleted_ = fullSyncCompleted;
        fetchError_ = fetchError;
        fetchWarning_ = warning;
        fetchQueue_.push_back(std::move(r));
    }

    void SetFetchIssuesForKeysResult(bool ok, std::vector<CachedTicket> tickets,
                                     const std::string& error = std::string()) {
        fetchIssuesForKeysOk_ = ok;
        fetchIssuesForKeysTickets_ = std::move(tickets);
        fetchIssuesForKeysError_ = error;
        fetchIssuesForKeysStructuredError_ = TrackerError::Ok();
    }
    /// Script the failure with a realistic kind (retire-transport-error-text item 12) — e.g.
    /// TrackerErrorTransport("timeout") — so structured-classification consumers are testable.
    void SetFetchIssuesForKeysError(TrackerError error) {
        fetchIssuesForKeysOk_ = false;
        fetchIssuesForKeysError_ = error.Detail;
        fetchIssuesForKeysStructuredError_ = std::move(error);
    }
    const std::vector<std::string>& FetchIssuesForKeysLastKeys() const { return fetchIssuesForKeysLastKeys_; }

    // EditMetaCacheService scripting: per-issue + default editmeta (field id -> can-edit map).
    void SetIssueEditMetaSuccess(const std::string& issueId, std::unordered_map<std::string, bool> fieldCanEdit) {
        ScriptedEditMeta s;
        s.Ok = true;
        s.FieldCanEdit = std::move(fieldCanEdit);
        issueEditMetaByIssue_[issueId] = std::move(s);
    }
    void SetIssueEditMetaFailure(const std::string& issueId, const std::string& error) {
        ScriptedEditMeta s;
        s.Ok = false;
        s.Error = error;
        issueEditMetaByIssue_[issueId] = std::move(s);
    }
    void SetDefaultIssueEditMetaSuccess(std::unordered_map<std::string, bool> fieldCanEdit) {
        issueEditMetaDefault_.Ok = true;
        issueEditMetaDefault_.FieldCanEdit = std::move(fieldCanEdit);
        issueEditMetaDefault_.Error.clear();
    }
    void SetDefaultIssueEditMetaFailure(const std::string& error) {
        issueEditMetaDefault_.Ok = false;
        issueEditMetaDefault_.FieldCanEdit.clear();
        issueEditMetaDefault_.Error = error;
    }

    // Per-project component scripting (Options is what EditMetaCacheService stores into
    // GridContextFieldCatalog::projectComponentOptions_).
    void SetProjectComponentsSuccess(const std::string& projectKey, std::vector<TrackerFieldOption> options,
                                     std::vector<TrackerComponent> components = {}) {
        ScriptedComponents s;
        s.Ok = true;
        s.Value.Options = std::move(options);
        s.Value.Components = std::move(components);
        projectComponentsByKey_[projectKey] = std::move(s);
    }
    void SetProjectComponentsFailure(const std::string& projectKey, const std::string& error) {
        ScriptedComponents s;
        s.Ok = false;
        s.Error = error;
        projectComponentsByKey_[projectKey] = std::move(s);
    }

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
    TrackerError fetchIssuesForKeysStructuredError_;
    TrackerError fetchErrorStructured_;
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

    // FetchIssueEditMeta — per-issue + default scripted maps (field id -> can-edit) + recording.
    struct ScriptedEditMeta {
        bool Ok = true;
        std::unordered_map<std::string, bool> FieldCanEdit;
        std::string Error;
    };
    std::unordered_map<std::string, ScriptedEditMeta> issueEditMetaByIssue_;
    ScriptedEditMeta issueEditMetaDefault_{false, {}, "FetchIssueEditMeta not scripted"};
    std::size_t fetchIssueEditMetaCalls_ = 0;
    std::vector<std::string> fetchIssueEditMetaKeys_;

    // FetchProjectComponents — per-project scripted result + recording.
    struct ScriptedComponents {
        bool Ok = true;
        TrackerProjectComponents Value;
        std::string Error;
    };
    std::unordered_map<std::string, ScriptedComponents> projectComponentsByKey_;
    bool projectComponentsDefaultOk_ = false;
    std::string projectComponentsDefaultError_ = "FetchProjectComponents not scripted";
    std::size_t fetchProjectComponentsCalls_ = 0;
    std::vector<std::string> fetchProjectComponentsKeys_;
};

} // namespace smatchet_tests

#endif
