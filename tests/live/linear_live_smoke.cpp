// linear_live_smoke.cpp — credential-gated LIVE end-to-end smoke for the Linear
// backend (the manual-residue item from docs/plans/active/linear-tracker-backend.md,
// automated). Drives the REAL LinearClient against a real Linear workspace:
// reachability → fetch → create → update → comment → archive (cleanup).
//
// This is NOT part of the normal test suite. It is an EXCLUDE_FROM_ALL target run
// only by .github/workflows/linear-live-smoke.yml (nightly + manual dispatch),
// gated on the LINEAR_TEST_API_KEY secret. No sibling backend has a live test —
// this is the one path CI can't cover hermetically (the hermetic half is
// LinearMutationPure.test.cpp + the deterministic fixture backend).
//
// Env contract (all but the key are optional):
//   SMATCHET_LINEAR_LIVE_API_KEY   personal API key (REQUIRED; absent → SKIP, exit 0)
//   SMATCHET_LINEAR_LIVE_TEAM_KEY  team key, e.g. "ENG" — resolves the team UUID
//                                  (the create scope) when TEAM_ID is unset
//   SMATCHET_LINEAR_LIVE_TEAM_ID   team UUID; optional when TEAM_KEY is set (it is
//                                  then resolved from the key via the teams query)
//   SMATCHET_LINEAR_LIVE_BASE_URL  defaults to https://api.linear.app/graphql
//
// Exit codes: 0 = pass (or SKIP when no key), 1 = a step failed. Creates a single
// throwaway issue tagged "[smatchet-smoke]" and archives it on the way out;
// archive failures are warned, not fatal (run against a sandbox team).

#include "ConfigManager.h"
#include "IssueDraft.h"
#include "LinearClient.h"
#include "LinearClientHelpers.h"
#include "TrackerFieldSchema.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

const char* EnvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

// Report one step; a false condition records a failure but the smoke continues so
// one run surfaces every broken step, not just the first.
void Step(const char* name, bool ok, const std::string& detail) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) {
        ++g_failures;
    }
}

// Case-insensitive string equality (Linear team keys are upper-case, but accept
// whatever case the operator configured).
bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::string::size_type i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Resolve a team UUID from its human-readable key via the `teams` query so the
// operator only has to configure the key, not hunt for the UUID. Direct cpr (not
// LinearClient — team listing is not part of the tested mutation surface). Returns
// empty on any failure / no match.
std::string ResolveTeamIdByKey(const std::string& apiUrl, const std::string& apiKey, const std::string& teamKey) {
    const cpr::Header header{{"Content-Type", "application/json"}, {"Authorization", apiKey}};
    const cpr::Response resp = cpr::Post(
        cpr::Url{apiUrl}, header,
        cpr::Body{smatchet::linear::BuildGraphQLBody("{ teams { nodes { id key } } }", nlohmann::json::object())});
    if (resp.status_code != 200) {
        return "";
    }
    const nlohmann::json parsed = nlohmann::json::parse(resp.text, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("teams") || !parsed["data"]["teams"].is_object()) {
        return "";
    }
    const nlohmann::json nodes = parsed["data"]["teams"].value("nodes", nlohmann::json::array());
    for (const auto& node : nodes) {
        if (node.is_object() && IEquals(node.value("key", std::string()), teamKey)) {
            return node.value("id", std::string());
        }
    }
    return "";
}

// Best-effort cleanup: resolve the created issue's UUID then issueArchive it.
// Direct cpr (not LinearClient — archive is not part of the tested surface).
// Warns on failure; never fails the smoke (sandbox hygiene, not a contract).
void ArchiveIssue(const std::string& apiUrl, const std::string& apiKey, const std::string& identifier) {
    if (identifier.empty()) {
        return;
    }
    const cpr::Header header{{"Content-Type", "application/json"}, {"Authorization", apiKey}};

    nlohmann::json resolveVars;
    resolveVars["id"] = identifier;
    const cpr::Response r1 = cpr::Post(cpr::Url{apiUrl}, header,
                                       cpr::Body{smatchet::linear::BuildGraphQLBody(
                                           "query($id:String!){ issue(id:$id){ id } }", resolveVars)});
    const nlohmann::json p1 = nlohmann::json::parse(r1.text, nullptr, false);
    const std::string uuid =
        (!p1.is_discarded() && p1.contains("data") && p1["data"].contains("issue") && p1["data"]["issue"].is_object())
            ? p1["data"]["issue"].value("id", std::string())
            : std::string();
    if (uuid.empty()) {
        std::printf("[WARN] cleanup: could not resolve UUID for %s — leaving the issue\n", identifier.c_str());
        return;
    }
    nlohmann::json archiveVars;
    archiveVars["id"] = uuid;
    const cpr::Response r2 = cpr::Post(cpr::Url{apiUrl}, header,
                                       cpr::Body{smatchet::linear::BuildGraphQLBody(
                                           "mutation($id:String!){ issueArchive(id:$id){ success } }", archiveVars)});
    std::printf("[INFO] cleanup: issueArchive(%s) HTTP %ld\n", identifier.c_str(), static_cast<long>(r2.status_code));
}

} // namespace

