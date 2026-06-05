# Plan — Gate-escape postmortem (incident → new gate)

> **Slug**: `gate-escape-postmortem` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Smatchet captures lessons two ways, both of which **miss failed outcomes**. The self-improvement loop captures *friction noticed during work* (agents end with `## Self-improvement` → `docs/self-improvement/categories/*`). The plan post-ship sections (§ Implementation log / § Deviations) capture *what changed vs plan*. Neither systematically captures the highest-signal event for a "gate, don't trust" harness: a **gate escape** — something shipped that a gate should have caught.

Gate escapes already emit signals Smatchet records but never acts on as a class:
- an override label used at merge (`tests-out-of-band` / `perf-out-of-band` / `cr-out-of-band`, per `project.config.json` `merge_gates.override_labels`) — each is an explicit "we shipped past a gate";
- a `git revert` / hotfix on `develop`;
- a `SMATCHET_DEVIATION` left past its `revisit=` (already a strict `deviation-overdue` lint hit, but with no root-cause step);
- a bug found *after* merge that a required check should have caught.

Each escape is evidence a gate failed, and — given the project's "gate, don't trust" philosophy — the correct response is a **new gate**, not a one-off fix. Today that conversion happens only by luck. Lived proof from the plan-shipping work: the markdownlint **MD038 / broken-link** CodeRabbit findings recurred across three consecutive PRs before a pre-clean habit formed; a postmortem on the first occurrence would have added a pre-push markdownlint + relative-link check and made every later PR clean on the first try.

**Intended outcome — one sentence:** after this lands, a gate escape (override label / revert / overdue deviation) raises a "postmortem owed" nudge, and the postmortem produces a blameless entry whose **mandatory** field is *the gate that would prevent the class* — filed into the existing self-improvement apply-loop.

## Approach

A narrow, **trigger-only** mechanism — not a per-PR ceremony (that would tax the autonomous one-turn ship-loop for no gain). Three parts, each formed per `docs/agent-rules/AGENT-VS-SKILL.md`:

**1. Detection — a script.** `agents/scripts/core/postmortem-owed.sh` inspects recent `develop` merges for the escape signals and emits a "postmortem owed: PR #N — <trigger>" line when an escape has no postmortem entry referencing it. Four triggers:
- **non-SUCCESS check on the merged head** (the highest-signal trigger; added 2026-06-03 after the agentic-harness campaign proved it) — scan each merged PR's `statusCheckRollup` (`gh pr view --json statusCheckRollup`) for any check that was NOT `SUCCESS`/`SKIPPED`/`NEUTRAL` at merge time. This catches the most common escape: a **non-required** CI job (e.g. "Doc anchors + agent contract") that was RED yet merged because branch-protection only gates the required set. The campaign shipped a portable-purity leak, an INDEX drift, and a dangling ref to `develop` exactly this way — no override label, no revert, no overdue deviation, just a red non-required check. The override-label-only detector would have been blind to all three.
- **override label** on the merged PR (via the `gh pr view --json labels` pattern `merge-gates.sh` already uses, against `project.config.json` `merge_gates.override_labels`) — an explicit "we shipped past a gate".
- **`Revert` commit** on `develop` (`git log --grep`) — a post-merge undo.
- **overdue `SMATCHET_DEVIATION`** — defers to the existing strict `deviation-overdue` lint (cross-reference, not re-implemented).

Wired as a `SessionStart` nudge, mirroring `memory-drain-nudge.sh` (deterministic check → nudge, no investigation) — so it surfaces at the start of the next session, not mid-ship.

**2. The postmortem — a skill.** `gate-escape-postmortem` (bounded: read the escaping PR/diff, do blameless RCA, name the preventing gate, file the entry) → skill per the rubric. It **escalates to `debug-detective`** only when the root cause needs deep C++ investigation (the one branch that fails the skill rubric). The skill writes one entry to a `docs/self-improvement/postmortems.md` ledger.

