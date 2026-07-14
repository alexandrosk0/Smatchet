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

#include "ConfigManager.h"
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

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
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
    bool FetchIssueWatchers(const std::string&, std::vector<TrackerUser>& outWatchers,
                            std::string& outError) const override {
        if (Fail) {
            outError = "watchers boom";
            return false;
        }
        TrackerUser u;
        u.AccountId = "w1";
        u.DisplayName = "Watcher One";
        outWatchers.push_back(u);
        return true;
    }
    bool FetchIssueVotes(const std::string&, std::vector<TrackerUser>& outVoters, std::string& outError,
                         int* outVoteCount, bool*, bool*) const override {
        if (Fail) {
            outError = "votes boom";
            return false;
        }
        TrackerUser u;
        u.AccountId = "v1";
        u.DisplayName = "Voter One";
        outVoters.push_back(u);
        if (outVoteCount) {
            *outVoteCount = 3;
        }
        return true;
    }
    bool SearchUsersByQuery(const std::string&, std::vector<TrackerUser>& outUsers,
                            std::string& outError) const override {
        if (Fail) {
            outError = "search boom";
            return false;
        }
        TrackerUser u;
        u.AccountId = "s1";
        u.DisplayName = "Search Hit";
        u.EmailAddress = "hit@example.com";
        outUsers.push_back(u);
        return true;
    }
};

struct FakeSync : IAppSync {
    int SyncCalls = 0;
    int RecreateCalls = 0;
    int RefreshCalls = 0;
    std::string RecreateError;
    TrackerConnectivityState State = TrackerConnectivityState::AuthenticatedReachable;
    std::string SyncWarning = "stale view";
    std::string CatalogError;
    TrackerIssueFetchPack Pack;

    void SyncWithBackend(const TrackerConfig*, const ViewsStore*) override { ++SyncCalls; }
    bool RecreateLocalCacheDatabase(std::string& outError) override {
        ++RecreateCalls;
        outError = RecreateError;
        return RecreateError.empty();
    }
    void RefreshLocalData() override { ++RefreshCalls; }
    TrackerIssueFetchPack FetchIssuesForActiveView(const TrackerConfig*, const ViewsStore*) override { return Pack; }
    TrackerConnectivityState GetLastTrackerConnectivityState() const override { return State; }
    const std::string& GetLastTicketSyncWarning() const override { return SyncWarning; }
    const std::string& GetFieldCatalogError() const override { return CatalogError; }
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
    std::string GetAppVersion() const override { return "9.9.9-test"; }
    std::string GetGitHubReleaseRepo() const override { return "owner/smatchet"; }
    void RequestAppQuit() const override { ++QuitRequests; }
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
    bool SubmitOk = true;
    bool CommentOk = true;
    bool WorklogOk = true;
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
    bool SubmitFieldEdit(const std::string& issueId, const TrackerField&, const std::vector<std::string>& rawValues,
                         std::string& outError) override {
        LastEditIssue = issueId;
        LastEditValues = rawValues;
        if (!SubmitOk) {
            outError = "edit rejected";
        }
        return SubmitOk;
    }
    bool AddIssueCommentPlain(const std::string&, const std::string&, std::string& outError) override {
        if (!CommentOk) {
            outError = "comment rejected";
        }
        return CommentOk;
    }
    bool SubmitWorklog(const std::string&, const std::string& timeSpent, const std::string&, const std::string&,
                       const std::string&, const std::string&, std::string& outError) override {
        LastWorklogTimeSpent = timeSpent;
        if (!WorklogOk) {
            outError = "worklog rejected";
        }
        return WorklogOk;
    }
    std::int64_t QueueCreateOffline(const IssueDraft&) override { return QueuedId; }
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

TEST_CASE("sync.* — trigger plumbing, destructive gate on sync.full, status mapping, fetch pack") {
    FakeSync sync;
    CommandRegistry reg;
    smatchet::cmd::RegisterSyncCommands(reg, sync);

    {
        const CommandResult r = reg.Dispatch("sync.incremental", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["triggered"] == true);
        CHECK(sync.SyncCalls == 1);
    }
    {
        // Unconfirmed automation source is stopped by the registry BEFORE the handler runs.
        const CommandResult r = reg.Dispatch("sync.full", {}, AutomationCtx());
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ConfirmRequired);
        CHECK(sync.RecreateCalls == 0);
    }
    {
        const CommandResult r = reg.Dispatch("sync.full", {}, AutomationCtx(false, /*dryRun*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data).contains("wouldDo"));
        CHECK(sync.RecreateCalls == 0);
    }
    {
        sync.RecreateError = "cache locked";
        const CommandResult r = reg.Dispatch("sync.full", {}, AutomationCtx(/*confirmed*/ true));
        REQUIRE(r.Ok);
        CHECK((*r.Data)["warning"] == "cache locked");
        CHECK(sync.RecreateCalls == 1);
        CHECK(sync.SyncCalls == 2);
    }
    {
        const CommandResult r = reg.Dispatch("sync.refresh_local", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK(sync.RefreshCalls == 1);
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
        const CommandResult r = reg.Dispatch("sync.fetch_active_view", {}, AutomationCtx());
        REQUIRE(r.Ok);
        CHECK((*r.Data)["tickets"][0]["summary"] == "fetched");
        CHECK((*r.Data)["error"] == "partial page");
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
        mut.SubmitOk = false;
        const nlohmann::json args = {{"id", "PROJ-1"}, {"field", "status"}, {"value", "Done"}};
        const CommandResult r = reg.Dispatch("ticket.set_field", args, yes);
        REQUIRE_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::BackendError);
        mut.SubmitOk = true;
    }
    {
        const CommandResult r = reg.Dispatch("ticket.add_comment", {{"id", "PROJ-1"}, {"body", "hi"}}, yes);
        REQUIRE(r.Ok);
        mut.CommentOk = false;
        const CommandResult fail = reg.Dispatch("ticket.add_comment", {{"id", "PROJ-1"}, {"body", "hi"}}, yes);
        CHECK(fail.Error.Code == ErrorCode::BackendError);
    }
    {
        // 5400 s -> "1h 30m"; 2700 s -> "45m"; 0 s -> "0s".
        const CommandResult r = reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 5400}}, yes);
        REQUIRE(r.Ok);
        CHECK((*r.Data)["timeSpent"] == "1h 30m");
        reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 2700}}, yes);
        CHECK(mut.LastWorklogTimeSpent == "45m");
        reg.Dispatch("ticket.add_worklog", {{"id", "PROJ-1"}, {"seconds", 0}}, yes);
        CHECK(mut.LastWorklogTimeSpent == "0s");
    }
    {
        const CommandResult r = reg.Dispatch("ticket.transition", {{"id", "PROJ-1"}, {"toStatus", "Done"}}, yes);
        REQUIRE(r.Ok);
        CHECK(mut.LastEditValues == std::vector<std::string>{"Done"});
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
