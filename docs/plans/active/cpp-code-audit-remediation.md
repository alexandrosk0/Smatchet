# Plan — C++ code audit remediation (CPP_CODE_AUDIT.md)

> **Slug**: `cpp-code-audit-remediation`
>
> **Status**: `active`
>
> **Source**: [`CPP_CODE_AUDIT.md`](../../../CPP_CODE_AUDIT.md) — 33 findings (High: 1, Medium: 8, Low: 24), complementary to the already-remediated `SECURITY_AUDIT.md` sweep ([`cpp-security-hardening.md`](../cpp-security-hardening.md) — see that plan for the prior JSON-DoS/MCP-auth campaign). Audit PR: alexandrosk0/Smatchet#1586.

## Context

`CPP_CODE_AUDIT.md` landed as a docs-only PR (#1586) with no source changes — it's a 12-partition, ten-defect-angle sweep of the ~140K-LOC first-party C++ tree. Four findings were hand-verified against source (data-loss truncation, GitHub credential mis-routing, duration-sort infinite loop, AI cancel-atom rebind); the rest are auditor-reported with file:line citations. This plan tracks remediation across multiple slices/PRs since the finding set is too large for one PR under the per-PR file ceiling. **Backlog check (this pass):** grepped `backlog/BACKLOG_CODE_REVIEW.md`, `docs/self-improvement/categories/*`, and GitHub Issues for every Slice 1 finding's symbol/mechanism (`TicketFieldEditor_Modal` truncation, `RouteTrackerEnvCredentials`, `ParseDurationToSecondsForSort`) — no pre-existing entries found; these are new.

**Intended outcome**: every finding in `CPP_CODE_AUDIT.md` is either fixed (this plan's slices) or explicitly deferred with a one-line reason recorded in § Deviations.

## Approach

Remediate in slices ordered by the audit's own § Recommended remediation order (highest user-impact / hand-verified first), each shippable as its own PR:

- **Slice 1 (this PR)** — the top 3 hand-verified findings (#1 data-loss, #2 GitHub credential mis-routing, #3 duration-sort infinite loop) plus the two mechanical ParseBounded sweeps (#8 Jira field-catalog, #9 Plane) that are the same well-proven mechanical fix as the prior `cpp-security-hardening` campaign. Also folds in #19 (integer-overflow in the same `ParseDurationToSecondsForSort` function touched for #3 — same file, same function, trivial saturating-add addition, no reason to defer a second pass over identical lines).
- **Slice 2 (follow-up)** — #4 (AI cancel-atom rebind), #5 (TextMerge O(n·m) OOM), #6 (offline-replay latch leak) — the AI/offline-replay reliability cluster the audit groups together.
- **Slice 3 (follow-up)** — #7 (locale-override format-string on the `SmatchetLocalizedImGui` `Text*` sinks) — same specifier-validation mechanism the prior audit's finding #1 fix added to `SmatchetLocalization::Format`, applied to the `TranslateSource` wrapper path.
- **Slice 4 (follow-up)** — remaining Low findings #10–33, batched by subsystem (ParseBounded stragglers #10–13; SSRF/security #14–16; integer handling #17–18 — #19 already folds into Slice 1; memory safety #20–22; concurrency #23–27; resource management #28–31; error handling #32; logic cluster #33).

## Files to modify

### Slice 1 (this PR)

1. `Source/Core/src/TicketFieldEditor_Modal.cpp` — **#1**: added `SeedTruncated` + `BufferSeedShown` state, a `SeedLongTextBuffer` helper used at both the sync and async-worker seed sites, `CommitLongTextEdit` now diffs against `BufferSeedShown` (what was actually loaded) instead of the untruncated seed, plus a red "too large to edit" banner in `DrawLongTextBanners`.
2. `Source/Core/src/Config/ConfigManager.cpp` — **#2**: `RouteTrackerEnvCredentials` now compares `trackerTypeLower == "github"` (was raw `cfg.TrackerType == "github"` against the canonical PascalCase `"GitHub"`) for both the token and base-URL routing arms.
3. `Source/Core/src/TicketGridModel.cpp` — **#3**: the unrecognized-unit-char branch in `ParseDurationToSecondsForSort` now advances `pos` before continuing (was an infinite loop). **#19** (same function): added a `saturatingAccumulate` helper so the unit multiply/add can't overflow `long long` (UB) on a hostile duration string.
4. `Source/Core/src/Tracker/TrackerFieldCatalog.cpp` — **#8**: all 10 bare `nlohmann::json::parse` sites (components/priority/issuetype/status/createmeta/project/boards/sprint/field-list, per-project components) routed through `smatchet::json_safe::ParseBounded`.
5. `Source/Core/src/Tracker/PlaneIssueSearch.cpp`, `PlaneIssueMutation.cpp`, `PlaneFieldCatalog.cpp` — **#9**: the 9 cited bare-parse sites (states, error-detail extraction, work-items page, list-projects, PATCH/POST error-detail ×2, create-issue response, comments, custom-field catalog ×2) routed through `ParseBounded`. `PlaneIssueSearch.cpp:96` (`ExtractProjectFromPlaneQuery`, a locally-authored structured-query blob, not network ingress) is intentionally **not** touched — the audit doesn't cite it.

### Slice 2–4 (follow-up, not in this PR)

See `CPP_CODE_AUDIT.md` findings #4–7, #10–33 for file:line citations — not re-listed here to avoid drift; this plan's § Deviations records the per-slice PR link once shipped.

## Existing utilities reused

- `smatchet::json_safe::ParseBounded(text, errOut)` — `Source/Core/include/Json/BoundedJsonParse.h:122`. Same shared bounded-ingress parser the prior `cpp-security-hardening` campaign wired everywhere else; Slice 1 extends its reach to the two TUs the original sweep's dedup gate skipped.
- The `(std::min)` / parenthesized-`numeric_limits` idiom already used elsewhere in this file family for the Windows `min`/`max` macro guard.

## Extraction sizing

N/A — no file crosses a split threshold in this slice.

## UX Pillar callouts

- **Pillar 1 (perf)**: negligible. `ParseBounded` drives the same SAX-based DOM builder as `nlohmann::json::parse`; the `SeedLongTextBuffer` helper is a single extra `memcpy`-sized copy already happening. `ParseDurationToSecondsForSort` gains one branch + one lambda call per unit token — sort comparator, not a steady-state per-frame path.
- **Pillar 2 (no UI-thread block > 100 ms)**: no new sync I/O — the long-text seed/commit and duration-sort fixes are pure in-memory logic; the Plane/Jira catalog parses already ran on the same thread as the unbounded parse they replace.
- **Pillar 3 (never crash)**: this slice is squarely Pillar 3 — #3 fixes an actual infinite loop (permanent UI freeze, worse than a crash), #8/#9 convert two more uncatchable depth-bomb stack-overflow sites into clean rejections, #1 fixes silent data corruption (not a crash, but the audit's highest-severity finding).
- **Pillar 4 (accessibility)**: no impact.

## Perf-review-system gates (diff touches `Source/Core/`)

1. **PR-fast CI** — no scenario directly exercises the long-text modal or the Jira/Plane catalog fetch in the curated PR-fast set; this is logic-correctness, not a perf-sensitive path. N/A.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — untouched.
4. **Visible-cue bucket-E harness** — no new >100 ms sync-stall path (the fixes *remove* an infinite-loop stall).
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: local build/CI is unavailable in this environment (`FetchContent` 403s through the proxy — same constraint recorded in `cpp-security-hardening.md` § Implementation log); CI is the build/test gate for this PR.

## Risks / non-goals

- **Risk — `TicketFieldEditor_Modal.cpp` fix changes Save semantics for over-64KB docs.** Mitigation: the fix is diff-baseline-only (compare against what was actually loaded, not the untruncated seed) plus a visible banner — it does not attempt full dynamic buffer growth (which would risk a dangling-pointer bug in the dictation-router registration lifecycle across the async reseed path); an unmodified over-limit doc is now a no-op Save instead of silent corruption, and an edited one is capped + visibly flagged rather than silently truncated.
- **Risk — Plane/Jira `ParseBounded` conversion changes error-detail extraction behavior for malformed error bodies.** Mitigation: mechanical 1:1 replacement of `is_discarded()`/`try-catch` control flow with `parseErr.empty()` checks — same branches, same fallback-to-raw-text behavior, just non-throwing instead of exception-based.
- **Non-goal**: this PR does not touch #4–7 or the Low findings — those are Slice 2–4, tracked here and shipped as follow-up PRs.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no local build available this session (see § Perf-review-system gates); CI's existing ctest suite covers `TicketGridModel`/`ConfigManager`/`Tracker` if fixtures exist, otherwise this is a residue item (see below).
- **Build gate**: CI-only this session — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` deferred to CI.
- **Doc validation**: `scripts/dev/test-docs.sh` — run locally where possible.
- **Plan stress-test — `grill-with-docs`**: not run interactively this session (autonomous single-turn task, no user available mid-turn); flagging as manual residue below rather than skipping silently.
- **Manual residue**: (1) no local ctest run — CI is the authoritative gate this session, consistent with the `cpp-security-hardening` precedent; (2) `grill-with-docs` stress-test not run — both logged here per `AGENTS.md` § Process rules "no silent residue."

## Out of scope (flagged, not designed)

- Slices 2–4 (findings #4–33) — deferred to follow-up PRs, tracked in § Approach above.
- `agents/scripts/project/*` lint-rule changes — this PR doesn't graduate any WARN-first gate (unlike `cpp-security-hardening` Slice 6); N/A.

## Implementation log

- Slice 1 (this PR, branch `claude/pr-1586-findings-697xkc`): #1, #2, #3, #8, #9, #19 — see § Files to modify for the per-file summary.

## Deviations from plan

- #19 folded into Slice 1 opportunistically (same function as #3) rather than deferred to Slice 4 with the other Low integer-handling findings — avoids a second pass over the exact same lines.

## Verification (actual)

- Not run — local build unavailable (`FetchContent` 403); CI is the gate for this PR. See § Manual residue.

## Out of scope — deferral residue-sweep

Grepped `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray references to findings #4–33 or this plan's slug — none found (this is a new plan with no prior deferred-symbol footprint to clear).
