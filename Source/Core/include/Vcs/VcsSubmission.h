#ifndef SMATCHET_VCS_SUBMISSION_H
#define SMATCHET_VCS_SUBMISSION_H

#include "P4Annotate.h"

#include <string>
#include <vector>

/**
 * Source-tagged VCS submission rows for the User Info window's unified feed
 * (docs/plans/active/user-info-window.md, Slice 1). Pure logic — no I/O, no
 * process spawn, no network. Test surface: tests/Core/VcsSubmission.test.cpp.
 */
namespace Vcs {

enum class VcsSource { P4, Git };

/** Common shape for one submission from either VCS source. */
struct VcsSubmission {
    VcsSource Source = VcsSource::P4;
    /** Changelist number (p4) or commit SHA (git). */
    std::string Id;
    /**
     * ISO-8601, normalised at the adapter boundary so the leading 10 chars are
     * YYYY-MM-DD whenever the source provided a parseable date. May be shorter
     * when the source date was missing/unparseable (see KeepOnOrAfterCutoff).
     */
    std::string Timestamp;
    std::string Author;
    /** First line of the change description / commit message. */
    std::string FirstLine;
    /** Browse URL when the source has one (git html_url); empty for p4. */
    std::string Url;
};

/**
 * Derive the p4 username from a tracker email by taking the local-part
 * (`alice@corp.io` -> `alice`). No `@` returns the input unchanged; empty
 * input returns empty (spec 6.1, identity edge-case §11).
 */
std::string P4UserFromEmail(const std::string& email);

/**
 * Adapt one `p4 changes -u` summary row to the common shape. Normalises the
 * p4 date `YYYY/MM/DD` to ISO `YYYY-MM-DD`; a date not in that shape is
 * passed through unchanged (the cutoff's short-timestamp rule then applies).
 */
VcsSubmission FromP4Change(const P4ChangeSummary& change);

/**
 * Day-window cutoff via string compare on the leading YYYY-MM-DD (spec 7.2).
 * A timestamp shorter than 10 chars is kept unconditionally — never silently
 * drop a row whose source date was missing or unparseable.
 */
bool KeepOnOrAfterCutoff(const std::string& timestamp, const std::string& cutoffIsoDate);

/**
 * Merge two feeds (each already newest-first) into one unified newest-first
 * feed. Stable on timestamp ties: rows from `a` precede rows from `b`, and
 * within each input the original order is preserved.
 */
std::vector<VcsSubmission> MergeFeedsNewestFirst(const std::vector<VcsSubmission>& a,
                                                 const std::vector<VcsSubmission>& b);

} // namespace Vcs

#endif
