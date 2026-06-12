# Plan — build-infra followups (6 backlog items)

> **Slug**: `build-infra-followups` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

## Context

Burn down six `docs/self-improvement/categories/` build-infra backlog items the user named:
infra.md `local-dev-gate-must-be-ci-scoped` / `subagent-build-reconfigure-hazard` / worktree-FetchContent-cache, and
tooling.md cold-configure / `scan_narrowing`-parallel / C++-lint-in-CI.

On audit, the **three tooling items were already code-shipped by #1166** (`fix(build): cpr-submodule
cold-configure + scan_narrowing parallel + C++ lint in CI`) but their backlog entries were left
`Status: open` (stale). The **three infra items are genuinely unimplemented** — each is doc + (one)
a small delta-gated lint. After this lands: the three tooling entries read `applied` (citing #1166),
and the three infra items have their concrete next-actions shipped (durable rules + one new lint +
worktree recipe docs).

## Approach

Two strands, one PR (one logical feature = "build-infra followups"):

1. **Bookkeeping** — flip the three stale-open tooling entries to `applied`, each citing #1166 with a
   one-line of what shipped; note the cpp-lint residual (advisory → promote-to-blocking after the 6
   standing empty-catch blocks are burned down).
2. **Implementation** — the three infra items:
   - `local-dev-gate-must-be-ci-scoped`: durable rule in `build-doctor.md` + `cpp-rules.md` §
     enforcement, **plus** a new delta-gated lint (`scan_ci_scoped_local_gate`) that flags a NEW
     `message(FATAL_ERROR …)` in `CMakeLists.txt` referencing a local knob (`msvc_toolset_pin`)
     without a nearby `ENV{CI}`/`ENV{GITHUB_ACTIONS}` guard — bats + `--selftest`, contract-card row.
   - `subagent-build-reconfigure-hazard`: a single shared standing clause (DRY — one source) wired
     into the five build-touching agent contracts; the optional configure-stamp is deferred with a
     same-turn backlog note.
   - worktree-FetchContent-cache: document the `-DFETCHCONTENT_BASE_DIR=<main-repo>/.fetchcontent-src`
     recipe in `process-rules.md` § worktree discipline + the delegation worktree boilerplate, noting
     #1166's `GIT_EXEC_PATH` fix now lets a worktree cold-configure on its own (the redirect is the
     skip-the-refetch optimization, no longer a hard requirement).

Trade-off named: the new lint is a cheap grep heuristic over `CMakeLists.txt` (not a CMake AST parse)
— scoped to the one realistic regression vector (a local-knob `FATAL_ERROR` without CI scope), matching
the entry's "cheap lint" framing.

## Files to modify

1. `docs/self-improvement/categories/tooling.md` — flip the 3 stale-open entries → `applied` (cite #1166).
2. `docs/self-improvement/categories/infra.md` — flip the 3 infra entries → `applied`/`applied-partial`.
3. `agents/scripts/project/test-lint-rules.sh` — add `scan_ci_scoped_local_gate` + contract-card + `--selftest`.
4. `tests/bats/<new>.bats` — fires-on-violation / passes-on-CI-guarded fixtures for the new lint.
5. `agents/core/build-doctor.md` — durable CI-scoped-gate rule (symptoms/gates section).
6. `docs/agent-rules/cpp-rules.md` — § Tiered enforcement: the CI-scoped-gate rule + new rule-id row.
7. `AGENTS.md` — Enforcement contract-card: add the new rule-id row; delegation worktree boilerplate line.
8. `docs/agent-rules/process-rules.md` — worktree FetchContent recipe + the no-reconfigure subagent clause home.
9. `agents/core/{tracker-backend,offline-sync,grid-engine,test-rig,ui-host}.md` — reference the shared no-reconfigure clause.

## Existing utilities reused

- `scan_narrowing` parallel pattern + the `_selftest` convention in `agents/scripts/project/test-lint-rules.sh` — the new lint mirrors its scan-fn + selftest + contract-card shape.
- The bats auto-enrolment pattern (`test-<name>-bats.sh` wrapper) used by `pr_burst_guard.bats` / `git_leftover_audit.bats`.

## UX Pillar callouts

- **Pillar 1 (perf)**: N/A — no `Source/Core` / runtime code; tooling + docs only.
- **Pillar 2 (UI-thread)**: N/A — no UI code.
- **Pillar 3 (never crash)**: N/A — no product code.
- **Pillar 4 (accessibility)**: N/A — no UI code.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

N/A — the diff touches no `Source/Core/` path (tooling scripts, agent prompts, docs, backlog only).

## Risks / non-goals

- **Risk**: the new CMake-`FATAL_ERROR` lint is a grep heuristic → could false-pass on an obfuscated
  knob reference. Accepted — the entry asked for a "cheap lint"; delta-gated so only NEW violations fire.
- **Non-goal**: the optional per-worktree configure-lock/stamp (infra:16 action 3) — deferred with a
  backlog note; the standing-clause + the #1166 cold-configure fix remove the acute pain.
- **Non-goal**: promoting the cpp-lint CI job from advisory → blocking (tooling:231 residual) — gated on
  burning down the 6 standing empty-catch blocks; tracked as the entry's residual.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ helper added.
- **Bucket E (ImGui Test Engine)**: N/A — no UI code.
- **Bash-driver / lint**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` green;
  the new lint's `--selftest` green; the new bats file green; `bash agents/scripts/project/test-lint-rules.sh` self-tests pass.
