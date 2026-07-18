#include "AppControllerImpl.h" // AppController::Impl — cold sol2/automation member storage (pImpl #19b)
#include "ILuaBindingHost.h"
#include "LuaAutomationHost.h"
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls app_.Cache-> methods.

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AiAssistantController.h"
#include "AiLuaPromptRateLimit.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IssueTableSerializer.h"
#include "LuaAutomationHookPolicyPure.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory> // std::atomic_load / atomic_store on the shared_ptr Cache (DR6)
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <exception>
#include <future>
#include <ghc/filesystem.hpp>
#include <limits>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <iterator>

#include <nlohmann/json.hpp>
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared AppController_LuaBindings-TU include prologue is grandfathered across the god-file-split siblings (AppController_LuaBindings.cpp / _Ui / _Ai / _Tickets) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared AppController_LuaBindings TU prologue header is introduced)
// clang-format on

#include "Json/BoundedJsonParse.h"

#include "imgui.h"
#include "Logger.h"
#include "SmatchetFieldIconRender.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"
#include "UiPerfMonitor.h"

#include "AppController_LuaBindings_detail.h"

namespace {

// CPP_CODE_AUDIT.md #20: dense-array join recursion depth cap, mirroring
// lua_json_detail::kJsonToLuaMaxDepth — without it a cyclic dense array
// (`local t={}; t[1]=t`) recurses without bound (stack overflow, SIGSEGV).
constexpr int kLuaObjectToIssueFieldStringMaxDepth = 64;

/** Flatten Lua values the same way as JSON import cells (strings, numbers, bools, simple arrays). */
static std::string LuaObjectToIssueFieldString(const sol::object& v, std::size_t maxDump = 4096, int depth = 0) {
    if (depth > kLuaObjectToIssueFieldStringMaxDepth) {
        return std::string("?");
    }
    if (!v.valid() || v.get_type() == sol::type::lua_nil) {
        return std::string();
    }
    if (v.is<bool>()) {
        return v.as<bool>() ? std::string("true") : std::string("false");
    }
    if (v.is<double>()) {
        const double d = v.as<double>();
        if (d == std::floor(d) && d >= static_cast<double>((std::numeric_limits<std::int64_t>::min)()) &&
            d <= static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
            return std::to_string(static_cast<std::int64_t>(d));
        }
        return std::to_string(d);
    }
    if (v.is<std::string>()) {
        return v.as<std::string>();
    }
    if (v.is<sol::table>()) {
        sol::table tbl = v.as<sol::table>();
        std::size_t maxIdx = 0;
        bool hasNonIntKey = false;
        tbl.for_each([&](sol::object k, sol::object /*val*/) {
            if (hasNonIntKey) {
                return;
            }
            std::size_t idx = 0;
            if (k.is<std::size_t>()) {
                idx = k.as<std::size_t>();
            } else if (k.is<int>()) {
                const int iv = k.as<int>();
                if (iv < 1) {
                    hasNonIntKey = true;
                    return;
                }
                idx = static_cast<std::size_t>(iv);
            } else {
                hasNonIntKey = true;
                return;
            }
            if (idx > maxIdx) {
                maxIdx = idx;
            }
        });
        if (!hasNonIntKey && maxIdx > 0 && maxIdx <= 100000) {
            bool dense = true;
            for (std::size_t i = 1; i <= maxIdx; ++i) {
                const sol::object el = tbl[i];
                if (!el.valid() || el.get_type() == sol::type::lua_nil) {
                    dense = false;
                    break;
                }
            }
            if (dense) {
                std::string joined;
                for (std::size_t i = 1; i <= maxIdx; ++i) {
                    if (!joined.empty()) {
                        joined.push_back(',');
                    }
                    joined += LuaObjectToIssueFieldString(tbl[i], maxDump / (maxIdx + 1), depth + 1);
                }
                return joined;
            }
        }
        try {
            std::string dumped = LuaToJson(v).dump();
            if (dumped.size() > maxDump) {
                return dumped.substr(0, maxDump) + "...";
            }
            return dumped;
        } catch (...) { // catch-all-ok: dump on Lua value for logging
            return std::string("?");
        }
    }
    return std::string();
}

static void LuaApplyIssueCreateKv(IssueDraft& draft, const std::string& rawKey, const std::string& val,
                                  const std::vector<TrackerField>& catalog) {
    const std::string low = AsciiLowerCopy(rawKey);
    if (low == "issuetypeid" || low == "issue_type_id") {
        draft.IssueTypeId = val;
        return;
    }
    if (low == "issuetypename" || low == "issue_type_name") {
        draft.IssueTypeName = val;
        return;
    }
    if (low == "projectkey" || low == "project_key") {
        IssueTableSerializer::ApplyKeyValueToDraft(draft, "project", val);
        return;
    }
    if (low == "parentkey" || low == "parent_key") {
        IssueTableSerializer::ApplyKeyValueToDraft(draft, "parent", val);
        return;
    }
    if (low == "existingissuekey" || low == "existing_issue_key") {
        draft.ExistingIssueKey = val;
        return;
    }
    const std::string resolved = IssueTableSerializer::ResolveColumnKey(rawKey, catalog);
    if (resolved.empty()) {
        return;
    }
    IssueTableSerializer::ApplyKeyValueToDraft(draft, resolved, val);
}

static void LuaMergeIssueCreateSpec(IssueDraft& draft, sol::table spec, const std::vector<TrackerField>& catalog) {
    spec.for_each([&](sol::object kObj, sol::object vObj) {
        if (!kObj.is<std::string>()) {
            return;
        }
        const std::string rawKey = kObj.as<std::string>();
        const std::string low = AsciiLowerCopy(rawKey);
        if (low == "offline" || low == "queue_offline") {
            return;
        }
        if (low == "fields" && vObj.is<sol::table>()) {
            LuaMergeIssueCreateSpec(draft, vObj.as<sol::table>(), catalog);
            return;
        }
        LuaApplyIssueCreateKv(draft, rawKey, LuaObjectToIssueFieldString(vObj), catalog);
    });
}

} // namespace

