// OpenPrRegistrar — phase-1 of the `coderabbit-react-loop` plan.
//
// One `Tick()` call per scheduled-poll iteration; the registrar itself owns
// no thread. See OpenPrRegistrar.h for the design rationale.

#include "OpenPrRegistrar.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposalStore.h"
#include "Logger.h"
#include "SubprocessCapture.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace agentic {

OpenPrRegistrar::OpenPrRegistrar(AgentProposalStore* store, std::string watchedBaseBranch)
    : store_(store), watchedBaseBranch_(std::move(watchedBaseBranch)) {
    if (watchedBaseBranch_.empty()) {
        // Defensive: never leave the branch empty — an empty `--base ""` arg
        // would broaden the listing to every open PR on the repo, which is
        // both noisier than intended and unsupported by phase-1's scope.
        watchedBaseBranch_ = "develop";
    }
}

OpenPrRegistrar::~OpenPrRegistrar() = default;

void OpenPrRegistrar::SetOpenPrLister(OpenPrLister lister) { lister_ = std::move(lister); }

void OpenPrRegistrar::SetWatchedBaseBranch(const std::string& branch) {
    if (branch.empty()) {
        LOG_WARN("OpenPrRegistrar::SetWatchedBaseBranch: empty branch rejected, keeping '%s'",
                 watchedBaseBranch_.c_str());
        return;
    }
    watchedBaseBranch_ = branch;
}

std::string OpenPrRegistrar::GetWatchedBaseBranch() const { return watchedBaseBranch_; }

int OpenPrRegistrar::Tick() {
    if (store_ == nullptr) {
        LOG_WARN("OpenPrRegistrar::Tick: store_ is null — registrar disabled this iteration");
        return 0;
    }
    if (!lister_) {
        LOG_DEBUG("OpenPrRegistrar::Tick: lister_ unwired — skipping iteration");
        return 0;
    }

    std::vector<OpenPrListing> listings;
    std::string listErr;
    if (!lister_(listings, listErr)) {
        LOG_WARN("OpenPrRegistrar::Tick: lister failed (base=%s): %s", watchedBaseBranch_.c_str(), listErr.c_str());
        return 0;
    }

    int upserted = 0;
    for (const auto& l : listings) {
        // Preserve cursor fields across re-upserts. The store's
        // SetOpenPrWatch uses `ON CONFLICT … DO UPDATE` so the cursor
        // columns are left untouched when the row already exists; we still
        // fetch the existing row first to populate the OpenPrWatchRow we
        // pass in (the columns are unused on the ON CONFLICT path but the
        // INSERT-side binds them on a fresh row, and a future Set variant
        // may need them — explicit is safer than implicit).
        AgentProposalStore::OpenPrWatchRow row;
        std::string getErr;
        if (!store_->GetOpenPrWatch(l.url, row, getErr)) {
            LOG_WARN("OpenPrRegistrar::Tick: GetOpenPrWatch(%s) failed: %s — proceeding with defaulted row",
                     l.url.c_str(), getErr.c_str());
            row = AgentProposalStore::OpenPrWatchRow();
        }
        row.prUrl = l.url;
        row.prNumber = l.number;
        row.headRefName = l.headRefName;
        row.headSha = l.headSha;
        // Cursor fields (lastSeenCommentIdStr, lastSeenCheckRunId,
        // iterationCount, lastPolledAtSec) carry through from the existing
        // row when present; defaulted on a fresh insert.

        std::string setErr;
        if (!store_->SetOpenPrWatch(row, setErr)) {
            LOG_WARN("OpenPrRegistrar::Tick: SetOpenPrWatch(%s) failed: %s — continuing", l.url.c_str(),
                     setErr.c_str());
            continue;
        }
        ++upserted;
    }

    LOG_INFO("OpenPrRegistrar::Tick: discovered %zu open PRs on base=%s, upserted %d rows",
             listings.size(), watchedBaseBranch_.c_str(), upserted);
    return upserted;
}

