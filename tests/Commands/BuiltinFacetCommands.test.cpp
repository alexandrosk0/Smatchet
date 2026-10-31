// TEST_COVERAGE_GAP_MAP.md Tier 3 — handler-BODY coverage for the facet-based
// Builtin/BuiltinCommands_*.cpp TUs inside the primary doctest rig. The scenario-lane
// sweep (CommandContractSweepScenario) and the Linux-only SmatchetCommandsTests dispatch
// harness exercise registration + the pre-handler pipeline against a real AppController;
// what neither delivers is these TUs in the OpenCppCoverage denominator with their
// handler bodies driven. The eight TUs linked here take narrow IApp* facets (or only
// IMainThreadPoster), so plain fake facets suffice — no AppController, no ImGui.
// Dispatch goes through the real CommandRegistry so the destructive-confirm gate,
// required-arg validation, and alias resolution run for real. Characterization tests —
// they pin CURRENT behaviour.

#include "../support/TestEnvGuard.h"

// Src-internal on purpose: the per-category registrars (RegisterTicketsCommands, …) are
// declared next to their only production consumers; this harness is the test-side consumer.
#include "../../Source/Core/src/Commands/Builtin/BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/IMainThreadPoster.h"
#include "Sync/SyncSessionStats.h"

#include "ConfigManager.h"
#include "ITrackerCollaboration.h" // TrackerIssueVotes — FakeUsers::FetchIssueVotes returns Result<TrackerIssueVotes>
#include "Interfaces/IAppAttachments.h"
#include "Interfaces/IAppAutomation.h"
#include "Interfaces/IAppFields.h"
#include "Interfaces/IAppMeta.h"
#include "Interfaces/IAppOfflineQueue.h"
#include "Interfaces/IAppSync.h"
#include "Interfaces/IAppTicketData.h"
#include "Interfaces/IAppTicketMutations.h"
#include "Interfaces/IAppUsers.h"
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "Sync/OfflineQueueTypes.h"
#include "Tracker/TrackerFieldSchema.h"
#include "VsyncControl.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using smatchet::cmd::CommandContext;
using smatchet::cmd::CommandRegistry;
using smatchet::cmd::CommandResult;
using smatchet::cmd::CommandSource;
using smatchet::cmd::ErrorCode;

namespace {

std::shared_ptr<const std::vector<CachedTicket>> MakeSnapshot() {
    auto tickets = std::make_shared<std::vector<CachedTicket>>();
    CachedTicket a;
    a.id = "PROJ-1";
    a.fieldValues["summary"] = "First ticket";
    a.fieldValues["status"] = "Open";
    tickets->push_back(a);
    CachedTicket b;
    b.id = "PROJ-2";
    b.fieldValues["summary"] = "Second one";
    tickets->push_back(b);
    return tickets;
}

struct FakeTicketData : IAppTicketData {
    std::shared_ptr<const std::vector<CachedTicket>> Snapshot = MakeSnapshot();
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override { return Snapshot; }
};

struct FakeUsers : IAppUsers {
    bool Fail = false;
    Result<std::vector<TrackerUser>> FetchIssueWatchers(const std::string&) const override {
        if (Fail) {
            return Result<std::vector<TrackerUser>>::Err("watchers boom");
        }
        TrackerUser u;
        u.AccountId = "w1";
        u.DisplayName = "Watcher One";
        std::vector<TrackerUser> out;
        out.push_back(u);
        return Result<std::vector<TrackerUser>>::Ok(std::move(out));
    }
    Result<TrackerIssueVotes> FetchIssueVotes(const std::string&) const override {
        if (Fail) {
            return Result<TrackerIssueVotes>::Err("votes boom");
        }
        TrackerUser u;
        u.AccountId = "v1";
        u.DisplayName = "Voter One";
        TrackerIssueVotes votes;
        votes.Voters.push_back(u);
        votes.VoteCount = 3;
        return Result<TrackerIssueVotes>::Ok(std::move(votes));
    }
    Result<std::vector<TrackerUser>> FetchUsersByAccountIds(const std::vector<std::string>& ids) const override {
        if (Fail) {
            return Result<std::vector<TrackerUser>>::Err("bulk boom");
        }
        std::vector<TrackerUser> out;
        for (const auto& id : ids) {
            TrackerUser u;
            u.AccountId = id;
            u.DisplayName = "Resolved " + id;
            out.push_back(std::move(u));
        }
        return Result<std::vector<TrackerUser>>::Ok(std::move(out));
    }
    Result<std::vector<TrackerUser>> SearchUsersByQuery(const std::string&) const override {
        if (Fail) {
            return Result<std::vector<TrackerUser>>::Err("search boom");
        }
        TrackerUser u;
        u.AccountId = "s1";
        u.DisplayName = "Search Hit";
        u.EmailAddress = "hit@example.com";
        std::vector<TrackerUser> out;
        out.push_back(u);
        return Result<std::vector<TrackerUser>>::Ok(std::move(out));
    }
};

struct FakeSync : IAppSync {
    int SyncCalls = 0;
    int RecreateCalls = 0;
    int RefreshCalls = 0;
    int RefreshAsyncCalls = 0;
    std::string RecreateError;
    TrackerConnectivityState State = TrackerConnectivityState::AuthenticatedReachable;
    std::string SyncWarning = "stale view";
    std::string CatalogError;
    TrackerIssueFetchPack Pack;

    int FullSyncCalls = 0;
    void SyncWithBackend(const TrackerConfig*, const ViewsStore*) override { ++SyncCalls; }
    void SyncWithBackendMode(const TrackerConfig*, const ViewsStore*, smatchet::sync::SyncMode mode) override {
        ++SyncCalls;
        if (mode == smatchet::sync::SyncMode::Full) {
            ++FullSyncCalls;
        }
    }
    bool RecreateWipes = false; // a failing recreate that already deleted the cache files
    // The recreate finishes inside the call (a real one finishes later, on the UI thread); `RecreateRefusal` models a
    // refusal to start, `HoldRecreate` leaves it running so a test can finish it by hand.
    std::string RecreateRefusal;
    bool HoldRecreate = false;
    std::atomic<bool> Recreating{false};
    std::function<void(const smatchet::sync::CacheRecreateOutcome&)> HeldDone;
    void FinishRecreate() {
        smatchet::sync::CacheRecreateOutcome outcome;
        outcome.Ok = RecreateError.empty();
        outcome.Wiped = RecreateError.empty() || RecreateWipes;
        outcome.Error = RecreateError;
        Recreating = false;
        if (outcome.Ok) {
            // The contract of IAppSync::StartRecreateLocalCacheDatabase: the recreate itself starts the follow-up full
            // sync (AppController::FinishCacheRecreate_), so the command's completion must not.
            SyncWithBackendMode(nullptr, nullptr, smatchet::sync::SyncMode::Full);
        }
        auto done = std::move(HeldDone);
        if (done) {
            done(outcome);
        }
    }
    VoidResult
    StartRecreateLocalCacheDatabase(std::function<void(const smatchet::sync::CacheRecreateOutcome&)> onDone) override {
        ++RecreateCalls;
        if (!RecreateRefusal.empty()) {
            return VoidResult::Err(RecreateRefusal);
        }
        HeldDone = std::move(onDone);
        Recreating = true; // after HeldDone: a thread that sees it may finish the recreate
        if (!HoldRecreate) {
            FinishRecreate();
        }
        return VoidOk();
    }
    bool IsLocalCacheRecreating() const override { return Recreating.load(); }
    std::string GetResolvedLocalCacheDbPath() const override { return "/tmp/fake-cache.db"; }
    void RefreshLocalData() override { ++RefreshCalls; }
    void RefreshLocalDataAsync() override { ++RefreshAsyncCalls; }
    TrackerIssueFetchPack FetchIssuesForActiveView(const TrackerConfig*, const ViewsStore*) override { return Pack; }
    TrackerConnectivityState GetLastTrackerConnectivityState() const override { return State; }
    const std::string& GetLastTicketSyncWarning() const override { return SyncWarning; }
    const std::string& GetFieldCatalogError() const override { return CatalogError; }
    smatchet::sync::SyncSessionStats GetLastSyncSessionStats() const override { return Stats; }

    smatchet::sync::SyncSessionStats Stats;
};

struct FakeOfflineQueue : IAppOfflineQueue {
    int CreateTicks = 0;
    int EditTicks = 0;
    std::vector<std::int64_t> DeletedCreateIds;
    std::vector<std::int64_t> DeletedEditIds;