- **Build gate**: N/A — no C++ change (nothing to compile; #1166 already shipped + verified the C++/CI pieces).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green
  (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: terminology checked against the backlog
  entries' own framing; outcome recorded post-ship.
- **Manual residue**: none expected — all verification is bats + doc-validation.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`,
`agents/*.md`, `docs/self-improvement/categories/` for stray refs to anything deferred here.

- Per-worktree configure-stamp (infra:16 action 3) — follow-up backlog note.
- cpp-lint advisory → blocking promotion (tooling:231 residual) — follow-up after empty-catch burndown.

## Implementation log

- `2f7a8df8` · wip(plan): authored this plan doc.
- `af8b0898` · feat(build-infra): the 6-item burndown — `cmake-local-gate-ci-scope` lint (`test-lint-rules.sh` scan-fn + `--scan-cmake-ci` mode + diff-gate block + `--selftest` + AGENTS.md contract-card row); 5 cases in `tests/bats/lint_rules.bats`; durable CI-scope rule in `build-doctor.md` + `cpp-rules.md`; no-reconfigure clause (`build.md` #5) + FetchContent recipe (`build.md` #6) + delegation-packet injection (`delegation.md`); 3 tooling + 3 infra backlog entries flipped to `applied`.
- *(this commit)* · post-ship: filled these sections + archived active → shipped (#1168).

## Deviations from plan

- **infra:16 home** — the no-reconfigure clause was single-sourced in `build.md` + the delegation packet (governing the 5 named agents at packet-build time) rather than copied into the 5 agent contracts, per the DRY pillar. The optional per-worktree configure-stamp (action 3) was deferred — tracked as the entry's residual.
- **bats location** — the 5 cmake-ci-scope cases went into the existing `tests/bats/lint_rules.bats` (which already covers `test-lint-rules.sh`) rather than a new `cmake_ci_scope.bats`, avoiding a new wrapper + auto-enrolment (DRY).
- **tooling items** — were code-shipped by #1166; this PR only flips their stale-open backlog entries to `applied` (no code change — verified #1166's diff).

## Verification (actual)

- Delta lint gate (`test-lint-rules.sh --diff origin/develop`): PASS — incl. `no un-CI-scoped local-knob CMake FATAL_ERROR`. ✅
- `tests/bats/lint_rules.bats`: 33 tests, 0 failures (5 new cmake-ci cases green). ✅
- `test-lint-rules.sh --selftest`: green (fire / CI-scoped-clean / deviation-clean + AGENTS.md-presence asserts-failure). ✅
- `--scan-cmake-ci` on the real tree: clean (the toolset guard is CI-scoped). ✅
- Doc-validation (`scripts/dev/test-docs.sh`): 13/13 PASS. ✅
- Build gate: N/A — no C++ change (#1166 already shipped + verified the C++/CI pieces). Not-run (correct).
- `grill-with-docs`: terminology checked against the backlog entries' own framing (the rule-id `cmake-local-gate-ci-scope` + the `subagent-build-reconfigure-hazard` / `worktree-FetchContent-cache` slugs are reused verbatim from the entries). No new domain terms introduced.
