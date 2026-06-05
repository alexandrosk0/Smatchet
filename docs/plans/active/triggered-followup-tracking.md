# Plan — Triggered follow-up tracking (machine-checkable deferred follow-ups)

> **Slug**: `triggered-followup-tracking` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Some backlog items are **deferred follow-ups gated on a future condition** — "re-measure after ~10 PRs", "after ~20 PRs compute the FP rate, then flip the gate". Today these live as **prose** in a `Concrete next action` line in `docs/self-improvement/categories/*.md`; nothing machine-checks the condition, so the firing relies on the **manual** self-improvement triage cadence. The archetype is the 2026-06-05 entry filed by [#866](https://github.com/alexandrosk0/Smatchet/pull/866) (`process.md:10`): the `reduce-coderabbit-review-spend` plan says "re-measure after ~10 PRs" against baseline 3.2 heads/PR + 1.3 nudges/PR, but once the plan archives to `shipped/` nothing surfaces it — the measurement that *proves the levers worked* can silently never happen. A second live instance: the `dup_audit.py` WARN→block graduation ("after ~20 PRs, if FP rate < 10% ship the flip", `process.md:22`). Putting these in the flat backlog is the current solution and it is **not enough** — a flat list has no trigger.

Intended outcome — *after this lands, a deferred follow-up carries a machine-checkable trigger and FIRES automatically when its condition is met (N PRs merged / a date / a plan archived / a file aged), surfaced at session start exactly like the existing `memory-drain` / `postmortem-owed` nudges — not by manual triage.*

Originating request: user, this session (the #866 follow-up is the worked example). Process/tooling change → backlog-class per ADR-0014, not a product Issue.

## Approach

Reuse the existing **SessionStart conditional-nudge** pattern wholesale; build no parallel system, no JSON registry (only ~2 live entries warrant a trigger today — the registry is deferred until 5+ concurrent triggered entries exist). Four slices on one branch → one PR.

1. **The field + the doc/process (the core ask)** — add an optional, grep-parseable `Triggered-follow-up:` line to the self-improvement entry format in `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` § Format, plus a short § in `docs/agent-rules/process-rules.md` (the catch-all sink — **NOT** `AGENTS.md`, which is 154/150 lines, over the grandfathered cap) defining the lifecycle. One `when=` spec per entry, four token grammars:
   - `pr-count:base=develop;since=2026-06-05;n=10` — N squash-merged PRs to a base since a date.
   - `date:2026-07-01` — calendar deadline.
   - `plan-shipped:<slug>` — a named plan archived to `docs/plans/shipped/<slug>.md`.
   - `file-age:<path>;days=30` — a path untouched for N days.
   Plus `action=<one-line what-to-do>; baseline=<optional metric prose>; fired=never` (the unfired sentinel). Entries without the line are invisible to the nudge — fully backward-compatible with the ~30 existing entries.
2. **The evaluator/nudge script** — `agents/scripts/core/followup-due-nudge.sh`, cloned from `memory-drain-nudge.sh` (offline triggers) + `postmortem-owed.sh` (the network `pr-count` path, `--list`/`--nudge` modes, `cd` repo-root anchor, gh-unavailable→advisory-skip, `has_entry()`-style dedup). It greps the category files for `Triggered-follow-up:`, parses the `when=` token-spec, dispatches to one of four cheap evaluators (each returns FIRE / SKIP / UNAVAILABLE — UNAVAILABLE treated as SKIP-silently so it never false-fires or hard-fails), skips entries whose `fired=` is a real date, and emits a single `## === follow-up due (N) ===` block echoing each firing entry's `action` + `baseline` verbatim. **Read-only** — it never stamps `fired=` itself (preserves the "nudges emit to stdout, never write tracked files" invariant + dodges the concurrent-PR shared-file conflict the count-column removal killed). Ships a `--selftest` asserting each grammar parses + synthetic FIRE/SKIP/UNAVAILABLE.
3. **Wire + migrate the archetype** — add one SessionStart stanza (10s timeout — `pr-count` may call gh) to the canonical `docs/harness/claude-code/settings.json.tmpl`, regenerate via `setup-harness.sh` (do **NOT** hand-edit the drifted live `.claude/settings.json`). Migrate the #866 entry (`process.md:10`) to carry the real field — the end-to-end proof. Secondary surfacing: `git-janitor` closeout runs `followup-due-nudge.sh --list` so a follow-up can also fire at end-of-session.
4. **Tests** — `tests/bats/followup_due_nudge.bats` (fixture entries, stubbed `gh`, assert nudge text + `fired=` idempotency suppression), wired into `test-all.sh` discovery; the `--selftest` mirrors the repo's gate-selftest convention.

Non-obvious decisions (recorded for the grill): the `fired=` write-back is **orchestrator-stamped, not script-stamped** (keeps the script read-only); `pr-count` uses **gh-first with an offline `git log … | grep -cE '\(#[0-9]+\)'` fallback** because Smatchet squash-merges (so `git log --merges` returns 0 — verified) and SessionStart network is not guaranteed.

## Files to modify

1. `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` § Format (new, **slice 1**) — ~10-line addition documenting the optional `Triggered-follow-up:` line, its four `when=` grammars, the `fired=never` sentinel, and the "nudge echoes baseline verbatim" rule.
2. [`docs/agent-rules/process-rules.md`](../../../docs/agent-rules/process-rules.md) (**slice 1**) — a short § "Triggered follow-ups" (lifecycle: author with `fired=never` → nudge surfaces when due → orchestrator acts + stamps `fired=<date>` via PR → close/spawn-follow-up). Soft-warn-only sink. **No `AGENTS.md` edit** (over the 150-line cap; § Process rules already routes to `process-rules.md`).
3. `agents/scripts/core/followup-due-nudge.sh` (new, **slice 2**) — the read-only scanner + 4 trigger evaluators + `--list`/`--nudge`/`--selftest` modes; exit 0 always.
4. [`docs/harness/claude-code/settings.json.tmpl`](../../../docs/harness/claude-code/settings.json.tmpl) (**slice 3**) — one SessionStart hook stanza (`bash agents/scripts/core/followup-due-nudge.sh --nudge`, 10000ms timeout). Regenerated into `.claude/settings.json` by `setup-harness.sh`.
5. [`docs/self-improvement/categories/process.md`](../../../docs/self-improvement/categories/process.md) (**slice 3**) — migrate the #866 archetype entry (`:10`) to carry `Triggered-follow-up: when=pr-count:base=develop;since=2026-06-05;n=10; action=re-run last-10-PRs CR-audit; baseline=3.2 heads/PR + 1.3 nudges/PR (success <2 and ->0); fired=never`.
6. [`agents/core/git-janitor.md`](../../../agents/core/git-janitor.md) § Standard cleanup loop (**slice 3**) — add a `followup-due-nudge.sh --list` step to the closeout (≤ 250-line cap — one line).
7. `tests/bats/followup_due_nudge.bats` (new, **slice 4**) — fixture + stubbed-gh + idempotency assertions.
8. `tests/bats/` discovery / `scripts/dev/test-all.sh` (**slice 4**) — confirm the new `.bats` is auto-discovered (it likely is via glob; verify, don't reinvent).

Before adding rows: `rg -l 'followup|follow-up' agents/scripts/ docs/` to confirm no existing follow-up tracker under a synonym (the prior-art lens found none — the process.md:30 stale-active-plan proposal is a *sibling* gate, not this).

## Existing utilities reused

- `agents/scripts/core/memory-drain-nudge.sh` — clone for the OFFLINE triggers (`date` / `file-age` / `plan-shipped`): env-overridable thresholds, `find -mtime` / epoch arithmetic, silent-exit-below-threshold, single `printf`, exit 0 always.
- `agents/scripts/core/postmortem-owed.sh` — clone for the NETWORK `pr-count` trigger + dedup discipline: `--list`/`--nudge` switch, `cd "$(dirname "$0")/../../.."` repo anchor, gh-unavailable→advisory-skip, `has_entry()` grep-idempotency (here: the entry's own `fired=` marker), single batched `gh` call, `## === … (N) ===` block.
- `docs/self-improvement/categories/*.md` — the trigger store itself; no new file. The #866 entry already encodes the `pr-count` trigger in prose — migrating it is the first worked example.
- `docs/harness/claude-code/settings.json.tmpl` SessionStart block — copy one `{type:command, command:bash …, timeout, matcher:""}` stanza.
- `test-backlog-counts.sh --selftest` — model for the `--selftest` grammar check.
- `tests/bats/merge_gates.bats` — shape precedent for `followup_due_nudge.bats` (fixtures + stubbed gh + assertions).
- `agents/scripts/core/setup-harness.sh` — regenerates `.claude/settings.json` from the `.tmpl`; reused, not edited.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — agentic-shell (bash nudge + docs + bats), zero runtime/UI code.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact — no UI-thread code; the SessionStart hook runs out-of-process with a 10s timeout, never on the UI thread.
- **Pillar 3 (never crash)**: no impact — no product C++; the script is `set -euo pipefail`, exit 0 always (advisory), failures silent.
- **Pillar 4 (accessibility)**: no impact — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

N/A — pure docs + agentic-shell (`docs/self-improvement/*`, `docs/agent-rules/process-rules.md`, `agents/scripts/core/*.sh`, `docs/harness/*.tmpl`, `agents/core/git-janitor.md`, `tests/bats/*.bats`); no `Source/Core/` or perf-gated path touched. All five gates (PR-fast CI, Pillar-2 scanner, dispatcher drain, bucket-E, marker inventory) are N/A for the same reason.

## Risks / non-goals

- **Risk: script self-stamping `fired=` violates the no-write nudge invariant + causes concurrent-PR conflicts.** Mitigation — script is **read-only**; the orchestrator stamps `fired=<date>` through the normal PR flow when it acts. (This is the exact failure class the `AGENT_SELF_IMPROVEMENT.md` count-column removal eliminated.)
- **Risk: stale-local-fetch under-count for `pr-count`** — a SessionStart that hasn't fetched `origin/develop` since the date rolled reads 0 and never fires. Mitigation — gh-first with `git log` offline fallback; ambiguous → SKIP-and-retry-next-session, never a one-shot fire-or-miss.
- **Risk: silent-spec-rot** — a `when=` that never becomes true (typo'd slug, wrong base) silently never fires, replacing prose-rot with spec-rot. Mitigation — `--selftest` grammar check + an optional "unparseable trigger" WARN line in `--list`.
- **Risk: squash-merge `(#N)` false positives** — `(#N)` can appear in revert/cherry-pick subjects. Low risk for a count threshold; add a `merged-only` filter if precision matters.
- **Risk: SessionStart banner noise / latency** — N nudges accreting. Mitigation — 10s timeout, single combined block, silent unless a real trigger fires (the memory-drain/postmortem-owed discipline).
- **Risk: wiring the drifted live `.claude/settings.json`** (it already points two hooks at a nonexistent `agents/scripts/dev/` path and omits `postmortem-owed`) → a hand-edit is lost on regenerate. Mitigation — edit the `.tmpl` only + regenerate; the live-file drift is filed as a separate `tooling.md` entry (see Out of scope).
- **Non-goal**: a JSON/YAML trigger registry in `project.config.json` + a centralized `eval-trigger.sh` evaluator — premature at ~2 live entries; deferred until 5+ concurrent triggered entries justify the parser.
- **Non-goal**: the `process.md:30` stale-active-plan CI gate — a sibling advisory closeout, tracked separately; this plan only co-surfaces, doesn't build it.
- **Non-goal**: porting the nudge to Codex/Cursor — SessionStart hooks are a Claude-Code harness feature; other harnesses read the `Triggered-follow-up:` field as plain text during manual triage.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ changed.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver / sanitizer**: the bats suite IS the bash-driver coverage (see below).
- **Build gate**: N/A — agentic-shell + docs diff (`is-pure-docs-diff.sh` / agentic-shell envelope); no `cmake --build` (no C++).
- **Script self-checks**: `bash agents/scripts/core/followup-due-nudge.sh --selftest` green (asserts the 4 `when=` grammars parse + synthetic FIRE/SKIP/UNAVAILABLE); `bash tests/bats/followup_due_nudge.bats` green (fixture entries, stubbed `gh`, `fired=` suppresses re-nudge); `bash scripts/dev/test-all.sh` discovers + passes the new bats.
- **Wiring check**: `bash agents/scripts/core/setup-harness.sh claude-code` regenerates `.claude/settings.json` with the new SessionStart hook present; a dry session-start shows the nudge fires on the migrated #866 entry **only after** the `pr-count` trigger is met (and is silent before).
- **Shell-lint**: `shellcheck` clean on the new script (the repo's shell-lint gate).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script for the sub-step list). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model (the nudge contract, the `fired=` idempotency model, the squash-merge counting reality, the settings.json-drift hazard) + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Manual residue**: the "nudge actually fires after 10 real PRs" path can't be fully exercised until 10 PRs merge; deferred-automation action = the bats suite stubs the `pr-count` evaluator with a fixture count to prove FIRE/SKIP deterministically (no real-PR wait), logged in `docs/self-improvement/categories/tooling.md` if any residue remains. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (esp. the JSON registry / centralized evaluator if a reader assumes they're in scope), and revise or delete them.

- **JSON trigger registry + `eval-trigger.sh` centralized evaluator** — deferred; gate on 5+ concurrent triggered entries. Follow-up plan: lift the four inline evaluators into one config-driven evaluator with per-trigger state in `%LOCALAPPDATA%/Smatchet/` (mirrors merge-watcher).
- **`process.md:30` stale-active-plan CI gate (`test-plan-staleness.sh`)** — sibling advisory closeout; separate plan. This plan's nudge can later absorb it into one "closeout-followups" block (a grill open-decision).
- **Fixing the drifted live `.claude/settings.json`** (`agents/scripts/dev/` dead paths + missing `postmortem-owed`) — file a `docs/self-improvement/categories/tooling.md` P2 entry; out of this plan's scope but discovered during it.
- **`Triggered-follow-up:` on plan-docs** (so "re-measure after 10 PRs" lives in `docs/plans/active/<slug>.md § Verification` where authored, not only the backlog mirror) — a grill open-decision; if accepted, the nudge scans `docs/plans/` too. No-action until decided.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
