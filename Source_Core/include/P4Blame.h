#ifndef P4_BLAME_H
#define P4_BLAME_H

#include "ConfigManager.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct P4LineBlame {
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

/**
 * Run `p4` with given arguments (executable from config). Uses process environment (P4PORT, etc.).
 * Returns false if spawn fails; stderr may contain p4 messages.
 */
bool P4RunCommand(const BlameAnalysisConfig& cfg, const std::vector<std::string>& args, int& outExitCode,
                  std::string& outStdout, std::string& outStderr);

/** Blame a single 1-based source line; may set Approximate on fallback. */
P4LineBlame P4BlameLine(const BlameAnalysisConfig& cfg, const std::string& depotOrPath, int oneBasedLine,
                        const std::string& atChangelist);

/** Full file annotate for detail view (1-based line indices in output). */
std::vector<P4AnnotatedLine> P4AnnotateFile(const BlameAnalysisConfig& cfg, const std::string& depotOrPath,
                                            const std::string& atChangelist, std::string& outError);

/** LRU-ish cache for `p4 describe -s` (bounded by maxEntries). Thread-safe. */
class P4ChangelistDescribeCache {
  public:
    explicit P4ChangelistDescribeCache(int maxEntries = 512);

    /** Returns cached or empty with Loaded=false if missing; caller may call Store. */
    P4ChangelistDetails Get(const std::string& changelist) const;

    void Store(const std::string& changelist, P4ChangelistDetails d);

    /** Fetch via p4 describe -s if not cached (blocking). */
    P4ChangelistDetails GetOrFetch(const BlameAnalysisConfig& cfg, const std::string& changelist);

  private:
    void Touch(const std::string& cl);
    void EvictIfNeeded();

    int maxEntries_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, P4ChangelistDetails> map_;
    std::vector<std::string> lru_;
};

#endif