**3. The forcing function.** A postmortem entry **cannot close** without its `### Preventing gate` field — the concrete new gate/rule/test/lint that catches the class (e.g. "add markdownlint to the pre-push gate", "tighten the perf scenario map to cover X"). That field's action is **filed into the existing `docs/self-improvement/categories/{tooling,process,test,…}`** as a normal entry — so the established apply-threshold + triage cadence applies it. **No new apply-loop**; the postmortem is the incident *finder*, the existing loop is the *applier*.

**Blameless by construction.** Entries name the gate hole, never an agent/person — Smatchet's actors are autonomous agents anyway; blame is meaningless, the gate is everything.

## Files to modify

1. `agents/scripts/core/postmortem-owed.sh` (new) — detector + nudge. Scans the last N `develop` merges for **non-SUCCESS checks on the merged head** (primary) / override-labels / `Revert` commits (+ cross-references `deviation-overdue`); emits "postmortem owed" for any escape lacking a `postmortems.md` entry. Passes `test-shell-lint.sh` (5 rules). `--list` (plain) / `--nudge` (SessionStart-formatted) modes.
2. `.claude/settings.json` template under `docs/harness/claude-code/` (edit) — register `postmortem-owed.sh --nudge` as a `SessionStart` hook alongside the existing `memory-drain-nudge.sh` / `clear-session-context.sh`. (Generated into `.claude/` by `setup-harness.sh`; the tracked source is the harness template.)
3. `agents/_shared/skills/gate-escape-postmortem/SKILL.md` (new) — triggers (`postmortem`, `gate escape`, `why did this ship`, `post-merge bug`, an override-used nudge); workflow (identify the escaped class → blameless RCA → **mandatory** `### Preventing gate` → file the category entry → append to `postmortems.md`); escalate-to-`debug-detective` clause for deep C++ RCA. Skill-only → add to `SKILL_ONLY_HELPERS`.
4. `docs/self-improvement/postmortems.md` (new) — the append-only ledger. Entry shape: `## <date> · PR #N · <trigger>` → `### What escaped` (the gate that didn't catch it) · `### Root cause` (blameless) · `### Preventing gate` (mandatory; the new gate) · `### Filed as` (link to the spawned category entry).
5. `agents/scripts/core/test-skill-vs-agent-parity.sh` (edit) — add `gate-escape-postmortem` to `SKILL_ONLY_HELPERS`.
6. `AGENTS.md` (edit) — § Self-improvement loop gains a one-line pointer: gate escapes (override / revert / overdue deviation) owe a postmortem (`postmortems.md`); the preventing-gate action files into the normal categories.
7. `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (edit) — document the postmortem entry path + the mandatory preventing-gate field + that postmortem-spawned actions use the existing category format/threshold.

## Existing utilities reused

- `agents/scripts/core/memory-drain-nudge.sh` — the SessionStart deterministic-check-then-nudge precedent the detector copies.
- `agents/scripts/core/merge-gates.sh` label-inspection (`gh pr view --json labels`) — the detector reuses the same label read.
- `project.config.json` `merge_gates.override_labels` — the canonical override-label set the detector scans for (config-sourced, not hardcoded).
- `docs/self-improvement/categories/*` + `AGENT_SELF_IMPROVEMENT.md` (priority P0–P3, apply threshold, triage cadence) — the apply-loop the preventing-gate action files into; unchanged.
- `agents/core/debug-detective.md` — the escalation target for deep-RCA postmortems.
- `docs/agent-rules/AGENT-VS-SKILL.md` (from `internal-procedure-skills`) — governs the script/skill/agent form split here.
- `agents/scripts/core/setup-harness.sh` skill loop + `SessionStart` hook wiring — auto-links the skill + the nudge.

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact — a detector script + a skill + docs. Zero product code. Indirectly strengthens all four by converting gate escapes (incl. perf/crash escapes) into new gates.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code, no C++. The diff is `*.md` + scripts under `agents/scripts/core/` + a harness-template `settings.json` — `*.md` and `agents/scripts/**` are pure-docs-allowlisted by is-pure-docs-diff.sh, so it classifies pure-docs and build/ctest/perf gates skip. Verification is shell-lint + the detector dry-run.

## Risks / non-goals

**Risks:**
- **Ceremony creep** — a postmortem on every PR would tax the autonomous loop. → trigger-only: fires solely on an *escape signal* (override / revert / overdue deviation / named post-merge bug), never on a clean merge.
- **Duplicating the self-improvement loop.** → the postmortem is the incident *finder*; the preventing-gate action files into the existing categories with the existing threshold. One ledger (`postmortems.md`) + the existing apply-loop, not a second system.
- **Postmortem without a fix** (RCA theatre). → the `### Preventing gate` field is mandatory; a postmortem cannot close without naming the concrete gate/rule/test. No gate named = not done.
- **Nudge noise / false triggers** — a legitimate override (e.g. `cr-out-of-band` on a docs PR) raising a postmortem that concludes "working as intended." → acceptable: the entry can close with `### Preventing gate: none — override legitimate (reason)`, which is itself a recorded decision; the detector dedupes on PR number so it nudges once.
- **Blame drift** — entries naming agents/people. → blameless by construction; the entry template has no actor field, only the gate hole.

**Non-goals:**
- A per-PR retrospective — covered (adequately) by plan § Deviations; not duplicated.
- Auto-applying the preventing gate — suggestion-only; the orchestrator/human applies via the normal loop.
- Detecting post-merge *bugs* automatically — that trigger is manual (the user/orchestrator names it); only override/revert/overdue-deviation are auto-detected.
- A CI-blocking "postmortem required" gate — the nudge is advisory; blocking merges on an unfiled postmortem would re-introduce the ceremony tax.

## Verification

- **Bucket A / E**: N/A — no code.
- **Detector**: `bash agents/scripts/core/postmortem-owed.sh --list` flags (a) a merged PR with a non-SUCCESS check on its head, (b) a merge carrying an override label, and (c) a `Revert` commit, and stays silent on a clean merge whose checks were all green; dedupes once a matching `postmortems.md` entry exists.
- **Forcing function**: a `postmortems.md` entry missing `### Preventing gate` is rejected (a `test-postmortem-format.sh` check, or the skill refuses to close) — verified by a fixture entry.
- **Loop integration**: a sample postmortem's preventing-gate action lands as a well-formed `docs/self-improvement/categories/<cat>.md` entry (existing format).
- **Parity + lint**: `test-skill-vs-agent-parity.sh` green (`gate-escape-postmortem` in `SKILL_ONLY_HELPERS`); `test-shell-lint.sh` on the detector.
- **Hook wiring**: after `setup-harness.sh claude-code`, the SessionStart nudge runs alongside `memory-drain-nudge.sh` without error.
- **Pure-docs**: `is-pure-docs-diff.sh` returns true → build/ctest skipped.
- **Build gate**: N/A — pure-docs.
- **Manual residue**: the post-merge-bug trigger is manual (named by the user/orchestrator) — documented in `AGENT_SELF_IMPROVEMENT.md`, not silent.

## Out of scope (flagged, not designed)

- **Auto-detecting post-merge bugs** — needs a "this bug should have been caught by gate X" classifier; manual trigger for now.
- **A `coverage-out-of-band` trigger** — depends on the `coverage-threshold-graduation` plan landing that label first; add to the detector's label set when it exists.
- **Trend analytics over `postmortems.md`** (which gate-class escapes most) — a later `context-budget`/`rules-distill`-style audit, once the ledger has volume.
- **CI-blocking postmortem enforcement** — deliberately advisory; revisit only if escapes go unaddressed.

## Implementation log

- `agents/scripts/core/postmortem-owed.sh` (new) — detector + nudge. **4 triggers**: non-SUCCESS check on the merged head (primary, added this revision), override label (config-sourced `PC_OVERRIDE_LABELS`), `Revert` commit, + `deviation-overdue` cross-ref. `--list` / `--nudge` modes; dedupes on `postmortems.md` "PR #N". Optimized to a **single batched `gh pr list --json statusCheckRollup`** call (~3 s, SessionStart-safe) instead of per-PR `gh pr view`.
- `agents/_shared/skills/gate-escape-postmortem/SKILL.md` (new) — 6-step workflow (identify class → blameless RCA → mandatory `### Preventing gate` → file category entry → append ledger → PR-only), `debug-detective` escalation for deep C++ RCA. Skill-only.
- `docs/self-improvement/postmortems.md` (new) — the append-only ledger + entry-shape header, **seeded** with this session's real doc-gate-escape postmortem (dogfooding; dedups #771/#774/#776/#778/#780).
- `docs/harness/claude-code/settings.json.tmpl` — `postmortem-owed.sh --nudge` SessionStart hook (timeout 10000, after `memory-drain-nudge.sh`).
- `agents/scripts/core/test-skill-vs-agent-parity.sh` — `gate-escape-postmortem` → `SKILL_ONLY_HELPERS`.
- `AGENTS.md` § Self-improvement loop + `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` — gate-escape-postmortem path documented.
- **Red-check trigger now reads the lossless snapshot ledger first** (`merge-snapshot-ledger` plan, 2026-06-05). For an in-window merged PR with a `docs/self-improvement/merge-snapshots.jsonl` line (keyed by `pr`+`mergeCommit`), the detector derives the trigger (override-labels / red-checks captured *at the merge instant*) from that snapshot; the live `statusCheckRollup`/labels query is **demoted to a documented degraded fallback** used only when no ledger entry exists (un-instrumented / pre-ledger merges). This closes the losslessness gap the original trigger had — GitHub overwrites rollup contexts on re-run and strips override labels post-merge, so a live read can miss a real escape. Rationale: `docs/adr/0017-merge-time-snapshot-ledger.md`.

## Deviations from plan

- **Added a 4th, primary detector trigger** — "non-SUCCESS check on the merged head" — after the double-check found the original override-label/revert/overdue triggers would have been **blind to this session's actual escapes** (3 doc-gate regressions + a CR-findings bypass merged with a RED non-required check, no override label). § Approach + § Files patched. The first live run confirmed it dominates (caught #774/#776/#780).
- **`test-postmortem-format.sh` deferred** — the "entry cannot close without `### Preventing gate`" forcing function is enforced by the **skill workflow** (step 3, explicit) + the ledger-header contract, not a standalone CI check. A format-lint is a cheap follow-up if drift appears; filed as a residual rather than shipped now.
- Detector reworked from per-PR `gh pr view` to one batched `gh pr list` call for SessionStart latency.
- **Scan window now mergedAt-ordered (`agent-audit-remediation` plan, 2026-06-05).** `gh pr list` has no mergedAt sort, so the detector over-fetches by createdAt-desc (`POSTMORTEM_FETCH_N`, default `SCAN_N*3`) then re-sorts by `mergedAt` in jq and keeps the most-recently-merged `SCAN_N` — a long-lived branch created early but merged late no longer falls outside the window. The lossless **merge-time snapshot ledger** (so the red-check trigger reads the gate verdict *at the merge instant* rather than live `statusCheckRollup`, which GitHub overwrites on re-run) **shipped** in its own plan `docs/plans/shipped/merge-snapshot-ledger.md` (split from the now-shipped `agent-audit-remediation` plan; see also the § Implementation-log note above + ADR-0017).

## Verification (actual)

- Detector: `--list` on real history flags exactly the escapes (red-check #774/#776/#780, override-label PRs, reverts); dedupes the seeded entries (#771/#774/#776/#778/#780 no longer listed); silent triggers on a clean merge. ~3 s.
- `test-shell-lint` (5-rule) PASS on the detector + parity script. `shellcheck` clean (only SC1091/SC2016 info — sourced-file + jq-literal false-positives).
- `test-skill-vs-agent-parity` → `gate-escape-postmortem` SKIP (skill-only). `test-portable-purity` PASS. `test-docs` 7/7. `settings.json.tmpl` valid JSON.
- Pure-docs/agentic-shell — no build/ctest gate.
