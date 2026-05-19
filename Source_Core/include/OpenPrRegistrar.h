// OpenPrRegistrar.h — phase-1 of the `coderabbit-react-loop` plan
// (docs/design/coderabbit-react-loop.md). The registrar enumerates every
// open PR on a configured base branch via `gh pr list --json …` and
// upserts a row per discovered PR into `agent_open_pr_watch`. Future
// phases (the comment classifier + check-run watcher) iterate that table
// to decide which PRs to poll on the same scheduled-poll worker.
//
// Design — Tick()-only, no owned thread. Mirrors the shipped
// `PrCommentWatcher` shape (see Source_Core/include/PrCommentWatcher.h:18-28).
// The scheduled-poll worker calls `Tick()` once per iteration; the
// registrar owns no cancel atomic + no condition variable. A separate
// thread would add cost (sleep, cancel, restart) without changing the
// wake cadence.
//
// Subprocess seam — `OpenPrLister` is a `std::function` injection point so
// the doctest rig substitutes a fake without touching the real `gh`
// binary. Production binds to `OpenPrRegistrar::RunGhPrList` which calls
// `SubprocessCapture::Run` against a 30-second-bounded `gh pr list`
// invocation. The fake is the only seam the test uses; we do NOT spawn
// real subprocesses in unit tests (per the H1 SubprocessCapture test
// rig — real binaries belong to bucket-B scenarios).
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC.

#ifndef SMATCHET_OPEN_PR_REGISTRAR_H
#define SMATCHET_OPEN_PR_REGISTRAR_H

#if defined(SMATCHET_WITH_AGENTIC)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class AgentProposalStore;

namespace smatchet {
namespace agentic {

// One row from `gh pr list --json number,headRefName,headRefOid,url`.
struct OpenPrListing {
    int number = 0;
    std::string headRefName;
    std::string headSha;
    std::string url;
};

// Seam — production binds to a SubprocessCapture invocation of `gh pr list ...`.
// Returns false + outError on subprocess / parse failure; the registrar logs
// + skips that tick. Tests inject a fake that returns canned listings.
using OpenPrLister =
    std::function<bool(std::vector<OpenPrListing>& outListings, std::string& outError)>;

class OpenPrRegistrar {
  public:
    explicit OpenPrRegistrar(AgentProposalStore* store, std::string watchedBaseBranch = "develop");
    ~OpenPrRegistrar();

    OpenPrRegistrar(const OpenPrRegistrar&) = delete;
    OpenPrRegistrar& operator=(const OpenPrRegistrar&) = delete;

    void SetOpenPrLister(OpenPrLister lister);
    void SetWatchedBaseBranch(const std::string& branch);
    std::string GetWatchedBaseBranch() const;

    /// One iteration. Runs the lister, upserts each returned PR into
    /// agent_open_pr_watch. Returns the number of rows upserted (informational
    /// — not a strict success metric; subprocess + DB failures LOG_WARN +
    /// return what landed). Safe to call from the scheduled-poll worker.
    int Tick();

    /// Production binding for `OpenPrLister` — runs `gh pr list --base <branch>
    /// --state open --json number,headRefName,headRefOid,url` via
    /// SubprocessCapture. Exposed for direct wiring in AppController and for
    /// callers that want one-shot listings without instantiating a Registrar.
    static bool RunGhPrList(const std::string& baseBranch,
                            std::vector<OpenPrListing>& outListings,
                            std::string& outError);

    /// Parse the JSON array `gh pr list --json …` returns into OpenPrListing
    /// rows. Exposed for tests.
    static bool ParseGhPrListOutput(const std::string& json,
                                    std::vector<OpenPrListing>& outListings,
                                    std::string& outError);

  private:
    AgentProposalStore* store_;
    OpenPrLister lister_;
    std::string watchedBaseBranch_;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
#endif // SMATCHET_OPEN_PR_REGISTRAR_H
