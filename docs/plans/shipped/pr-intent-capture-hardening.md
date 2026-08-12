# Plan — PR intent capture hardening (observability + blocking gate + cross-harness)

> **Slug**: `pr-intent-capture-hardening` (matches this file's basename without `.md`).
>
> **Status**: `shipped`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

`docs/plans/shipped/pr-intent-capture.md` shipped the `## Intent` capture feature (PR #1260), but a review of the 11 PRs that have used it surfaced three real gaps, plus two scope re-decisions the parent plan deferred:

- **No proof the hook fires.** 2 of 11 filled Intents are explicitly hand-synthesised and the other 9 are indeterminate — a filled `## Intent` is indistinguishable from a hand-filled one, and nothing confirms the `UserPromptSubmit` hook has ever run in a live session.
- **Branch-resolution footgun.** `capture-intent.sh:55` runs `git rev-parse --abbrev-ref HEAD || echo _unknown`; on an *unborn* branch `rev-parse` prints `HEAD` to stdout **and** exits non-zero, so both outputs concatenate → `HEAD-_unknown.log`.
- **No orphan-bats guard.** The parent plan's own 10th-file deviation (a `.bats` suite that almost shipped un-run because no `test-*.sh` wrapper discovered it) has no mechanical gate — the parent plan explicitly backlogged one.
- **Advisory gate never graduated** (parent non-goal) and **emails are preserved** into public PR bodies (parent decision) — both re-opened here.

Intended outcome — **after this lands, the Intent gate is merge-blocking with a documented override, the capture hook is self-identifying + wiring-verified, emails never reach a public PR Intent, the cross-harness fallback is formalised, and an orphan bats suite can never merge un-run.**

## Approach

A forward-only hardening slice layered on the shipped 10-file feature. All seven review items are in scope (user decision). The risk profile is identical to the parent plan: **no `Source/Core/` touched** — the diff is CI workflow, agent-rule docs, harness templates, a Python redactor, a bash hook, bats tests, `merge-gates.sh`, and an ADR.

The one non-obvious trade-off, recorded in **ADR-0022**: the gate is promoted to blocking via the `merge-gates.sh` **meant-to-block allow-list** (the `Coverage`/`Sanitizer` mechanism) rather than `project.config.json` `branch_protection.required_contexts`. The allow-list path is reversible and avoids the merge-queue deadlock class (`merge-gates.md:83`: a branch-protection required check that never reports on `merge_group` deadlocks the queue forever). The fresh-worktree / non-Claude-Code fallback flows are covered by a new `intent-out-of-band` override label mirroring `perf-out-of-band`.

## Files to modify

**Capture path (redactor + hook)**
1. `agents/scripts/core/redact-intent.py` — (#7) mask emails → `[REDACTED-EMAIL]`; add email-redaction cases to the `--selftest` set (bumps the 58-case count). Reverses the parent's "emails preserved" decision (see § Deviations once shipped).
2. `docs/harness/claude-code/hooks/capture-intent.sh:55` — (#2) replace the branch resolver with `git -C "$PROJ_DIR" symbolic-ref --short HEAD` (correct on unborn branches, fails cleanly to `_unknown` on detached HEAD). `:~59` (#1) write a `# capture-intent v1 <ISO8601>` provenance header when first creating the file. `:61` (#4) cap-on-append: trim to the header line + last ~200 `- ` bullets on each write.
3. `agents/scripts/core/setup-harness.sh:~249` — (#1) wiring check: after `copy_template` of `capture-intent.sh`, assert it is registered in the deployed `settings.json` `UserPromptSubmit` event AND a `python3`/`python`/`py` interpreter resolves; surface a single SessionStart-visible line if not (capability doctor, not a per-line proof).

**Tests**
4. `tests/bats/capture_intent.bats` — flip test 23 (email is now **redacted**, not preserved); add cases for: symbolic-ref branch naming (unborn-branch no longer concatenates), provenance header present + well-formed, cap-on-append trims bullets but **keeps** the header, and the orchestrator-side `- `-line filter ignoring the header. Enrolled via the existing `agents/scripts/core/test-capture-intent-bats.sh` (no wrapper change — and #3 now enforces that).

**Gate promotion (#5)**
5. `.github/workflows/doc-validation.yml:365` — rename job `Intent section (advisory)` → `Intent section` (the allow-list excludes any name containing `advisory`); change both the missing-section and empty-section branches from `sys.exit(0)` to `sys.exit(1)` (block) while keeping the annotation. Stays `pull_request`-only — no `merge_group` trigger needed because promotion rides the poller allow-list, not branch protection.
6. `agents/scripts/core/merge-gates.sh:132` — append `|Intent section` to `MERGE_GATES_BLOCK_ALLOWLIST_RE` (single source of truth; `safe-admin-merge.sh` + `postmortem-owed.sh` inherit it). Add `intent-out-of-band` downgrade handling mirroring `perf-out-of-band` (`$downgraded` jq at `:428` + the label flag plumbing).
7. `docs/agent-rules/merge-gates.md:10,44` — document `Intent section` on the allow-list and the new `intent-out-of-band` override label (FAIL→WARN, scoped to the Intent gate only), under the same never-pre-apply hygiene as the other `*-out-of-band` labels (`:48`).
8. `tests/bats/merge_gates.bats` (+ `tests/fixtures/merge_gates_*.json`) — add fixtures/cases: a red `Intent section` check blocks; `intent-out-of-band` downgrades it to WARN. Copy the shape of `tests/fixtures/merge_gates_label_perf_oob.json`.
9. `project.config.json` — **deliberate non-edit** (recorded here so a reviewer doesn't ask): the gate is promoted via the allow-list, NOT added to `branch_protection.required_contexts` / `ci.required_checks`, to avoid the `merge_group` deadlock class. Mirrors the parent plan's "Deliberately NOT in project.config.json § branch_protection."

**Orphan-bats gate (#3)**
10. `scripts/dev/test-docs.sh` — new **blocking** sub-step (test-docs is itself merge-blocking): assert every `tests/bats/*.bats` is referenced by at least one `test-*.sh` wrapper; exit non-zero on an orphan. Closes the exact gap that nearly shipped the parent suite un-run.

**Cross-harness fallback (#6)**
11. `docs/agent-rules/ship-loops.md:74` — make the Intent-fill fallback **explicitly mandatory** for non-Claude-Code harnesses (Codex/Cursor/pi/Generic have no prompt-submit hook); note the orchestrator reads only `- `-prefixed lines (skips the `# capture-intent` header).
12. `docs/harness/capability-adapter.md` — add a `prompt-intent-capture` capability row: Claude Code = `UserPromptSubmit` hook; Codex/Cursor/pi/Generic = orchestrator fallback. Add the matching Harness-notes line.

**ADR**
13. `docs/adr/0022-intent-gate-promotion.md` (**new**) — record the allow-list-over-required_contexts promotion decision (#5).

*(Process, not a code file: write this plan to `active/`, then `bash agents/scripts/core/test-plan-index.sh --fix`.)*

## Existing utilities reused

- `MERGE_GATES_BLOCK_ALLOWLIST_RE` — `agents/scripts/core/merge-gates.sh:132`, the single-source allow-list constant; one alternation added, every consumer (`safe-admin-merge.sh`, `postmortem-owed.sh`) follows.
- `$downgraded` label-override jq — `agents/scripts/core/merge-gates.sh:428` — mirror the `perf-out-of-band` branch for `intent-out-of-band`.
- `tests/fixtures/merge_gates_label_perf_oob.json` — fixture template to copy for the `intent-out-of-band` case.
- `test-oob-label-impl` (doc-validation step) — already enforces that every documented `*-out-of-band` label is implemented in `merge-gates.sh` (6 today → 7); this gate mechanically couples files #6 (impl) and #7 (doc) so they must land together.
- `agents/scripts/core/test-capture-intent-bats.sh` — already enrolls the bats suite in `test-all.sh` + CI; reused unchanged.
- `redact-intent.py --selftest` — in-place selftest harness extended for the email-redaction cases.
- `docs/harness/claude-code/hooks/clear-tree-dirty.sh` — the reference hook pattern `capture-intent.sh` already mirrors (jq-with-sed fallback, `CLAUDE_PROJECT_DIR`, exit 0).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — no `Source/Core/` / UI-thread code; the hook runs out-of-process on prompt submit.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: unaffected — same reason; no sync I/O reaches `ImGui::*`.
- **Pillar 3 (never crash)**: product-binary stability unaffected. Hook robustness keeps the parent's fail-safe (errors/absent-python → write nothing, exit 0); the cap-on-append trim is best-effort and never blocks prompt submission.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

N/A — the diff touches only CI workflow, agent-rule docs, harness templates, a Python redactor, a bash hook, bats tests/fixtures, `merge-gates.sh`, `scripts/dev/test-docs.sh`, and an ADR. No `Source/Core/` / `project.config.json` `lint.zones` / `perf` paths. All five perf-system gates (PR-fast CI, Pillar-2 scanner, dispatcher drain, bucket-E cue, marker inventory) are **N/A**.

## Risks / non-goals

- **Risk — blocking gate strands a legitimate fallback flow** (fresh worktree provisioned before the hook, or a non-Claude-Code harness that didn't hand-fill). Mitigation: the `intent-out-of-band` override label (FAIL→WARN), documented with the same never-pre-apply hygiene as the other `*-out-of-band` labels.
- **Risk — renaming the job drops the `(advisory)` exclusion incorrectly.** The allow-list filter is `name matches RE AND name !contains "advisory"`. Renaming to `Intent section` + adding `|Intent section` to the RE is the matched pair; a bats case asserts the red check now blocks.
- **Risk — email redaction over-redacts a legitimate Intent.** Accepted — over-redaction is the parent plan's stated safe direction, and the destination is a *public* PR body where third-party PII must not leak.
- **Risk — cap-on-append drops early prompts** on extremely chatty branches. Accepted — the orchestrator synthesises a single line from the accumulated set, and recent prompts track the final diff better than the first exploratory ones; N≈200 means realistic sessions never hit it. The trim must preserve the header (test-asserted).
- **Non-goal — GitHub branch-protection required_contexts.** Chosen allow-list instead (ADR-0022); revisit only if a merge queue is adopted *and* the job is wired to report on `merge_group`.
- **Non-goal — Codex SessionStart pseudo-capture.** Infeasible — `codex/hooks-equivalent.md` documents the payload-dependent-hook gap (SessionStart has no prompt payload).
- **Non-goal — structured multi-prompt Intent rendering.** Unchanged parent non-goal; the orchestrator still synthesises one line.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no `Source/Core/` helper added; the redactor is Python, tested via selftest + bats.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario / screenshot / sanitizer**: `python3 agents/scripts/core/redact-intent.py --selftest` (extended with email-redaction cases); `bash agents/scripts/core/test-capture-intent-bats.sh` (branch-fix, provenance-header, cap-on-append, email-redaction-flip cases); `bash tests/bats/merge_gates.bats` (Intent-blocks + `intent-out-of-band`-downgrades cases).
- **Build gate**: N/A — no C++ touched; `cmake --build` not exercised.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `bash scripts/dev/test-docs.sh` green, **including the new orphan-bats sub-step** + plan-index + ref-integrity + `test-required-context-parity` (the renamed `Intent section` job is allow-listed, NOT a required context, so parity is unaffected — confirm).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: **DONE this session** — 7 decisions resolved one-at-a-time against `merge-gates.md`, `capability-adapter.md`, the harness `hooks-equivalent.md` docs, and the capture substrate (`capture-intent.sh:61` / `ship-loops.md:78`); produced ADR-0022; no `docs/CONTEXT.md` change (Intent capture is process plumbing, not a domain term, per SMATCHET-NOTES glossary scope).
- **Manual residue**: none — redactor + hook fully bats/selftest-driven; the live `UserPromptSubmit` fire is covered by the synthetic-JSON end-to-end hook test + the new setup-harness wiring check.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **GitHub merge-queue / branch-protection promotion** — follow-up only if a merge queue is adopted; needs `merge_group` reporting first.
- **Per-line capture telemetry** (counts, timing) — the provenance header + wiring check are the agreed observability floor; richer metrics are a follow-up if the hook's live-fire still proves hard to confirm.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