bool OpenPrRegistrar::RunGhPrList(const std::string& baseBranch, std::vector<OpenPrListing>& outListings,
                                  std::string& outError) {
    outListings.clear();
    outError.clear();

    SubprocessCapture::CaptureOptions opts;
    opts.argv0 = "gh";
    opts.args = {"pr", "list", "--base", baseBranch, "--state", "open", "--json",
                 "number,headRefName,headRefOid,url"};
    opts.timeoutMs = 30000;
    // 4 MiB is well above what `gh pr list` returns even for hundreds of
    // open PRs — guards against a runaway response without truncating
    // legitimate use. stderr cap stays at the SubprocessCapture default.
    opts.stdoutByteCap = 4u * 1024u * 1024u;

    SubprocessCapture::CaptureResult result;
    std::string spawnErr;
    if (!SubprocessCapture::Run(opts, result, spawnErr)) {
        outError = std::string("gh pr list spawn failed: ") + spawnErr;
        return false;
    }
    if (result.timedOut) {
        outError = "gh pr list timed out after 30s";
        return false;
    }
    if (result.exitCode != 0) {
        // Cap the stderr blob so a misconfigured `gh` does not flood the
        // log with kilobytes of token-expiry diagnostics. 512 bytes is the
        // same cap the H7 GitHub error path uses.
        const std::string trimmedStderr =
            result.stderrText.size() > 512 ? result.stderrText.substr(0, 512) : result.stderrText;
        outError = std::string("gh pr list failed exit=") + std::to_string(result.exitCode) + ": " + trimmedStderr;
        return false;
    }

    return ParseGhPrListOutput(result.stdoutText, outListings, outError);
}

bool OpenPrRegistrar::ParseGhPrListOutput(const std::string& json, std::vector<OpenPrListing>& outListings,
                                          std::string& outError) {
    outListings.clear();
    outError.clear();

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("ParseGhPrListOutput: ") + e.what();
        return false;
    }

    if (!root.is_array()) {
        outError = "ParseGhPrListOutput: top-level is not a JSON array";
        return false;
    }

    for (const auto& entry : root) {
        if (!entry.is_object()) {
            LOG_WARN("OpenPrRegistrar::ParseGhPrListOutput: skipping non-object entry");
            continue;
        }
        OpenPrListing l;
        bool missingField = false;
        // `number` — required.
        if (entry.contains("number") && entry["number"].is_number_integer()) {
            l.number = entry["number"].get<int>();
        } else {
            LOG_WARN("OpenPrRegistrar::ParseGhPrListOutput: entry missing/invalid `number` — skipping");
            missingField = true;
        }
        // `headRefName` — required.
        if (!missingField) {
            if (entry.contains("headRefName") && entry["headRefName"].is_string()) {
                l.headRefName = entry["headRefName"].get<std::string>();
            } else {
                LOG_WARN("OpenPrRegistrar::ParseGhPrListOutput: entry missing/invalid `headRefName` — skipping");
                missingField = true;
            }
        }
        // `headRefOid` — required (head sha).
        if (!missingField) {
            if (entry.contains("headRefOid") && entry["headRefOid"].is_string()) {
                l.headSha = entry["headRefOid"].get<std::string>();
            } else {
                LOG_WARN("OpenPrRegistrar::ParseGhPrListOutput: entry missing/invalid `headRefOid` — skipping");
                missingField = true;
            }
        }
        // `url` — required.
        if (!missingField) {
            if (entry.contains("url") && entry["url"].is_string()) {
                l.url = entry["url"].get<std::string>();
            } else {
                LOG_WARN("OpenPrRegistrar::ParseGhPrListOutput: entry missing/invalid `url` — skipping");
                missingField = true;
            }
        }
        if (missingField) {
            continue;
        }
        outListings.push_back(std::move(l));
    }

    return true;
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