// Sol-free interface methods kept on AppController (see AppController.h); Impl forwards to app_.
std::vector<CachedTicket> AppController::LuaGetActiveTicketsBind() {
    const auto snap = GetActiveTicketsSnapshot();
    return std::vector<CachedTicket>(snap->begin(), snap->end());
}

std::vector<CachedTicket> AppController::Impl::LuaGetActiveTicketsBind() { return app_.LuaGetActiveTicketsBind(); }

const TrackerField* AppController::Impl::FindFieldById(const std::string& fieldId) const {
    return app_.FindFieldById(fieldId);
}

VoidResult AppController::Impl::SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                                const std::vector<std::string>& rawValues) {
    return app_.SubmitFieldEdit(issueId, field, rawValues);
}

std::tuple<sol::object, std::string> AppController::Impl::LuaGetTicketBind(sol::state_view sv,
                                                                           const std::string& issueId) {
    // Marshal against the *calling* state `sv`, not the member `lua`: the caller may be an
    // off-UI-thread fresh state (MCP / automation worker). Touching `lua` here would re-introduce
    // cross-thread lua_State access + a cross-state sol::object return. See
    // docs/plans/shipped/mcp-lua-fresh-state-race.md.
    CachedTicket ticket;
    // CacheBackendKeyCopy is mutex-guarded — this bind runs on the Lua automation / MCP
    // worker thread while the UI thread may re-stamp the key on a tracker swap (Slice 1b).
    // DR6: snapshot Cache with atomic_load and hold the shared_ptr for the whole deref, so a
    // concurrent UI-thread RecreateLocalCacheDatabase swap can't free the cache under us. A null
    // snapshot degrades to a nil return (missing ticket) rather than a crash (Pillar 3). Mirrors
    // the ADR-0012 Backend atomic_load reader pattern.
    auto cacheSnap = std::atomic_load(&app_.Cache);
    if (cacheSnap && cacheSnap->TryGetTicket(app_.focusedContext().CacheBackendKeyCopy(), issueId, ticket)) {
        return {sol::make_object(sv, ticket), ""};
    }
    return {sol::make_object(sv, sol::nil), "Ticket not found in local cache"};
}

