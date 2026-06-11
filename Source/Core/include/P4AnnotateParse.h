#ifndef SMATCHET_P4_ANNOTATE_PARSE_H
#define SMATCHET_P4_ANNOTATE_PARSE_H

#include "P4Annotate.h"

#include <string>
#include <vector>

/**
 * Pure-logic parsers lifted from P4Annotate.cpp's anonymous namespace so they can be unit-tested
 * without spawning `p4`. No process spawn, no I/O, no network, no SQLite. Implementations are
 * byte-identical to the originals (regex literals + control flow preserved); the only change
 * is namespace + linkage.
 *
 * Owner: `p4-annotate` subsystem. Test surface: tests/Core/P4AnnotateParse.test.cpp.
 */
namespace P4AnnotateParse {

/**
 * Split a `p4`-tool stdout blob into per-line strings. Trims trailing CR/LF and surrounding
 * ASCII whitespace via `TrimCopy`. Handles both LF and CRLF line endings (the CR is consumed
 * by trim, not the splitter). Preserves empty lines from consecutive newlines as trimmed-empty
 * entries. Trailing-newline-absent input still emits the final partial line.
 */
std::vector<std::string> SplitLines(const std::string& s);

/**
 * Strip a trailing `@domain` suffix from a Perforce user identifier in-place. `alice@corp.io`
 * becomes `alice`. No `@` is a no-op. Empty input is a no-op.
 */
void StripP4UserDomain(std::string& user);

/**
 * Parse `p4 annotate` text lines. Observed formats:
 * - rev user: code
 * - rev: user YYYY/MM/DD code   (common with -c -u on some servers)
 * - rev user YYYY/MM/DD code
 * - rev: code
 *
 * Output strings are cleared up-front so a partial / failed match never leaks earlier state.
 * `outCode` defaults to the raw line when no format matches and the function returns false.
 * Returns true when one of the four formats matched.
 */
bool ParseAnnotateTextLine(const std::string& line, std::string& outCl, std::string& outUser, std::string& outAnnotDate,
                           std::string& outCode);

/**
 * Parse the header line of `p4 changes -m 1` output to extract the latest changelist + user.
 * Format: `Change <N> on <date> by <user@client> ...`. Returns a `P4LineAnnotate` with the
 * fields populated and `Approximate = true`. On parse miss the returned annotate carries
 * `Error` set to `stderrText` (if non-empty) or `"p4 changes produced no match"`.
 *
 * Empty input → no match → `Error` populated, no UB.
 */
P4LineAnnotate ParseLatestChangeFromChangesOutput(const std::string& stdoutText, const std::string& stderrText);

/**
 * Parse multi-line `p4 changes -u <user>` stdout into per-change summaries. Expected line shape:
 * `Change <N> on <YYYY/MM/DD> by <user@client> '<truncated desc>'`. Lines that do not match are
 * skipped (p4 may interleave info lines). User has the `@client` suffix stripped; the description
 * is the content between the outermost single quotes (may be empty; trailing quote optional on
 * truncated output). Order preserved (p4 emits newest first).
 */
std::vector<P4ChangeSummary> ParseChangesForUserOutput(const std::string& stdoutText);

} // namespace P4AnnotateParse

#endif