    void TickOfflineCreates() override { ++CreateTicks; }
    void TickOfflineFieldEdits() override { ++EditTicks; }
    std::size_t GetPendingCreateCount() const override { return 1; }
    std::vector<PendingCreate> GetPendingCreates() const override {
        PendingCreate pc;
        pc.Id = 11;
        pc.Attempts = 2;
        pc.Payload = "{\"summary\":\"queued\"}";
        return {pc};
    }
    std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const override {
        PendingFieldEditRecord fe;
        fe.Id = 21;
        fe.IssueKey = "PROJ-9";
        fe.FieldId = "status";
        fe.Attempts = 1;
        return {fe};
    }
    std::vector<DeadPendingCreate> GetDeadPendingCreates() const override {
        DeadPendingCreate d;
        d.DeadId = 31;
        return {d};
    }
    std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const override {
        DeadPendingFieldEdit d;
        d.DeadId = 41;
        return {d};
    }
    DeadLetterDeleteSummary DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds) override {
        DeletedCreateIds = deadIds;
        DeadLetterDeleteSummary s;
        s.Deleted = static_cast<int>(deadIds.size());
        return s;
    }
    DeadFieldEditDeleteSummary DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) override {
        DeletedEditIds = deadIds;
        DeadFieldEditDeleteSummary s;
        s.Deleted = static_cast<int>(deadIds.size());
        return s;
    }
};

struct FakeMeta : IAppMeta {
    mutable int QuitRequests = 0;
    mutable std::string OpenedUrl;
    std::string GetAppVersion() const override { return "9.9.9-test"; }
    std::string GetGitHubReleaseRepo() const override { return "owner/smatchet"; }
    void RequestAppQuit() const override { ++QuitRequests; }
    void OpenUrl(const std::string& url) const override { OpenedUrl = url; }
    AppUpdateInfo CheckForAppUpdate(bool) const override {
        AppUpdateInfo info;
        info.Ok = true;
        info.UpdateAvailable = true;
        info.CurrentVersion = "9.9.9";
        info.LatestVersion = "10.0.0";
        info.ReleaseUrl = "https://example.invalid/rel";
        return info;
    }
};

struct FakeAttachments : IAppAttachments {
    std::string OpenedUrl;
    std::string OpenedFilename;
    std::string OpenedMime;
    bool DownloadOk = true;
    void OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType) override {
        OpenedUrl = url;
        OpenedFilename = filename;
        OpenedMime = mimeType;
    }
    bool DownloadAttachmentForPreview(const std::string&, const std::string&, const std::string&,
                                      std::string* outError) override {
        if (!DownloadOk && outError) {
            *outError = "disk full";
        }
        return DownloadOk;
    }
};

struct FakeMutations : IAppTicketMutations {
    TrackerField StatusField;
    PendingActionSubmitResult::Kind EditOutcome = PendingActionSubmitResult::Kind::Sent;
    PendingActionSubmitResult::Kind CommentOutcome = PendingActionSubmitResult::Kind::Sent;
    PendingActionSubmitResult::Kind WorklogOutcome = PendingActionSubmitResult::Kind::Sent;
    std::int64_t QueuedId = 77;
    bool CreateOk = true;
    std::string LastEditIssue;
    std::vector<std::string> LastEditValues;
    std::string LastWorklogTimeSpent;
    std::shared_ptr<const std::vector<CachedTicket>> Snapshot = MakeSnapshot();

    FakeMutations() {
        StatusField.Id = "status";
        StatusField.Name = "Status";
    }
    const TrackerField* FindFieldById(const std::string& fieldId) const override {
        return fieldId == "status" ? &StatusField : nullptr;
    }
    static PendingActionSubmitResult Outcome(PendingActionSubmitResult::Kind kind, const char* rejection) {
        PendingActionSubmitResult r;
        r.K = kind;
        r.QueueId = kind == PendingActionSubmitResult::Kind::Queued ? 42 : 0;
        r.Error = kind == PendingActionSubmitResult::Kind::Failed ? rejection : "";
        return r;
    }
    PendingActionTarget LatchPendingActionTarget() const override { return PendingActionTarget(); }
    PendingActionSubmitResult SubmitFieldEditOrQueue(const PendingActionTarget&, const std::string& issueId,
                                                     const TrackerField&,
                                                     const std::vector<std::string>& values) override {
        LastEditIssue = issueId;
        LastEditValues = values;
        return Outcome(EditOutcome, "edit rejected");
    }
    PendingActionSubmitResult SubmitOrQueueComment(const PendingActionTarget&, const std::string&,
                                                   const std::string&) override {
        return Outcome(CommentOutcome, "comment rejected");
    }
    PendingActionSubmitResult SubmitOrQueueWorklog(const PendingActionTarget&, const std::string&,
                                                   const std::string& timeSpent, const std::string&, const std::string&,
                                                   const std::string&, const std::string&) override {
        LastWorklogTimeSpent = timeSpent;
        return Outcome(WorklogOutcome, "worklog rejected");
    }
    std::int64_t QueueCreateOffline(const IssueDraft&) override { return QueuedId; }
    std::int64_t QueueCreateOffline(const IssueDraft&, std::string& outError) override {
        outError = QueueRefusal;
        return QueuedId;
    }
    std::string QueueRefusal; // why a queue-offline create returned 0 (the recreating cache), empty for no reason
    std::future<IssueCreateResult> CreateIssueAsync(const IssueDraft& draft, smatchet::ui::CancelToken) override {
        std::promise<IssueCreateResult> p;
        IssueCreateResult r;
        r.Ok = CreateOk;
        r.IssueKey = CreateOk ? draft.ProjectKey + "-100" : std::string();
        r.Error = CreateOk ? std::string() : "backend down";
        p.set_value(r);
        return p.get_future();
    }
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override { return Snapshot; }
};

struct FakeAutomation : IAppAutomation {
    std::string LastScript;
    std::vector<std::string> LastIds;
    bool LastProcessAll = false;
    std::string LastFlatScript;
    std::string LastSetupScript;
    void RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds,
                       bool processAll) override {
        LastScript = scriptPath;
        LastIds = selectedIds;
        LastProcessAll = processAll;
    }
    void RunFlatScriptAsync(const std::string& scriptPath) override { LastFlatScript = scriptPath; }
    void RunLuaSetupScript(const std::string& scriptPath) override { LastSetupScript = scriptPath; }
    std::vector<std::string> GetLuaGlobalActionNames() const override { return {"alpha", "beta"}; }
    VoidResult ApproveLuaScript(const std::string& scriptName) override {
        LastApprovedScript = scriptName;
        return VoidOk();
    }
    VoidResult RevokeLuaScript(const std::string& scriptName) override {
        LastRevokedScript = scriptName;
        return VoidOk();
    }
    std::vector<std::string> ListApprovedLuaScriptPaths() const override { return ApprovedPaths; }
    std::string LastApprovedScript;
    std::string LastRevokedScript;
    std::vector<std::string> ApprovedPaths;
};

struct FakeFields : IAppFields {
    std::vector<TrackerField> Fields;
    std::string CatalogError;
    bool RefreshOk = true;
    int RefreshCalls = 0;
    bool IconFound = false;
    std::string IconTarget;
    mutable std::string LastIconFieldId;
    mutable std::string LastIconValue;

    FakeFields() {
        TrackerField prio;
        prio.Id = "priority";
        prio.Name = "Priority";
        prio.Type = "priority";
        TrackerField summary;
        summary.Id = "summary";
        summary.Name = "Summary";
        summary.Type = "string";
        Fields = {prio, summary};
    }
    const std::vector<TrackerField>& GetAvailableFields() const override { return Fields; }
    const TrackerField* FindFieldById(const std::string& fieldId) const override {
        for (const TrackerField& f : Fields) {
            if (f.Id == fieldId) {
                return &f;
            }
        }
        return nullptr;
    }
    bool RefreshFieldCatalog(const TrackerConfig&) override {
        ++RefreshCalls;
        return RefreshOk;
    }
    const std::string& GetFieldCatalogError() const override { return CatalogError; }
    bool TryGetFieldIconMapTarget(const std::string& fieldId, const TrackerField*, const std::string& rawValue,
                                  std::string& outPathOrUrl) const override {
        LastIconFieldId = fieldId;
        LastIconValue = rawValue;
        if (IconFound) {
            outPathOrUrl = IconTarget;
        }
        return IconFound;
    }
};

// The doctest rig is single-threaded, so "already on the UI thread" is the truthful
// answer and RunOnUiThreadAsCommandResult runs its lambda inline — no dispatcher needed.
struct InlinePoster : IMainThreadPoster {
    bool IsOnUiThread() const override { return true; }
    void PostToMainThread(std::function<void()> fn) override { fn(); }
};