// `decode_json` parses ATTACKER-CONTROLLED text (a Lua script can hand it any
// string). nlohmann's DOM parser is iterative (it drives `json_sax_dom_parser`
// from `sax_parse`, NOT a per-level recursion), so a deeply-nested payload
// ("[[[[...]]]]") does NOT overflow while parsing — it builds the full DOM, and
// THAT deep tree overflows the C++ stack when it is destroyed (`~json` recurses
// per nesting level) and grows the heap unboundedly while it is built (Pillar 3 —
// Never crash). The 4 MB byte cap in LuaDecodeJsonBind does NOT bound depth —
// ~2 M nested arrays fit easily.
// The depth/node-bounded parse lives in the shared smatchet::json_safe helper
// (Source/Core/include/Json/BoundedJsonParse.h) so this sink, the MCP REST /
// JSON-RPC ingress, and the Lua-MCP-tool params path all route through ONE
// BoundedDecodeSax (DRY / Pillar 5). The caps sit far above any legitimate JSON
// the bindings exchange. On overflow we return a parse error string up to Lua —
// graceful degradation, NOT a C++ throw across the sol2 boundary.
std::tuple<sol::object, std::string> AppController::Impl::LuaDecodeJsonBind(sol::state_view sv, const std::string& s) {
    // Marshal against the calling state `sv` (see LuaGetTicketBind).
    // Depth/node-bounded parse via the shared json_safe helper (NOT a bare
    // json::parse, which would build the full DOM and then stack-overflow when that
    // deep tree is torn down — see the recursive `~json` note above). On a cap hit
    // ParseBounded returns null + a non-empty errOut: report it to Lua as a parse
    // error string rather than crashing or throwing across the sol2 boundary
    // (Pillar 3). Keep the 4 MiB byte cap on top of the depth/node caps.
    constexpr std::size_t kMaxDecodeBytes = 4u * 1024u * 1024u;
    std::string err;
    const nlohmann::json j = smatchet::json_safe::ParseBounded(s, err, kMaxDecodeBytes);
    if (!err.empty()) {
        if (err == smatchet::json_safe::OverflowError()) {
            // decode_json is reachable concurrently (Lua automation + MCP worker
            // threads), so the once-only warn latch must be atomic — a plain `static
            // bool` is a data race under concurrent overflow (issue #1287). exchange()
            // makes exactly one thread observe the false→true edge.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                LOG_WARN("decode_json: input exceeded depth (%d) or node (%zu) cap; rejecting. "
                         "Possible hostile or malformed payload.",
                         smatchet::json_safe::kDefaultMaxDepth, smatchet::json_safe::kDefaultMaxNodes);
            }
        }
        return {sol::make_object(sv, sol::nil), err};
    }
    return {JsonToLua(sv, j), std::string()};
}

std::tuple<sol::object, std::string> AppController::Impl::LuaCreateIssueBind(sol::state_view sv, sol::table spec) {
    // Marshal against the calling state `sv`, not the member `lua` (see LuaGetTicketBind):
    // `spec` is already on `sv`, and the result table must be too.
    const TrackerConfig cfg = ConfigManager::Load();
    // Same base as the grid new-issue row: config fallbacks plus last-row project / issue type when present.
    IssueDraft draft = app_.BuildDraftFromLastTicket(cfg);

    sol::object offlineObj = spec["offline"];
    if (!offlineObj.valid() || offlineObj.get_type() == sol::type::lua_nil) {
        offlineObj = spec["queue_offline"];
    }
    const bool offline = LuaTruthy(offlineObj);

    LuaMergeIssueCreateSpec(draft, std::move(spec), app_.fieldCatalog().AvailableFields);

    sol::table result = sv.create_table();

    if (offline) {
        // DR6: snapshot Cache with atomic_load — this bind runs on the Lua automation / MCP
        // worker thread and RecreateLocalCacheDatabase may swap the cache concurrently on the UI
        // thread. Mirrors the ADR-0012 Backend atomic_load reader pattern. QueueCreateOffline
        // itself takes its own snapshot via deps_.CacheShared() for the enqueue path.
        auto cacheSnap = std::atomic_load(&app_.Cache);
        if (!cacheSnap) {
            return {sol::make_object(sv, sol::nil),
                    std::string("Local cache not initialized (cannot queue offline create)")};
        }
        const std::int64_t qid = app_.QueueCreateOffline(draft);
        if (qid <= 0) {
            result["ok"] = false;
            result["error"] = std::string("Failed to queue offline create (see logs)");
            return {result, std::string()};
        }
        result["ok"] = true;
        result["offline_queued_id"] = static_cast<double>(qid);
        result["issue_key"] = std::string("offline:") + std::to_string(qid);
        result["error"] = std::string();
        return {result, std::string()};
    }

    std::future<IssueCreateResult> fut = app_.CreateIssueAsync(draft);
    IssueCreateResult r;
    try {
        r = fut.get();
    } catch (const std::exception& e) {
        return {sol::make_object(sv, sol::nil),
                std::string("create_issue failed while waiting for result: ") + e.what()};
    } catch (...) { // catch-all-ok: future::get may throw any type; surface as a clean error string
        return {sol::make_object(sv, sol::nil),
                std::string("create_issue failed while waiting for result: unknown exception")};
    }

    result["ok"] = r.Ok;
    if (!r.IssueKey.empty()) {
        result["issue_key"] = r.IssueKey;
    }
    result["error"] = r.Error;
    if (!r.MissingFieldIds.empty()) {
        sol::table miss = sv.create_table();
        std::size_t i = 1;
        for (const auto& id : r.MissingFieldIds) {
            miss[i++] = id;
        }
        result["missing_field_ids"] = miss;
    }
    if (!r.AttachmentFailures.empty()) {
        sol::table af = sv.create_table();
        std::size_t i = 1;
        for (const auto& p : r.AttachmentFailures) {
            sol::table row = sv.create_table();
            row["path"] = p.first;
            row["reason"] = p.second;
            af[i++] = row;
        }
        result["attachment_failures"] = af;
    }
    return {result, std::string()};
}
