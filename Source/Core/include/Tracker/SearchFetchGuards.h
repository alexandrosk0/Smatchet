#pragma once

// SearchFetchGuards — the per-backend ceilings on one paginated issue search (Quality Pillar 3: a
// pathological query must not exhaust memory). One copy, shared by the Jira / Plane / GitHub page
// loops and their test, so the test checks the values the loops actually use. Each page is also
// bounded by json_safe::kDefaultMaxBytes (Json/BoundedJsonParse.h). A loop that stops on any of
// these sets a soft warning on its fetch summary, so a cut-short result never reads as complete.

#include "TunableLimits.h"

#include <chrono>
#include <cstddef>

namespace smatchet {
namespace search_guards {

/// Rows asked for per page by every backend (server protocol; not user-tunable).
constexpr int kPageSize = 100;

// The ceilings below are user-tunable (Config/TunableLimits.h, config.json "limits"); each call reads the
// currently published value, so callers snapshot it once before their page loop.

inline int JiraMaxPages() { return limits::Get().JiraSearchMaxPages; }
inline std::size_t JiraMaxTotalFetchBytes() { return limits::MiB(limits::Get().JiraSearchMaxFetchMiB); }
/// Soft cap on accumulated rows; a well-behaved server may reach it before the page cap, so a
/// truncated result carries a warning.
inline std::size_t JiraMaxResultCount() { return static_cast<std::size_t>(limits::Get().JiraSearchMaxResults); }

inline int PlaneMaxPages() { return limits::Get().PlaneSearchMaxPages; }
inline std::size_t PlaneMaxTotalFetchBytes() { return limits::MiB(limits::Get().PlaneSearchMaxFetchMiB); }
inline std::size_t PlaneMaxResultCount() { return static_cast<std::size_t>(limits::Get().PlaneSearchMaxResults); }

/// GitHub search returns at most 1000 items (10 pages of 100), the REST search API's own limit
/// (so the page-cap setting is clamped to 10 in TunableLimits.h).
inline int GitHubMaxPages() { return limits::Get().GitHubSearchMaxPages; }
inline std::size_t GitHubMaxTotalFetchBytes() { return limits::MiB(limits::Get().GitHubSearchMaxFetchMiB); }
inline std::size_t GitHubMaxResultCount() { return static_cast<std::size_t>(limits::Get().GitHubSearchMaxResults); }

/// Background membership probes (Jira keys-only reconcile, Jira/Plane changed-since) walk a whole view or
/// update window but must never act on a truncated list, so they get a looser ceiling than the interactive
/// search above; a probe that still hits it reports a partial walk and the reconcile is skipped.
inline int ProbeMaxPages() { return limits::Get().MembershipProbeMaxPages; }
inline std::size_t ProbeMaxResultCount() { return static_cast<std::size_t>(limits::Get().MembershipProbeMaxResults); }
inline std::size_t ProbeMaxTotalFetchBytes() { return limits::MiB(limits::Get().MembershipProbeMaxFetchMiB); }

/// Page, result-count and byte ceilings handed to a page loop, so one loop serves both the interactive
/// search (per-backend caps above) and the background probes (ProbeCaps). A changed-since probe fetches
/// full rows, so its byte ceiling is raised alongside its row ceiling.
struct LoopCaps {
    int maxPages;
    std::size_t maxResultCount;
    std::size_t maxTotalFetchBytes;
};
inline LoopCaps JiraCaps() { return {JiraMaxPages(), JiraMaxResultCount(), JiraMaxTotalFetchBytes()}; }
inline LoopCaps PlaneCaps() { return {PlaneMaxPages(), PlaneMaxResultCount(), PlaneMaxTotalFetchBytes()}; }
inline LoopCaps ProbeCaps() { return {ProbeMaxPages(), ProbeMaxResultCount(), ProbeMaxTotalFetchBytes()}; }

/// Wall-clock budget for one whole paginated fetch. The per-request timeout bounds a single call; this
/// bounds the walk, so a huge query cannot hold a sync worker for pages x request-timeout.
class Deadline {
  public:
    explicit Deadline(int seconds) : end_(std::chrono::steady_clock::now() + std::chrono::seconds(seconds)) {}
    bool Expired() const { return std::chrono::steady_clock::now() >= end_; }

  private:
    std::chrono::steady_clock::time_point end_;
};

/// A deadline of limits::Get().SearchMaxTotalSeconds starting now.
inline Deadline StartSearchDeadline() { return Deadline(limits::Get().SearchMaxTotalSeconds); }

} // namespace search_guards
} // namespace smatchet