int main() {
    const char* apiKey = std::getenv("SMATCHET_LINEAR_LIVE_API_KEY");
    if (apiKey == nullptr || apiKey[0] == '\0') {
        std::printf("[SKIP] SMATCHET_LINEAR_LIVE_API_KEY not set — live smoke not run.\n");
        return 0;
    }
    const std::string apiUrl = EnvOr("SMATCHET_LINEAR_LIVE_BASE_URL", "https://api.linear.app/graphql");
    std::string teamId = EnvOr("SMATCHET_LINEAR_LIVE_TEAM_ID", "");
    const std::string teamKey = EnvOr("SMATCHET_LINEAR_LIVE_TEAM_KEY", "");

    // Operator convenience: when only the human-readable key is configured, resolve
    // the team UUID (the create scope) from it automatically, so the operator never
    // has to look the UUID up by hand.
    if (teamId.empty() && !teamKey.empty()) {
        teamId = ResolveTeamIdByKey(apiUrl, apiKey, teamKey);
        if (teamId.empty()) {
            std::printf("[WARN] could not resolve team key '%s' to a UUID\n", teamKey.c_str());
        } else {
            std::printf("[INFO] resolved team key '%s' -> %s\n", teamKey.c_str(), teamId.c_str());
        }
    }

    // Isolated profile dir so the smoke never writes a developer/runner's real
    // config, and so the cfg-less mutation paths (ResolveAuth(nullptr) →
    // ConfigManager::Load) read back exactly what we seed here.
    char tmpl[] = "/tmp/linear-smoke-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (dir != nullptr) {
        setenv("SMATCHET_USER_DATA", dir, 1);
    }

    TrackerConfig cfg;
    cfg.TrackerType = "Linear";
    cfg.LinearApiKey = apiKey;
    cfg.LinearBaseUrl = apiUrl;
    cfg.LinearTeamId = teamId;
    cfg.LinearTeamKey = teamKey;
    ConfigManager::Save(cfg);

    // Gate: the cfg-less create/update paths only work if the key round-trips to
    // disk on this platform (DPAPI on Windows, sealed fallback elsewhere). Verify
    // before proceeding so a persistence gap fails loudly here, not mid-flow.
    const TrackerConfig reloaded = ConfigManager::Load();
    Step("config_key_roundtrip", reloaded.LinearApiKey == std::string(apiKey),
         reloaded.LinearApiKey.empty() ? "reloaded key is empty" : "");

    LinearClient client(apiUrl, apiKey);

    // 1) Reachability.
    const TrackerReachabilityProbeResult probe = client.ProbeReachability(cfg);
    Step("reachability", probe.Kind == TrackerReachabilityProbeKind::AuthenticatedReachable, probe.Diagnostic);

    // 2) Fetch (empty result is fine on a fresh sandbox; an error is not).
    bool fullSync = false;
    std::string fetchErr;
    std::string fetchWarn;
    const std::vector<CachedTicket> issues = client.FetchIssues(&fullSync, nullptr, nullptr, &fetchErr, &fetchWarn);
    Step("fetch", fetchErr.empty(), fetchErr.empty() ? (std::to_string(issues.size()) + " issue(s)") : fetchErr);

    // 3) Create a throwaway issue (requires a configured team).
    std::string createdIdentifier;
    if (teamId.empty()) {
        Step("create", false,
             "no team scope — set SMATCHET_LINEAR_LIVE_TEAM_ID or a resolvable SMATCHET_LINEAR_LIVE_TEAM_KEY");
    } else {
        IssueDraft draft;
        const std::string title = std::string("[smatchet-smoke] ") + std::to_string(static_cast<long>(std::time(nullptr)));
        draft.FieldValues["summary"] = title;
        draft.FieldValues["description"] = "Automated Smatchet live smoke — safe to delete.";
        const Result<nlohmann::json, TrackerError> payload = client.BuildCreatePayload(draft, {});
        if (!payload) {
            Step("create", false, payload.error().Detail);
        } else {
            const Result<std::string, TrackerError> created = client.CreateIssue(payload.value());
            if (!created) {
                Step("create", false, created.error().Detail);
            } else {
                createdIdentifier = created.value();
                Step("create", !createdIdentifier.empty(),
                     createdIdentifier.empty() ? "created but no identifier returned" : createdIdentifier);
            }
        }
    }

    // 4) Update + 5) Comment on the created issue (skip if create failed).
    if (!createdIdentifier.empty()) {
        TrackerField summaryField;
        summaryField.Id = "summary";
        const TrackerError upd =
            client.UpdateField(createdIdentifier, summaryField, {std::string("[smatchet-smoke] updated")});
        Step("update", upd.IsOk(), upd.IsOk() ? "" : upd.Detail);

        const TrackerError cmt = client.AddIssueCommentPlain(cfg, createdIdentifier, "Smatchet live smoke comment.");
        Step("comment", cmt.IsOk(), cmt.IsOk() ? "" : cmt.Detail);
    }

    // 6) Cleanup (best-effort).
    ArchiveIssue(apiUrl, apiKey, createdIdentifier);

    std::printf("\n=== Linear live smoke: %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