// A poster whose "am I the UI thread" answer can be flipped. The sync.* handlers hop through it
// (RunOnUiThread posts and waits, so running the closure inline satisfies the wait), and
// sync.fetch_active_view refuses to run when the caller IS the UI thread.
struct TogglePoster : IMainThreadPoster {
    bool OnUi = false;
    int Posts = 0;
    std::atomic<bool> Accepting{true}; // false models a host that has begun shutting down
    bool IsOnUiThread() const override { return OnUi; }
    bool AcceptsPosts() const override { return Accepting.load(); }
    void PostToMainThread(std::function<void()> fn) override {
        ++Posts;
        fn();
    }
};

// The one-shot in-process CLI: the command runs on the UI thread, but no frame loop ever drains the posted tasks,
// so the command has to pump the queue itself. `OnPump` models what the dispatcher would run (the recreate's
// completion); left empty it models a queue that never produces the outcome.
struct NoFrameLoopPoster : IMainThreadPoster {
    int Pumps = 0;
    std::function<void()> OnPump;
    bool IsOnUiThread() const override { return true; }
    void PostToMainThread(std::function<void()> fn) override { fn(); }
    bool HasFrameLoop() const override { return false; }
    void DrainMainThreadOnce() override {
        ++Pumps;
        if (OnPump) {
            OnPump();
        }
    }
};

CommandContext AutomationCtx(bool confirmed = false, bool dryRun = false) {
    CommandContext ctx;
    ctx.Source = CommandSource::Cli; // automation source — the destructive gate is armed
    ctx.ConfirmedDestructive = confirmed;
    ctx.DryRun = dryRun;
    return ctx;
}

} // namespace

