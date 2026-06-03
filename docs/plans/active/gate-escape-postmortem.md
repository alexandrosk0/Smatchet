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

**1. Detection — a script.** `agents/scripts/core/postmortem-owed.sh` inspects recent `develop` merges for the escape signals (override label on the merged PR via the `gh pr view --json labels` pattern `merge-gates.sh` already uses; `git log` for `Revert`; `deviation-overdue` hits). It emits a "postmortem owed: PR #N — <trigger>" line when an escape has no postmortem entry referencing it. Wired as a `SessionStart` nudge, mirroring `memory-drain-nudge.sh` (deterministic check → nudge, no investigation) — so it surfaces at the start of the next session, not mid-ship.

**2. The postmortem — a skill.** `gate-escape-postmortem` (bounded: read the escaping PR/diff, do blameless RCA, name the preventing gate, file the entry) → skill per the rubric. It **escalates to `debug-detective`** only when the root cause needs deep C++ investigation (the one branch that fails the skill rubric). The skill writes one entry to a `docs/self-improvement/postmortems.md` ledger.

**3. The forcing function.** A postmortem entry **cannot close** without its `### Preventing gate` field — the concrete new gate/rule/test/lint that catches the class (e.g. "add markdownlint to the pre-push gate", "tighten the perf scenario map to cover X"). That field's action is **filed into the existing `docs/self-improvement/categories/{tooling,process,test,…}`** as a normal entry — so the established apply-threshold + triage cadence applies it. **No new apply-loop**; the postmortem is the incident *finder*, the existing loop is the *applier*.

**Blameless by construction.** Entries name the gate hole, never an agent/person — Smatchet's actors are autonomous agents anyway; blame is meaningless, the gate is everything.

## Files to modify

1. `agents/scripts/core/postmortem-owed.sh` (new) — detector + nudge. Scans the last N `develop` merges for override-labels / `Revert` commits / `deviation-overdue` hits; emits "postmortem owed" for any escape lacking a `postmortems.md` entry. Passes `test-shell-lint.sh` (5 rules). `--list` (plain) / `--nudge` (SessionStart-formatted) modes.
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
- **Detector**: `bash agents/scripts/core/postmortem-owed.sh --list` on a synthetic history flags (a) a merge carrying an override label and (b) a `Revert` commit, and stays silent on a clean docs-only merge; dedupes once a matching `postmortems.md` entry exists.
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
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
