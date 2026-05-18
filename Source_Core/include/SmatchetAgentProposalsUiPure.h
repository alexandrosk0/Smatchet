#ifndef SMATCHET_AGENT_PROPOSALS_UI_PURE_H
#define SMATCHET_AGENT_PROPOSALS_UI_PURE_H

#include <cstddef>
#include <string>

/**
 * Pure-logic helpers lifted from SmatchetAgentProposalsUi.cpp's anonymous namespace so the
 * preview-truncation rule can be unit-tested without ImGui / AppController. Implementations
 * are byte-identical to the originals; only namespace + linkage change.
 *
 * Pure C++14; no third-party deps. Stdlib only.
 *
 * Owner: SmatchetAgentProposalsUi (UI) — pure helpers live here.
 * Test surface: tests/Source_Core/SmatchetAgentProposalsUiPure.test.cpp.
 */
namespace SmatchetAgentProposalsUiPure {

/// Max bytes of rationale text shown inline before the user clicks "[expand]".
/// 200 bytes ≈ first sentence — enough for at-a-glance scan without dominating the row.
constexpr std::size_t kRationalePreviewBytes = 200;

/**
 * Returns `s` unchanged if its byte length is at or below `kRationalePreviewBytes`.
 * Otherwise truncates to the previous UTF-8 codepoint boundary at or before that cutoff
 * and appends `"..."`. Never splits a multi-byte UTF-8 sequence.
 *
 * - ASCII-only input → exact byte cut at `kRationalePreviewBytes`.
 * - UTF-8 input where the cutoff lands inside a multi-byte sequence → back off to the
 *   start byte of that sequence; the truncated string ends at a valid codepoint boundary.
 * - Empty input → returns empty unchanged.
 */
std::string TruncatePreview(const std::string& s);

} // namespace SmatchetAgentProposalsUiPure

#endif