TEST_CASE("tickets.* — snapshot projections, search, get, exists, legacy alias") {
    FakeTicketData data;
    CommandRegistry reg;
    smatchet::cmd::RegisterTicketsCommands(reg, data);
    const CommandContext ctx = AutomationCtx();

    {
        const CommandResult r = reg.Dispatch("tickets.list_active", {}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["total"] == 2);
        CHECK((*r.Data)["items"][0]["id"] == "PROJ-1");
        CHECK((*r.Data)["items"][0]["status"] == "Open");
        CHECK((*r.Data)["items"][1] == nlohmann::json({{"id", "PROJ-2"}, {"summary", "Second one"}}));
    }
    {
        // Legacy MCP tool name routes to the same handler.
        const CommandResult r = reg.Dispatch("list_active_tickets", {}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["total"] == 2);
    }
    {
        const CommandResult r = reg.Dispatch("tickets.search_active", {{"query", "second"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["total"] == 1);
        CHECK((*r.Data)["items"][0]["id"] == "PROJ-2");
    }
    {
        const CommandResult r = reg.Dispatch("tickets.get", {{"id", "PROJ-1"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["fields"]["summary"] == "First ticket");
    }
    {
        const CommandResult r = reg.Dispatch("tickets.get", {{"id", "PROJ-404"}}, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::NotFound);
    }
    {
        const CommandResult r = reg.Dispatch("tickets.exists", {{"id", "PROJ-2"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["exists"] == true);
    }
}

TEST_CASE("users.* — backend results propagate; a failed fetch is BackendError, not an empty ok") {
    FakeUsers users;
    CommandRegistry reg;
    smatchet::cmd::RegisterUsersCommands(reg, users);
    const CommandContext ctx = AutomationCtx();

    {
        const CommandResult r = reg.Dispatch("users.search", {{"query", "hit"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["items"][0]["email"] == "hit@example.com");
    }
    {
        const CommandResult r = reg.Dispatch("users.watchers", {{"ticketId", "PROJ-1"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["items"][0]["displayName"] == "Watcher One");
    }
    {
        const CommandResult r = reg.Dispatch("users.votes", {{"ticketId", "PROJ-1"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["voteCount"] == 3);
        CHECK((*r.Data)["voters"][0]["id"] == "v1");
    }
    users.Fail = true;
    for (const auto& call :
         std::vector<std::pair<std::string, nlohmann::json>>{{"users.search", {{"query", "hit"}}},
                                                             {"users.watchers", {{"ticketId", "PROJ-1"}}},
                                                             {"users.votes", {{"ticketId", "PROJ-1"}}}}) {
        const CommandResult r = reg.Dispatch(call.first, call.second, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::BackendError);
        CHECK(r.Error.Message.find("boom") != std::string::npos);
    }
}

TEST_CASE("cache.recreate words a failure by whether the cache files were already wiped") {
    FakeSync sync;
    TogglePoster poster;
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    sync.RecreateError = "Failed to reopen database: disk full";
    sync.RecreateWipes = false; // nothing was deleted
    const CommandResult untouched = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
    REQUIRE_FALSE(untouched.Ok);
    CHECK(untouched.Error.Message.find("was not recreated") != std::string::npos);
    CHECK(untouched.Error.Message.find("was wiped") == std::string::npos);

    sync.RecreateWipes = true; // the files are gone, the new database could not be set up
    const CommandResult wiped = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
    REQUIRE_FALSE(wiped.Ok);
    CHECK(wiped.Error.Code == ErrorCode::HandlerError);
    CHECK(wiped.Error.Message.find("was wiped") != std::string::npos);
    CHECK(wiped.Error.Message.find("disk full") != std::string::npos);
    CHECK(wiped.Error.Message.find("was not recreated") == std::string::npos);
    CHECK(sync.FullSyncCalls == 0); // no sync starts after a failure either way
}

TEST_CASE(
    "cache.recreate from the UI thread starts the recreate and returns at once; the sync follows its completion") {
    FakeSync sync;
    sync.HoldRecreate = true; // the real recreate finishes later, on the UI thread
    TogglePoster poster;
    poster.OnUi = true; // the command palette: this thread cannot wait for the outcome
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
    REQUIRE(r.Ok);
    CHECK((*r.Data)["started"] == true);
    CHECK((*r.Data)["completed"] == false);
    CHECK(sync.RecreateCalls == 1);
    CHECK(sync.IsLocalCacheRecreating());
    CHECK(sync.FullSyncCalls == 0); // nothing is re-fetched before the new database is in place

    sync.FinishRecreate();
    CHECK_FALSE(sync.IsLocalCacheRecreating());
    CHECK(sync.FullSyncCalls == 1);
}

TEST_CASE("cache.recreate from another thread waits for the outcome and reports whether the wipe happened") {
    FakeSync sync;
    sync.HoldRecreate = true;
    TogglePoster poster; // not on the UI thread: an MCP / CLI / Lua caller
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    CommandResult result;
    std::thread caller([&]() { result = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!sync.IsLocalCacheRecreating() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(sync.IsLocalCacheRecreating());
    sync.RecreateError = "Failed to open new database: disk full";
    sync.RecreateWipes = true; // the files are gone, the new database could not be set up
    sync.FinishRecreate();     // the UI thread's completion
    caller.join();

    REQUIRE_FALSE(result.Ok);
    CHECK(result.Error.Code == ErrorCode::HandlerError);
    CHECK(result.Error.Message.find("was wiped") != std::string::npos);
    CHECK(sync.FullSyncCalls == 0);
}

TEST_CASE("cache.recreate from another thread stops waiting once the host begins shutting down (#2306)") {
    // The outcome is delivered by a UI-thread post, which is a no-op after BeginShutdown; the MCP worker must not
    // hold the host's pool join for the whole 5-minute bound.
    FakeSync sync;
    sync.HoldRecreate = true;
    TogglePoster poster;
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    CommandResult result;
    std::thread caller([&]() { result = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!sync.IsLocalCacheRecreating() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(sync.IsLocalCacheRecreating());
    const auto shutdownAt = std::chrono::steady_clock::now();
    poster.Accepting.store(false);
    caller.join();
    const auto waitedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shutdownAt).count();

    REQUIRE_FALSE(result.Ok);
    CHECK(result.Error.Code == ErrorCode::HandlerError);
    CHECK(result.Error.Message.find("shutting down") != std::string::npos);
    CHECK(waitedMs < 5000); // one 50 ms wait slice, with room for sanitizers
    sync.FinishRecreate();  // the late completion has no waiter left and must not throw
}

TEST_CASE("cache.recreate with no frame loop pumps the UI-thread queue until the outcome arrives") {
    // Smatchet.exe cmd cache.recreate --yes without MCP: the command runs on the thread that booted the app, nothing
    // drains the dispatcher, and the process exits right after. Returning "started" there left the recreate undone.
    FakeSync sync;
    sync.HoldRecreate = true;
    NoFrameLoopPoster poster;
    poster.OnPump = [&]() {
        if (poster.Pumps == 3) {
            sync.FinishRecreate(); // the dispatcher ran the worker's completion on the third pump
        }
    };
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
    REQUIRE(r.Ok);
    CHECK((*r.Data)["completed"] == true);
    CHECK((*r.Data)["wiped"] == true);
    CHECK(poster.Pumps == 3);
    CHECK(sync.RecreateCalls == 1);
    CHECK_FALSE(sync.IsLocalCacheRecreating());
    CHECK(sync.FullSyncCalls == 1); // the follow-up re-fetch started when the new database was ready
}

TEST_CASE("cache.recreate with no frame loop reports a failed recreate by whether the cache was wiped") {
    FakeSync sync;
    sync.HoldRecreate = true;
    sync.RecreateError = "Failed to open new database: disk full";
    sync.RecreateWipes = true;
    NoFrameLoopPoster poster;
    poster.OnPump = [&]() { sync.FinishRecreate(); };
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
    REQUIRE_FALSE(r.Ok);
    CHECK(r.Error.Message.find("was wiped") != std::string::npos);
    CHECK(sync.FullSyncCalls == 0);
}

TEST_CASE("cache.recreate with no frame loop fails, rather than claiming it started, when the queue never delivers") {
    // A poster that never drains to an outcome: the old code returned {started: true} with exit 0 and nothing wiped.
    FakeSync sync;
    sync.HoldRecreate = true; // the recreate never finishes
    NoFrameLoopPoster poster;
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);
    CommandContext ctx = AutomationCtx(/*confirmed*/ true);
    ctx.TimeoutMs = 60; // bound the wait

    const CommandResult r = reg.Dispatch("cache.recreate", {}, ctx);
    REQUIRE_FALSE(r.Ok);
    CHECK(r.Error.Code == ErrorCode::HandlerError);
    CHECK(r.Error.Message.find("did not finish") != std::string::npos);
    CHECK(r.Error.Message.find("no frame loop") != std::string::npos);
    CHECK(poster.Pumps >= 1); // it did pump while it waited
    CHECK(sync.RecreateCalls == 1);
    CHECK(sync.FullSyncCalls == 0);
}

TEST_CASE("cache.recreate reports a refusal to start as nothing changed") {
    FakeSync sync;
    sync.RecreateRefusal = "The local cache is already being recreated.";
    TogglePoster poster;
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
    REQUIRE_FALSE(r.Ok);
    CHECK(r.Error.Message.find("already being recreated") != std::string::npos);
    CHECK(r.Error.Message.find("was not recreated") != std::string::npos);
    CHECK(sync.FullSyncCalls == 0);
}

TEST_CASE(
    "sync.* / cache.recreate — trigger plumbing, destructive gate on cache.recreate, status mapping, fetch pack") {
    FakeSync sync;
    TogglePoster poster;
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync, poster);

    {
        const CommandResult r = reg.Dispatch("sync.incremental", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["triggered"] == true);
        CHECK(sync.SyncCalls == 1);
        CHECK(poster.Posts == 1); // hopped to the UI thread instead of running on the caller's thread
    }
    {
        // sync.full no longer wipes anything, so it needs no confirmation: it forces a Full re-download.
        const CommandResult r = reg.Dispatch("sync.full", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["triggered"] == true);
        CHECK(sync.RecreateCalls == 0);
        CHECK(sync.FullSyncCalls == 1);
        CHECK(sync.SyncCalls == 2);
        CHECK(poster.Posts == 2);
    }
    {
        const CommandResult r = reg.Dispatch("sync.full", {}, AutomationCtx(false, /*dryRun*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data).contains("wouldDo"));
        CHECK(sync.FullSyncCalls == 1);
    }
    {
        // cache.recreate is the destructive wipe: an unconfirmed automation source is stopped by the registry
        // BEFORE the handler runs ...
        const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx());
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ConfirmRequired);
        CHECK(sync.RecreateCalls == 0);
    }
    {
        const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx(false, /*dryRun*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data).contains("wouldDo"));
        CHECK(sync.RecreateCalls == 0);
    }
    {
        // ... a confirmed one whose wipe fails reports the failure and starts NO sync ...
        sync.RecreateError = "cache locked";
        const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::HandlerError);
        CHECK(r.Error.Message.find("cache locked") != std::string::npos);
        CHECK(sync.RecreateCalls == 1);
        CHECK(sync.FullSyncCalls == 1);
        CHECK(sync.SyncCalls == 2);
    }
    {
        // ... and a confirmed one that wipes then re-fetches in full.
        sync.RecreateError.clear();
        const CommandResult r = reg.Dispatch("cache.recreate", {}, AutomationCtx(/*confirmed*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data)["triggered"] == true);
        CHECK_FALSE((*r.Data).contains("warning"));
        CHECK(sync.RecreateCalls == 2);
        CHECK(sync.FullSyncCalls == 2);
        CHECK(sync.SyncCalls == 3);
    }
    {
        const CommandResult r = reg.Dispatch("sync.refresh_local", {}, AutomationCtx());
        REQUIRE(r.Ok);
        // The UI-thread hop only resolves the pane; the SQLite read must not run inline.
        CHECK(sync.RefreshCalls == 0);
        CHECK(sync.RefreshAsyncCalls == 1);
        CHECK(poster.Posts == 5);
    }
    {
        const CommandResult r = reg.Dispatch("sync.tracker_status", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["state"] == "authenticated-reachable");
        CHECK((*r.Data)["syncWarning"] == "stale view");
        sync.State = TrackerConnectivityState::TransportDown;
        const CommandResult r2 = reg.Dispatch("sync.tracker_status", {}, AutomationCtx());
        REQUIRE(r2.Ok);
        CHECK((*r2.Data)["state"] == "transport-down");
    }
    {
        CachedTicket t;
        t.id = "PROJ-5";
        t.fieldValues["summary"] = "fetched";
        sync.Pack.Tickets.push_back(t);
        sync.Pack.FetchError = "partial page";
        // On the UI thread the blocking fetch is refused (Pillar 2) ...
        poster.OnUi = true;
        const CommandResult refused = reg.Dispatch("sync.fetch_active_view", {}, AutomationCtx());
        REQUIRE_FALSE(refused.Ok);
        CHECK(refused.Error.Code == ErrorCode::HandlerError);
        // ... and runs normally from a CLI / MCP thread.
        poster.OnUi = false;
        const CommandResult r = reg.Dispatch("sync.fetch_active_view", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["tickets"][0]["summary"] == "fetched");
        CHECK((*r.Data)["error"] == "partial page");
    }
    {
        sync.Stats.Valid = true;
        sync.Stats.Mode = "delta";
        sync.Stats.RosterSize = 120;
        sync.Stats.BodiesFetched = 3;
        sync.Stats.Unchanged = 117;
        sync.Stats.FallbackReason = "roster error";
        const CommandResult r = reg.Dispatch("sync.stats", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["valid"] == true);
        CHECK((*r.Data)["mode"] == "delta");
        CHECK((*r.Data)["rosterSize"] == 120);
        CHECK((*r.Data)["bodiesFetched"] == 3);
        CHECK((*r.Data)["unchanged"] == 117);
        CHECK((*r.Data)["fallbackReason"] == "roster error");
    }
}

TEST_CASE("offline.* — queue projections, replay ticks, dead-letter prune passes exact ids") {
    FakeOfflineQueue q;
    CommandRegistry reg;
    smatchet::cmd::RegisterOfflineCommands(reg, q);

    {
        const CommandResult r = reg.Dispatch("offline.list_pending", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["creates"]["items"][0]["id"] == 11);
        CHECK((*r.Data)["fieldEdits"]["items"][0]["ticketId"] == "PROJ-9");
    }
    {
        const CommandResult r = reg.Dispatch("offline.replay_now", {}, AutomationCtx(false, /*dryRun*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data)["wouldDo"]["pendingCreates"] == 1);
        CHECK(q.CreateTicks == 0);
    }
    {
        const CommandResult r = reg.Dispatch("offline.replay_now", {}, AutomationCtx(/*confirmed*/ true));
        REQUIRE(r.Ok);
        CHECK(q.CreateTicks == 1);
        CHECK(q.EditTicks == 1);
    }
    {
        const CommandResult r = reg.Dispatch("offline.prune_dead", {}, AutomationCtx(/*confirmed*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data)["deletedCreates"] == 1);
        CHECK((*r.Data)["deletedFieldEdits"] == 1);
        CHECK(q.DeletedCreateIds == std::vector<std::int64_t>{31});
        CHECK(q.DeletedEditIds == std::vector<std::int64_t>{41});
    }
}

TEST_CASE("app.* — version, gated quit, update check, read-only persist round-trip") {
    smatchet_tests::TestEnvGuard env; // app.set_readonly writes through the real ConfigManager
    FakeMeta meta;
    CommandRegistry reg;
    smatchet::cmd::RegisterAppCommands(reg, meta);

    {
        const CommandResult r = reg.Dispatch("app.version", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["version"] == "9.9.9-test");
        CHECK((*r.Data)["releaseRepo"] == "owner/smatchet");
        // build.agentDebug must be emitted UNCONDITIONALLY. It is the machine
        // surface that separates "no instrumented call site fired" from
        // "SMATCHET_AGENT_DEBUG never reached the compiler", so an absent key
        // restores exactly the ambiguity it exists to remove — and absence is
        // indistinguishable from false to a reader doing `.get<bool>()` on a
        // missing key. Presence + type is the property under test; whether the
        // value is true or false for a given target is pinned separately by
        // tests/Core/AboutInfo.test.cpp against GatherAboutInfo.
        REQUIRE(r.Data->contains("build"));
        const auto& build = (*r.Data)["build"];
        CHECK(build.contains("agentDebug"));
        CHECK(build["agentDebug"].is_boolean());
    }
    {
        const CommandResult r = reg.Dispatch("app.quit", {}, AutomationCtx());
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ConfirmRequired);
        CHECK(meta.QuitRequests == 0);
    }
    {
        const CommandResult r = reg.Dispatch("app.quit", {}, AutomationCtx(/*confirmed*/ true));
        REQUIRE(r.Ok);
        CHECK(meta.QuitRequests == 1);
    }
    {
        const CommandResult r = reg.Dispatch("app.check_updates", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["updateAvailable"] == true);
        CHECK((*r.Data)["latestVersion"] == "10.0.0");
    }
    {
        const CommandResult dry =
            reg.Dispatch("app.set_readonly", {{"on", true}}, AutomationCtx(false, /*dryRun*/ true));
        REQUIRE(dry.Ok);
        CHECK((*dry.Data)["wouldDo"]["readOnlyMode"] == true);
        CHECK_FALSE(ConfigManager::Load().ReadOnlyMode); // dry-run must not persist

        const CommandResult r = reg.Dispatch("app.set_readonly", {{"on", true}}, AutomationCtx(/*confirmed*/ true));
        REQUIRE(r.Ok);
        CHECK(ConfigManager::Load().ReadOnlyMode);
    }
}

TEST_CASE("attach.* — open forwards the triple; failed preview download is HandlerError") {
    FakeAttachments att;
    CommandRegistry reg;
    smatchet::cmd::RegisterAttachCommands(reg, att);
    const nlohmann::json args = {{"url", "https://t.example/a.png"}, {"filename", "a.png"}, {"mimeType", "image/png"}};

    {
        const CommandResult r = reg.Dispatch("attach.open", args, AutomationCtx(/*confirmed*/ true));
        REQUIRE(r.Ok);
        CHECK(att.OpenedUrl == "https://t.example/a.png");
        CHECK(att.OpenedFilename == "a.png");
        CHECK(att.OpenedMime == "image/png");
    }
    {
        const CommandResult r = reg.Dispatch("attach.download_preview", args, AutomationCtx());
        REQUIRE(r.Ok);
        att.DownloadOk = false;
        const CommandResult fail = reg.Dispatch("attach.download_preview", args, AutomationCtx());
        REQUIRE_FALSE(fail.Ok);
        CHECK(fail.Error.Code == ErrorCode::HandlerError);
        CHECK(fail.Error.Message.find("disk full") != std::string::npos);
    }
}

TEST_CASE("ticket.* mutations — catalog lookup, dry-run diff, worklog formatting, create paths") {
    FakeMutations mut;
    CommandRegistry reg;
    smatchet::cmd::RegisterTicketMutationCommands(reg, mut);
    const CommandContext yes = AutomationCtx(/*confirmed*/ true);

    {
        const nlohmann::json args = {{"id", "PROJ-1"}, {"field", "status"}, {"value", "Done"}};
        const CommandResult r = reg.Dispatch("ticket.set_field", args, yes);
        REQUIRE(r.Ok);
        CHECK(mut.LastEditIssue == "PROJ-1");
        CHECK(mut.LastEditValues == std::vector<std::string>{"Done"});
    }
    {
        // Dry-run reads the current value off the snapshot for the diff preview.
        const nlohmann::json args = {{"id", "PROJ-1"}, {"field", "status"}, {"value", "Done"}};
        const CommandResult r = reg.Dispatch("ticket.set_field", args, AutomationCtx(false, /*dryRun*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data)["wouldDo"]["from"] == "Open");
        CHECK((*r.Data)["wouldDo"]["to"] == "Done");
    }
    {
        const nlohmann::json args = {{"id", "PROJ-1"}, {"field", "nosuch"}, {"value", "x"}};
        const CommandResult r = reg.Dispatch("ticket.set_field", args, yes);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::NotFound);
    }
    {
        mut.EditOutcome = PendingActionSubmitResult::Kind::Failed;
        const nlohmann::json args = {{"id", "PROJ-1"}, {"field", "status"}, {"value", "Done"}};
        const CommandResult r = reg.Dispatch("ticket.set_field", args, yes);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::BackendError);
        CHECK(r.Error.Message.find("edit rejected") != std::string::npos);
        mut.EditOutcome = PendingActionSubmitResult::Kind::Sent;
    }
    {
        // Tracker unreachable: a field edit is saved to the offline queue and sent on reconnect —
        // a success whose envelope says so, for set_field, transition and each set_fields entry.
        mut.EditOutcome = PendingActionSubmitResult::Kind::Queued;
        const nlohmann::json args = {{"id", "PROJ-1"}, {"field", "status"}, {"value", "Done"}};
        const CommandResult s = reg.Dispatch("ticket.set_field", args, yes);
        REQUIRE(s.Ok);
        CHECK((*s.Data)["ok"] == true);
        CHECK((*s.Data)["queued"] == true);
        CHECK((*s.Data)["offlineId"] == 42);
        const CommandResult t = reg.Dispatch("ticket.transition", {{"id", "PROJ-1"}, {"toStatus", "Done"}}, yes);
        REQUIRE(t.Ok);
        CHECK((*t.Data)["queued"] == true);
        const nlohmann::json fields = {{"status", "Done"}};
        const CommandResult m = reg.Dispatch("ticket.set_fields", {{"id", "PROJ-1"}, {"fields", fields}}, yes);
        REQUIRE(m.Ok);
        CHECK((*m.Data)["results"]["status"]["ok"] == true);
        CHECK((*m.Data)["results"]["status"]["queued"] == true);
        CHECK((*m.Data)["results"]["status"]["error"] == "");
        mut.EditOutcome = PendingActionSubmitResult::Kind::Sent;
    }
    {
        const CommandResult r = reg.Dispatch("ticket.add_comment", {{"id", "PROJ-1"}, {"body", "hi"}}, yes);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["ok"] == true);
        CHECK_FALSE(r.Data->contains("queued"));
        mut.CommentOutcome = PendingActionSubmitResult::Kind::Failed;
        const CommandResult fail = reg.Dispatch("ticket.add_comment", {{"id", "PROJ-1"}, {"body", "hi"}}, yes);
        REQUIRE_FALSE(fail.Ok);
        CHECK(fail.Error.Code == ErrorCode::BackendError);
        CHECK(fail.Error.Message.find("comment rejected") != std::string::npos);
        mut.CommentOutcome = PendingActionSubmitResult::Kind::Sent;
    }
    {
        // Tracker unreachable: the write is saved to the pending-action queue and posts on reconnect.
        // Still a success, and the envelope says it was queued and which queue row holds it.
        mut.CommentOutcome = PendingActionSubmitResult::Kind::Queued;
        const CommandResult c = reg.Dispatch("ticket.add_comment", {{"id", "PROJ-1"}, {"body", "hi"}}, yes);
        REQUIRE(c.Ok);
        CHECK((*c.Data)["ok"] == true);
        CHECK((*c.Data)["queued"] == true);
        CHECK((*c.Data)["offlineId"] == 42);
        mut.WorklogOutcome = PendingActionSubmitResult::Kind::Queued;
        const CommandResult w = reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 60}}, yes);
        REQUIRE(w.Ok);
        CHECK((*w.Data)["queued"] == true);
        CHECK((*w.Data)["offlineId"] == 42);
        CHECK((*w.Data)["timeSpent"] == "1m");
        mut.WorklogOutcome = PendingActionSubmitResult::Kind::Failed;
        const CommandResult wf = reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 60}}, yes);
        REQUIRE_FALSE(wf.Ok);
        CHECK(wf.Error.Code == ErrorCode::BackendError);
        mut.CommentOutcome = PendingActionSubmitResult::Kind::Sent;
        mut.WorklogOutcome = PendingActionSubmitResult::Kind::Sent;
    }
    {
        // 5400 s -> "1h 30m"; 2700 s -> "45m".
        const CommandResult r = reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 5400}}, yes);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["timeSpent"] == "1h 30m");
        reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 2700}}, yes);
        CHECK(mut.LastWorklogTimeSpent == "45m");

        // #2054: the sub-minute remainder used to be dropped — 3630 s went to the tracker as "1h",
        // silently logging 30 s less than the caller asked for. Every non-zero component is emitted.
        reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 3630}}, yes);
        CHECK(mut.LastWorklogTimeSpent == "1h 30s");
        reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 3690}}, yes);
        CHECK(mut.LastWorklogTimeSpent == "1h 1m 30s");
        reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 90}}, yes);
        CHECK(mut.LastWorklogTimeSpent == "1m 30s");

        // #2054: zero / negative are rejected as an argument error instead of being submitted as
        // timeSpent="0s" / "-30s" and coming back as an opaque backend rejection. The fake records
        // the last timeSpent it saw, so an unchanged value proves SubmitOrQueueWorklog was never reached.
        mut.LastWorklogTimeSpent = "sentinel";
        const CommandResult zero = reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 0}}, yes);
        REQUIRE_FALSE(zero.Ok);
        CHECK(zero.Error.Code == ErrorCode::ValidationError);
        CHECK(mut.LastWorklogTimeSpent == "sentinel");

        const CommandResult neg = reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", -30}}, yes);
        REQUIRE_FALSE(neg.Ok);
        CHECK(neg.Error.Code == ErrorCode::ValidationError);
        CHECK(mut.LastWorklogTimeSpent == "sentinel");

        // An omitted `seconds` is caught earlier, by the registry's Required check.
        const CommandResult missing = reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}}, yes);
        REQUIRE_FALSE(missing.Ok);
        CHECK(missing.Error.Code == ErrorCode::MissingRequiredArg);
        CHECK(mut.LastWorklogTimeSpent == "sentinel");
    }
    {
        // Direct unit coverage of the shared formatter the handler delegates to (production symbol,
        // not a re-implementation) — including the boundaries the handler itself never reaches.
        using smatchet::cmd::builtin_detail::FormatWorklogTimeSpent;
        CHECK(FormatWorklogTimeSpent(3600) == "1h");
        CHECK(FormatWorklogTimeSpent(60) == "1m");
        CHECK(FormatWorklogTimeSpent(1) == "1s");
        CHECK(FormatWorklogTimeSpent(59) == "59s");
        CHECK(FormatWorklogTimeSpent(3661) == "1h 1m 1s");
        CHECK(FormatWorklogTimeSpent(7200) == "2h");
        // Non-positive is a programming error (the handler rejects it first) — never an empty or
        // negative timeSpent on the wire.
        CHECK(FormatWorklogTimeSpent(0) == "0s");
        CHECK(FormatWorklogTimeSpent(-30) == "0s");
    }
    {
        const CommandResult r = reg.Dispatch("ticket.transition", {{"id", "PROJ-1"}, {"toStatus", "Done"}}, yes);
        REQUIRE(r.Ok);
        CHECK(mut.LastEditValues == std::vector<std::string>{"Done"});
        CHECK_FALSE(r.Data->contains("queued"));
    }
    {
        const nlohmann::json fields = {{"status", "Done"}, {"nosuch", "x"}};
        const CommandResult r = reg.Dispatch("ticket.set_fields", {{"id", "PROJ-1"}, {"fields", fields}}, yes);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["results"]["status"]["ok"] == true);
        CHECK((*r.Data)["results"]["nosuch"]["ok"] == false);
    }
    {
        const nlohmann::json args = {{"projectKey", "PROJ"}, {"summary", "s"}, {"offline", true}};
        const CommandResult r = reg.Dispatch("ticket.create", args, yes);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["offlineId"] == 77);
        mut.QueuedId = 0;
        const CommandResult fail = reg.Dispatch("ticket.create", args, yes);
        CHECK(fail.Error.Code == ErrorCode::HandlerError);
        CHECK(fail.Error.Message == "Failed to queue offline create."); // no specific reason: the generic text
        mut.QueueRefusal = "The local database is being recreated, retry in a moment.";
        const CommandResult recreating = reg.Dispatch("ticket.create", args, yes);
        CHECK(recreating.Error.Code == ErrorCode::HandlerError);
        CHECK(recreating.Error.Message == mut.QueueRefusal); // the refusal reaches the caller
    }
    {
        const nlohmann::json args = {{"projectKey", "PROJ"}, {"summary", "s"}};
        const CommandResult r = reg.Dispatch("ticket.create", args, yes);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["issueKey"] == "PROJ-100");
        mut.CreateOk = false;
        const CommandResult fail = reg.Dispatch("ticket.create", args, yes);
        REQUIRE_FALSE(fail.Ok);
        CHECK(fail.Error.Code == ErrorCode::BackendError);
    }
}

TEST_CASE("automation.* — id extraction shapes, empty-script guard, inline reload, globals") {
    FakeAutomation autom;
    InlinePoster poster;
    CommandRegistry reg;
    smatchet::cmd::RegisterAutomationCommands(reg, autom, poster);
    const CommandContext ctx = AutomationCtx();

    {
        const nlohmann::json args = {{"script", "Auto.lua"}, {"ids", nlohmann::json::array({"A-1", 7})}};
        const CommandResult r = reg.Dispatch("automation.run-script", args, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["idCount"] == 2);
        CHECK(autom.LastIds == std::vector<std::string>{"A-1", "7"});
    }
    {
        // A bare (non-JSON) string for the Json-typed `ids` param is rejected by the
        // registry's coercion (ParseBounded of the string's CONTENT) before the handler runs.
        const nlohmann::json args = {{"script", "Auto.lua"}, {"ids", "B-1, B-2"}};
        const CommandResult r = reg.Dispatch("automation.run-script", args, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ValidationError);
    }
    {
        // The handler's comma/space-separated convenience shape is reachable with JSON-string
        // quoting (the CLI --ids '"B-1, B-2"' form): coercion yields a json string, which
        // ExtractIdsArray splits.
        const nlohmann::json args = {{"script", "Auto.lua"}, {"ids", "\"B-1, B-2\""}};
        const CommandResult r = reg.Dispatch("automation.run-script", args, ctx);
        REQUIRE(r.Ok);
        CHECK(autom.LastIds == std::vector<std::string>{"B-1", "B-2"});
    }
    {
        const nlohmann::json args = {{"script", "Auto.lua"}, {"process_all", true}};
        const CommandResult r = reg.Dispatch("automation.run-script", args, ctx);
        REQUIRE(r.Ok);
        CHECK(autom.LastProcessAll);
    }
    {
        // A present-but-empty script clears required-arg validation and hits the handler's guard.
        const CommandResult r = reg.Dispatch("automation.run-script", {{"script", ""}}, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::MissingRequiredArg);
    }
    {
        const CommandResult r = reg.Dispatch("automation.run-flat", {{"script", "Flat.lua"}}, ctx);
        REQUIRE(r.Ok);
        CHECK(autom.LastFlatScript == "Flat.lua");
    }
    {
        const CommandResult r = reg.Dispatch("automation.reload-hooks", {}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["reloaded"] == "SmatchetHooks.lua");
        CHECK(autom.LastSetupScript == "SmatchetHooks.lua");
    }
    {
        const CommandResult r = reg.Dispatch("automation.list-globals", {}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["count"] == 2);
        CHECK((*r.Data)["actions"][1] == "beta");
    }
    {
        // lua.approve-script / lua.revoke-script route through the VoidResult member-fn-pointer
        // helper (RegisterLuaScriptConsentCommand) after the #21 AppController public-API flip:
        // the success path reaches the fake, and the empty-script guard fails before dispatch.
        const CommandResult ok = reg.Dispatch("lua.approve-script", {{"script", "Hooks.lua"}}, ctx);
        REQUIRE(ok.Ok);
        CHECK((*ok.Data)["approved"] == "Hooks.lua");
        CHECK(autom.LastApprovedScript == "Hooks.lua");

        const CommandResult rev = reg.Dispatch("lua.revoke-script", {{"script", "Hooks.lua"}}, ctx);
        REQUIRE(rev.Ok);
        CHECK((*rev.Data)["revoked"] == "Hooks.lua");
        CHECK(autom.LastRevokedScript == "Hooks.lua");

        const CommandResult empty = reg.Dispatch("lua.approve-script", {{"script", ""}}, ctx);
        REQUIRE_FALSE(empty.Ok);
        CHECK(empty.Error.Code == ErrorCode::MissingRequiredArg);
    }
}

TEST_CASE("registry pre-handler pipeline — unknown command and missing required arg") {
    FakeTicketData data;
    CommandRegistry reg;
    smatchet::cmd::RegisterTicketsCommands(reg, data);

    {
        const CommandResult r = reg.Dispatch("tickets.nosuch", {}, AutomationCtx());
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::UnknownCommand);
    }
    {
        const CommandResult r = reg.Dispatch("tickets.get", {}, AutomationCtx());
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::MissingRequiredArg);
    }
}

TEST_CASE("commands.* — registry introspection over the real registered catalog") {
    // Meta takes no app object — it introspects the CommandRegistry it registers into. Seed
    // the registry with a couple of other builtins so list/search/recents have real rows.
    FakeTicketData data;
    CommandRegistry reg;
    smatchet::cmd::RegisterMetaCommands(reg);
    smatchet::cmd::RegisterTicketsCommands(reg, data);
    const CommandContext ctx = AutomationCtx();

    {
        const CommandResult r = reg.Dispatch("commands.list", {{"category", "tickets"}}, ctx);
        REQUIRE(r.Ok);
        // Every returned row is in the requested category.
        for (const auto& item : (*r.Data)["items"]) {
            CHECK(item["name"].get<std::string>().rfind("tickets.", 0) == 0);
        }
        CHECK((*r.Data)["total"].get<int>() >= 4);
    }
    {
        const CommandResult r = reg.Dispatch("commands.search", {{"query", "list_active"}}, ctx);
        REQUIRE(r.Ok);
        // Fuzzy search returns bare command-name strings (PaginateString); one must match.
        bool found = false;
        for (const auto& item : (*r.Data)["items"]) {
            if (item.get<std::string>().find("list_active") != std::string::npos) {
                found = true;
            }
        }
        CHECK(found);
    }
    {
        // Successful dispatches DO get recorded (Dispatch → RecordDispatch), so on this
        // registry recents already holds the earlier list/search calls, newest-first.
        const CommandResult r = reg.Dispatch("commands.recents", {}, ctx);
        REQUIRE(r.Ok);
        REQUIRE(r.Data->contains("items"));
        const auto& items = (*r.Data)["items"];
        REQUIRE(items.size() >= 2);
        CHECK(items[0] == "commands.search"); // most recent successful dispatch first
        CHECK(items[1] == "commands.list");
    }
}

TEST_CASE("commands.recents — fresh registry with no dispatches yields an empty list") {
    // A default-constructed registry loads nothing from disk (LoadRecents runs only under
    // AppController), so recents is a well-formed EMPTY envelope until a command dispatches.
    CommandRegistry reg;
    smatchet::cmd::RegisterMetaCommands(reg);
    const CommandResult r = reg.Dispatch("commands.recents", {}, AutomationCtx());
    REQUIRE(r.Ok);
    REQUIRE(r.Data->contains("items"));
    CHECK((*r.Data)["items"].empty());
    CHECK((*r.Data)["total"] == 0);
}

TEST_CASE("config.* — allowlist gate, dry-run preview, and persisted round-trip") {
    smatchet_tests::TestEnvGuard env; // config.set / tickets.monitor persist via ConfigManager
    CommandRegistry reg;
    smatchet::cmd::RegisterConfigCommands(reg);
    const CommandContext ctx = AutomationCtx();

    {
        const CommandResult r = reg.Dispatch("config.path", {}, ctx);
        REQUIRE(r.Ok);
        CHECK(r.Data->contains("userData"));
        CHECK((*r.Data)["env"].contains("observed"));
    }
    {
        // A key outside the config.set allowlist is a ValidationError (never written).
        const CommandResult r = reg.Dispatch("config.set", {{"key", "nope"}, {"value", "x"}}, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ValidationError);
    }
    {
        // Dry-run previews the cmd-key → json-key mapping without writing.
        const CommandResult r =
            reg.Dispatch("config.set", {{"key", "mcpPort"}, {"value", "9999"}}, AutomationCtx(false, /*dryRun*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data)["wouldDo"]["jsonKey"] == "mcp_port");
        CHECK((*r.Data)["wouldDo"]["value"] == 9999);
    }
    {
        // Real write round-trips: config.get reads back the value config.set persisted.
        const CommandResult w = reg.Dispatch("config.set", {{"key", "mcpPort"}, {"value", "8123"}}, ctx);
        REQUIRE(w.Ok);
        CHECK((*w.Data)["written"] == true);
        const CommandResult g = reg.Dispatch("config.get", {{"key", "mcpPort"}}, ctx);
        REQUIRE(g.Ok);
        CHECK((*g.Data)["mcpPort"] == 8123);
    }
    {
        // config.reload triggers a disk re-read (cache flush); it reports success.
        const CommandResult r = reg.Dispatch("config.reload", {}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["triggered"] == true);
    }
    {
        // tickets.monitor status is a read-only projection of the two monitor prefs.
        const CommandResult r = reg.Dispatch("tickets.monitor", {{"action", "status"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["action"] == "status");
        CHECK(r.Data->contains("enabled"));
        CHECK(r.Data->contains("intervalSec"));
    }
}

TEST_CASE("config.set rejects a non-integer for an integer key, and a damaged int never resets the fields after it") {
    smatchet_tests::TestEnvGuard env;
    CommandRegistry reg;
    smatchet::cmd::RegisterConfigCommands(reg);
    const CommandContext ctx = AutomationCtx();
    REQUIRE(reg.Dispatch("config.set", {{"key", "mcpPort"}, {"value", "8123"}}, ctx).Ok);
    REQUIRE(reg.Dispatch("config.set", {{"key", "maxUserChanges"}, {"value", "77"}}, ctx).Ok);
    const int ttlBefore = ConfigManager::Load().LookupCacheTtlSec;

    for (const char* bad : {"abc", "3.5", "\"7\"", "true", "99999999999", "18446744073709551615"}) {
        const CommandResult r = reg.Dispatch("config.set", {{"key", "lookupCacheTtlSec"}, {"value", bad}}, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ValidationError);
    }
    const TrackerConfig unchanged = ConfigManager::Load();
    CHECK(unchanged.LookupCacheTtlSec == ttlBefore);
    CHECK(unchanged.McpPort == 8123);
    CHECK(unchanged.MaxUserChanges == 77);

    // A damaged file (hand-edited): one bad integer costs only itself, whatever its position in the table.
    ConfigManager::UpdateConfigJson([](nlohmann::json& j) { j["window_x"] = "abc"; });
    const TrackerConfig damaged = ConfigManager::Load();
    CHECK(damaged.McpPort == 8123);
    CHECK(damaged.MaxUserChanges == 77);
    CHECK(damaged.LookupCacheTtlSec == ttlBefore);
}

TEST_CASE("config.set refuses a non-boolean for a boolean key and keeps the text of a string key as typed") {
    smatchet_tests::TestEnvGuard env;
    CommandRegistry reg;
    smatchet::cmd::RegisterConfigCommands(reg);
    const CommandContext ctx = AutomationCtx();
    REQUIRE(reg.Dispatch("config.set", {{"key", "readOnlyMode"}, {"value", "false"}}, ctx).Ok);

    for (const char* bad : {"yes", "42", "3.5", "\"maybe\"", "[]"}) {
        const CommandResult r = reg.Dispatch("config.set", {{"key", "readOnlyMode"}, {"value", bad}}, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ValidationError);
    }
    CHECK_FALSE(ConfigManager::Load().ReadOnlyMode); // the refused writes changed nothing

    // The spellings the vsync key takes work for the other boolean keys too.
    for (const char* on : {"on", "true", "1", "\"true\""}) {
        REQUIRE(reg.Dispatch("config.set", {{"key", "readOnlyMode"}, {"value", "false"}}, ctx).Ok);
        REQUIRE(reg.Dispatch("config.set", {{"key", "readOnlyMode"}, {"value", on}}, ctx).Ok);
        CHECK(ConfigManager::Load().ReadOnlyMode);
        CHECK(ConfigManager::LoadMergedConfigJson().at("read_only_mode").is_boolean());
    }

    // A string key takes the text as typed: 123 and true are not stored as a number and a boolean.
    for (const char* text : {"123", "true", "{\"a\":1}"}) {
        REQUIRE(reg.Dispatch("config.set", {{"key", "jqlQuery"}, {"value", text}}, ctx).Ok);
        REQUIRE(ConfigManager::LoadMergedConfigJson().at("jql").is_string());
        CHECK(ConfigManager::LoadMergedConfigJson().at("jql").get<std::string>() == text);
        CHECK(ConfigManager::Load().JqlQuery == text);
    }
}

TEST_CASE("config.set vsync takes the boolean spellings, stores a real boolean and applies it live") {
    // vsync has no normalisation of its own: it goes through the same boolean check as every boolean key.
    smatchet_tests::TestEnvGuard env;
    CommandRegistry reg;
    smatchet::cmd::RegisterConfigCommands(reg);
    const CommandContext ctx = AutomationCtx();
    const bool before = smatchet::vsync::Enabled();

    for (const char* off : {"off", "false", "0", "\"false\"", "\"off\""}) {
        smatchet::vsync::SetEnabled(true);
        const CommandResult r = reg.Dispatch("config.set", {{"key", "vsync"}, {"value", off}}, ctx);
        REQUIRE(r.Ok);
        CHECK_FALSE(smatchet::vsync::Enabled()); // applied live
        CHECK(ConfigManager::LoadMergedConfigJson().at("vsync_enabled").is_boolean());
        CHECK_FALSE(ConfigManager::Load().VsyncEnabled);
    }
    for (const char* on : {"on", "true", "1", "\"true\""}) {
        smatchet::vsync::SetEnabled(false);
        REQUIRE(reg.Dispatch("config.set", {{"key", "vsync"}, {"value", on}}, ctx).Ok);
        CHECK(smatchet::vsync::Enabled());
        CHECK(ConfigManager::Load().VsyncEnabled);
    }
    for (const char* bad : {"maybe", "5", "3.5", "[]"}) {
        const CommandResult r = reg.Dispatch("config.set", {{"key", "vsync"}, {"value", bad}}, ctx);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ValidationError);
        CHECK(r.Error.Message.find("must be a boolean") != std::string::npos);
    }
    smatchet::vsync::SetEnabled(before);
}

TEST_CASE("a hand-edited mistyped boolean, string or float costs only its own field, whatever its position") {
    smatchet_tests::TestEnvGuard env;
    ConfigManager::UpdateConfigJson([](nlohmann::json& j) {
        j["domain"] = "acme.atlassian.net";
        j["mcp_port"] = 8123;
    });
    const TrackerConfig defaults = TrackerConfig();
    ConfigManager::UpdateConfigJson([](nlohmann::json& j) {
        j["read_only_mode"] = "maybe"; // a string where a boolean belongs
        j["email"] = 7;                // a number where a string belongs
        j["show_log_window"] = nullptr;
    });

    const TrackerConfig loaded = ConfigManager::Load();

    CHECK(loaded.ReadOnlyMode == defaults.ReadOnlyMode);
    CHECK(loaded.Email == defaults.Email);
    CHECK(loaded.ShowLogWindow == defaults.ShowLogWindow);
    CHECK(loaded.Domain == "acme.atlassian.net"); // before the damaged keys in the table
    CHECK(loaded.McpPort == 8123);                // after them
}

TEST_CASE("config.set takes an integral float like the load does (60.0) and still refuses a fraction (3.5)") {
    smatchet_tests::TestEnvGuard env;
    CommandRegistry reg;
    smatchet::cmd::RegisterConfigCommands(reg);
    const CommandContext ctx = AutomationCtx();
    REQUIRE(reg.Dispatch("config.set", {{"key", "mcpPort"}, {"value", "8123"}}, ctx).Ok);

    const CommandResult accepted = reg.Dispatch("config.set", {{"key", "mcpPort"}, {"value", "60.0"}}, ctx);
    REQUIRE(accepted.Ok);
    CHECK(ConfigManager::Load().McpPort == 60);
    // Stored as the integer 60, not the float: the file stays in the shape the int table writes.
    CHECK(ConfigManager::LoadMergedConfigJson().at("mcp_port").is_number_integer());

    const CommandResult refused = reg.Dispatch("config.set", {{"key", "mcpPort"}, {"value", "3.5"}}, ctx);
    REQUIRE_FALSE(refused.Ok);
    CHECK(refused.Error.Code == ErrorCode::ValidationError);
    CHECK(ConfigManager::Load().McpPort == 60); // the refused write changed nothing
}

TEST_CASE("a hand-edited integral float for an integer key still loads; a fractional or out-of-range one is ignored") {
    smatchet_tests::TestEnvGuard env;
    ConfigManager::UpdateConfigJson([](nlohmann::json& j) {
        j["mcp_port"] = 8123.0;      // integral float: accepted as 8123
        j["max_user_changes"] = 3.5; // fractional: keeps its default
        j["window_x"] = 1e30;        // far out of int range: keeps its default
        j["window_y"] = -250.0;      // negative integral float
    });
    const TrackerConfig defaults = TrackerConfig();
    const TrackerConfig loaded = ConfigManager::Load();
    CHECK(loaded.McpPort == 8123);
    CHECK(loaded.MaxUserChanges == defaults.MaxUserChanges);
    CHECK(loaded.WindowX == defaults.WindowX);
    CHECK(loaded.WindowY == -250);
}

TEST_CASE("fields.* — catalog listing/pagination, get lookup, refresh plumbing, icon resolution") {
    smatchet_tests::TestEnvGuard env; // fields.refresh_catalog reads ConfigManager::Load()
    FakeFields fields;
    CommandRegistry reg;
    smatchet::cmd::RegisterFieldsCommands(reg, fields);
    const CommandContext ctx = AutomationCtx();

    {
        // list_available projects id/name/type and honours the pagination window.
        const CommandResult r = reg.Dispatch("fields.list_available", {}, ctx);
        REQUIRE(r.Ok);
        REQUIRE((*r.Data)["items"].size() == 2);
        CHECK((*r.Data)["items"][0]["id"] == "priority");
        CHECK((*r.Data)["items"][0]["type"] == "priority");
        CHECK((*r.Data)["items"][1]["name"] == "Summary");

        const CommandResult paged = reg.Dispatch("fields.list_available", {{"limit", 1}, {"offset", 1}}, ctx);
        REQUIRE(paged.Ok);
        REQUIRE((*paged.Data)["items"].size() == 1);
        CHECK((*paged.Data)["items"][0]["id"] == "summary");
    }
    {
        // get resolves a known id; an unknown id is a NotFound failure, not an empty ok.
        const CommandResult hit = reg.Dispatch("fields.get", {{"id", "priority"}}, ctx);
        REQUIRE(hit.Ok);
        CHECK((*hit.Data)["name"] == "Priority");

        const CommandResult miss = reg.Dispatch("fields.get", {{"id", "ghost"}}, ctx);
        REQUIRE_FALSE(miss.Ok);
        CHECK(miss.Error.Code == ErrorCode::NotFound);
    }
    {
        // refresh_catalog forwards to the facet and reports ok + the live field count.
        fields.RefreshOk = true;
        const CommandResult r = reg.Dispatch("fields.refresh_catalog", {}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["ok"] == true);
        CHECK((*r.Data)["fieldCount"] == 2);
        CHECK(fields.RefreshCalls == 1);

        // A failed refresh surfaces the facet's catalog error string.
        fields.RefreshOk = false;
        fields.CatalogError = "backend unreachable";
        const CommandResult bad = reg.Dispatch("fields.refresh_catalog", {}, ctx);
        REQUIRE(bad.Ok); // the command succeeds; the failure rides in the payload
        CHECK((*bad.Data)["ok"] == false);
        CHECK((*bad.Data)["error"] == "backend unreachable");

        // A refresh dropped because the pane moved on records no catalog error; the payload still says why.
        fields.CatalogError.clear();
        const CommandResult dropped = reg.Dispatch("fields.refresh_catalog", {}, ctx);
        REQUIRE(dropped.Ok);
        CHECK((*dropped.Data)["ok"] == false);
        CHECK_FALSE((*dropped.Data)["error"].get<std::string>().empty());
    }
    {
        // icon_for passes the field+value pair through and echoes the resolved target.
        fields.IconFound = true;
        fields.IconTarget = "/icons/prio-high.png";
        const CommandResult r = reg.Dispatch("fields.icon_for", {{"field", "priority"}, {"value", "High"}}, ctx);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["found"] == true);
        CHECK((*r.Data)["pathOrUrl"] == "/icons/prio-high.png");
        CHECK((*r.Data)["field"] == "priority");
        CHECK((*r.Data)["value"] == "High");
        CHECK(fields.LastIconFieldId == "priority");
        CHECK(fields.LastIconValue == "High");

        // A miss yields found=false with an empty path (never the stale target).
        fields.IconFound = false;
        const CommandResult miss = reg.Dispatch("fields.icon_for", {{"field", "summary"}, {"value", "x"}}, ctx);
        REQUIRE(miss.Ok);
        CHECK((*miss.Data)["found"] == false);
        CHECK((*miss.Data)["pathOrUrl"] == "");
    }
}
