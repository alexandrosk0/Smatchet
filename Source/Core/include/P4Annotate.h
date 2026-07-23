#ifndef P4_ANNOTATE_H
#define P4_ANNOTATE_H

#include "ConfigManager.h"
#include "SmatchetResult.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct P4LineAnnotate {
    std::string Changelist;
    std::string User;
    std::string Date;
    bool Approximate = false;
    std::string Error;
    /** Source line text from annotate when available (for Jira comment / AI export). */
    std::string LineSnippet;
};

struct P4AnnotatedLine {
    int SourceLine = 0;
    std::string Changelist;
    std::string User;
    /** Filled after annotate via `p4 describe` cache (YYYY/MM/DD when parseable). */
    std::string Date;
    std::string Code;
};

struct P4ChangelistDetails {
    std::string Author;
    /** Parsed from describe header (e.g. change date string). */
    std::string Date;
    std::string Description;
    bool Loaded = false;
    std::string Error;
};

/** One submitted changelist from `p4 changes -u <user>` (summary line, not full describe). */
struct P4ChangeSummary {
    std::string Changelist;
    /** As printed by p4: YYYY/MM/DD. */
    std::string Date;
    /** Domain/client stripped (`alice@ws` -> `alice`). */
    std::string User;
    /** Truncated single-quote description from the changes line. */
    std::string Description;
};

/**
 * Run `p4` with given arguments (executable from config). Uses process environment (P4PORT, etc.).
 * Returns false if spawn fails; stderr may contain p4 messages.
 */
bool P4RunCommand(const AnnotateAnalysisConfig& cfg, const std::vector<std::string>& args, int& outExitCode,
                  std::string& outStdout, std::string& outStderr);

/** Annotate a single 1-based source line; may set Approximate on fallback. */
P4LineAnnotate P4AnnotateLine(const AnnotateAnalysisConfig& cfg, const std::string& depotOrPath, int oneBasedLine,
                              const std::string& atChangelist);

/**
 * Full file annotate for detail view (1-based line indices in output).
 * Ok(rows) on success — an empty vector is a valid success (empty file);
 * Err(reason) on an empty path, spawn failure, or non-zero p4 exit.
 */
Result<std::vector<P4AnnotatedLine>> P4AnnotateFile(const AnnotateAnalysisConfig& cfg, const std::string& depotOrPath,
                                                    const std::string& atChangelist);

/**
 * Oldest submitted changelist whose submit time falls on [year/month/day, next day),
 * via `p4 changes -r -m 1 -s submitted //...@yyyy/mm/dd,yyyy/mm/dd`. Uses the server's
 * calendar interpretation for the date range (same as other Perforce date rev specs).
 * Ok(changelist) on success; Err(reason) on a bad date, spawn/exit failure, or no match.
 */
Result<std::string> P4FirstSubmittedChangelistOnCalendarDay(const AnnotateAnalysisConfig& cfg, int year, int month,
                                                            int day);

/**
 * Most recent submitted changelists by a user, newest first, via
 * `p4 changes -u <user> -m <maxN> -s submitted`. Blocking (call off the UI thread).
 * Err(reason) on spawn/exit failure; Ok with an empty vector means the user has no
 * visible submitted changes.
 */
Result<std::vector<P4ChangeSummary>> P4ChangesForUser(const AnnotateAnalysisConfig& cfg, const std::string& p4User,
                                                      int maxN);

/**
 * Resolve a Perforce login from an email by matching the `<email>` column of
 * `p4 users` (case-insensitive). The p4 login often differs from the email
 * local-part (e.g. login "alexk" for "alexkonstantonis@gmail.com"), so the naive
 * local-part strip in Vcs::P4UserFromEmail misses real changes. Returns the login
 * on a match, or "" if the email is empty, p4 fails, or no user matches. Blocking
 * (call off the UI thread).
 */
std::string P4UserForEmail(const AnnotateAnalysisConfig& cfg, const std::string& email);

/** LRU-ish cache for `p4 describe -s` (bounded by maxEntries). Thread-safe. */
class P4ChangelistDescribeCache {
  public:
    explicit P4ChangelistDescribeCache(int maxEntries = 512);

    /** Returns cached or empty with Loaded=false if missing; caller may call Store. */
    P4ChangelistDetails Get(const std::string& changelist) const;

    void Store(const std::string& changelist, P4ChangelistDetails d);

    /** Fetch via p4 describe -s if not cached (blocking). */
    P4ChangelistDetails GetOrFetch(const AnnotateAnalysisConfig& cfg, const std::string& changelist);

  private:
    void Touch(const std::string& cl);
    void EvictIfNeeded();

    int maxEntries_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, P4ChangelistDetails> map_;
    std::vector<std::string> lru_;
};

#endif
