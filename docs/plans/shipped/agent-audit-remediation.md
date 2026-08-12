# Plan — Agent-audit remediation (portfolio eval + infra-feedback fixes)

> **Slug**: `agent-audit-remediation` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

A two-part audit produced this plan. **Part 1** evaluated all 25 loaded agent definition files (`agents/core/*.md` ×17 + `agents/project/*.md` ×8): 22/25 grade A, 3 grade B, frontmatter + `## Self-improvement` + `## Outcome:` contract uniformly present — but it surfaced concrete defects (verified against the live repo): three agents instruct users to run **CMake presets that do not exist**, three correctness-gating reviewers cite the **retired MinGW/MSYS2 toolchain**, several agent descriptions have **overlapping routing triggers**, two agents sit **over the 250-line cap**, and the **ImGui host/theme/docking layer has no owning agent**. **Part 2** verified five pieces of external feedback (flagged "possibly obsolete"): two survived (the gate-escape detector `postmortem-owed.sh` reflects current-head status not merge-time truth, and new harness hooks never reach already-provisioned local adapters), one was partial (a stale doc-path shorthand, but not the claimed discovery break), and two were stale/false (the skill-parity guard is green; revert-dedupe already works).

Notably, both surviving feedback items sit on the **gate-escape detector itself** — the tool whose job is catching gate escapes has gate-escape-shaped holes (imprecise about merge-time, and not even wired in this adapter). That is the highest-signal "gate, don't trust" lesson in the batch and it shapes the sequencing below.

**Intended outcome — one sentence:** after this lands, every verified agent-prompt defect is corrected with a regression gate that prevents its class, the gate-escape detector reads a lossless merge-time snapshot over a merge-recency-ordered window, new harness hooks self-heal into existing local adapters, and the ImGui host layer has a dedicated `ui-host` owner.

## Approach

Five slices, sequenced by value and dependency (a sixth — the agent size-reduction — was split to its own plan per the grill). All are **pure-docs / agentic-shell** (agent `*.md`, `docs/*.md`, `agents/scripts/**`, `merge-watcher.py`, gitignored `.claude/`) — no `Source/Core/` C++, so build/ctest/perf gates skip (`is-pure-docs-diff.sh`). Each fix ships **with the gate that prevents its class** ("gate, don't trust"), not just the point-fix.

- **Slice 1 — Agent-doc correctness (P0 + P1 + C1; mechanical).** Fix the three non-existent CMake presets and the stale MinGW/MSYS2 toolchain citations, plus the `agents/*.md` doc-path shorthand. Ship **two new gates**: a selftest cross-checking every `ninja-*` token in `agents/**/*.md` against `CMakePresets.json`, and a doc-validation token failing on a retired-build-toolchain mention in agent prompts. Routes through `mechanic` (find-and-replace, no judgement). **Highest value — these are the only findings that hard-fail a user cycle.**
- **Slice 2 — Routing negative-guards (P1; mechanical).** Add one-clause negative guards to the routing-facing descriptions of `architect` (vs subsystem specialists), `security-review`/`code-review` (trust-boundary split), and the perf family (`perf-detective` vs `spike-hunter`; `perf-measure`/`perf-instrument` as helper-only). Co-shippable with Slice 1 (both pure agent-`*.md` edits) but tracked separately as a distinct concern.
- **Slice 3 — Gate-escape detector scan-window fix (C2-part-2; mechanical).** `postmortem-owed.sh` gets a **mergedAt-ordered** scan window so a late-merging long-lived branch's escape can't fall outside it. The merge-time **snapshot ledger** (C2-part-1) is **deferred to its own follow-up plan + ADR** (grill decision 2026-06-05): it's lossless-but-distributed complexity (a 3-actor write contract) for an advisory medium-severity nudge, and the live `statusCheckRollup` query remains the documented fallback meanwhile. Slice 3 is now purely mechanical.
- **Slice 4 — Adapter settings self-heal (C3; mechanical local + design).** Immediately repair *this* workspace's `.claude/settings.json` (C3-A, no PR — gitignored), and add a `sync_settings_hooks()` jq structural-merge that brings missing **template** hooks into an existing `settings.json` without clobbering user edits (C3-B), invoked from both `setup-harness.sh` and the `clear-session-context.sh` SessionStart auto-heal. This is what makes Slice 3's improved detector actually fire locally — so C3-A runs first.
- **Slice 5 — `ui-host` agent (P3; design).** New `agents/project/ui-host.md` owning ImGui host/theme/dockspace/bootstrap; relocate `grid-engine`'s self-disclaimed dock-migration invariant into it; register it in the delegation table, the visual-validation-exception owner slot, and `test-agent-contract.sh`'s `IMPLEMENTERS` array. Seed from the session's recovered design draft.

**PR batching (grill 2026-06-05): two PRs.** **PR1** = Slices 1+2+3 (agent-doc correctness + routing guards + postmortem ordering — all mechanical, one "agent-system correctness" feature). **PR2** = Slices 4+5 (settings self-heal + ui-host). C3-A (local `.claude/settings.json` repair) runs immediately, outside any PR. Escape hatch: if PR2's diff approaches the per-PR file ceiling, split along the settings-tooling / ui-host seam into two PRs.

Non-obvious trade-off shaping the deferred ledger (now § Out-of-scope): the live `statusCheckRollup` is *provably lossy* (GitHub overwrites rollup contexts by name on re-run, and override labels are stripped post-merge per policy), so the only lossless capture of merge-time truth is a snapshot taken **at the decision instant** by the merge actor — hence the cooperative distributed write the follow-up plan must design.

## Files to modify

### Slice 1 — Agent-doc correctness (mechanical)
1. `agents/core/build-doctor.md:45,68,69` — replace non-existent `ninja-debug-msvc-tsan` / `ninja-debug-msvc-msan` with the real `ninja-msvc-asan` / `ninja-clang-asan`, or reword the TSan/MSan bullets to state no TSan/MSan preset is wired (per `cmake/Sanitizers.cmake`: TSan deferred, MSan Clang-only). Fix the `:45` `(GCC; ASan+UBSan)` mislabel → MSVC `/fsanitize=address` (ASan only, no UBSan on MSVC).
2. `agents/core/test-author.md:175-177` — replace `ninja-asan` (preset + `build/ninja-asan/` path) with `ninja-msvc-asan`, matching the file's own correct `:57` reference.
3. `agents/core/code-review.md:59` — `MSVC + MinGW UCRT` → `MSVC + Clang`.
4. `agents/core/coderabbit-triage.md:95` — `must compile on MinGW UCRT` → `must compile on MSVC + Clang (Unreal compat)`.
5. `AGENTS.md:122` — `agents/<name>.md at repo root` → `agents/{core,project}/<name>.md`; `edits to agents/*.md` → `edits to agents/{core,project}/*.md`.
6. `docs/harness/codex/setup.md:6` — `agents/*.md` → `agents/core/*.md` + `agents/project/*.md`. **Do NOT** change the `:16` "It writes nothing" line (correct intended behaviour — Codex reads `AGENTS.md` natively).
7. `docs/harness/cursor/rules/agents.mdc:10` — `agents/*.md` → `agents/core/*.md` and `agents/project/*.md`.
8. `agents/scripts/core/test-agent-preset-refs.sh` (**new gate**) — grep every `ninja-*` token in `agents/**/*.md`, assert each resolves to a `name` in `CMakePresets.json`; non-zero exit on a dangling preset. Passes `test-shell-lint.sh` (5 rules). Wire into `scripts/dev/test-docs.sh` (or `test-lint-rules.sh`).
9. `scripts/dev/test-docs.sh` (edit) — add a `retired-toolchain` token check: fail on `MinGW UCRT` / `MSYS2`-as-build-toolchain in `agents/**/*.md` (allow the legitimate bats/bash-install `MSYS2` mentions in `agents/scripts/**`).

### Slice 2 — Routing negative-guards (mechanical, description-only)
10. `agents/core/architect.md` (frontmatter `description`) — append `NOT for changes confined to one subsystem → owning specialist; architect is for designs spanning 2+ subsystems`.
11. `agents/core/security-review.md` (`description`) — append `fires only when the diff crosses a trust boundary (MCP/CLI/Lua/p4/HTTP/SQLite/AI); general correctness → code-review`.
12. `agents/core/code-review.md` (`description`) — append `security-sensitive trust-boundary diffs also route to security-review`.
13. `agents/core/perf-detective.md` (`description`) — append `NOT intermittent hitches/p99 outliers → spike-hunter`.
14. `agents/core/perf-measure.md` + `agents/core/perf-instrument.md` (`description`) — append `helper-dispatched by perf-detective/spike-hunter only; not directly user-routed`.

### Slice 3 — Gate-escape detector scan-window fix (C2-part-2; mechanical)
15. `agents/scripts/core/postmortem-owed.sh:64-85` — add `mergedAt` to `--json`, raise fetch to `--limit $((SCAN_N*3))` (escape-hatch env `POSTMORTEM_FETCH_N`), and post-sort `sort_by(.mergedAt)|reverse|.[:SCAN_N]` in `JQ_ROWS` so the scan window is anchored to merge recency, not PR-creation order (a long-lived branch created early but merged late now lands inside it). `has_entry()` dedupe + the live-rollup trigger logic are unchanged.
16. `docs/plans/shipped/gate-escape-postmortem.md` (edit) — § Deviations note: scan window is now mergedAt-ordered; the lossless merge-time snapshot ledger is tracked as a deferred follow-up (see § Out-of-scope).

> **Deferred here (grill 2026-06-05):** the merge-time snapshot ledger — lossless capture of override-labels + red-checks at the merge-decision instant, written by `merge-watcher.py` `handle_pass()` / the orchestrator's `gh api .../merge` / `git-janitor` into a committed `docs/self-improvement/merge-snapshots.jsonl`, read ledger-first by `postmortem-owed.sh` — is pulled into its own follow-up plan + ADR. See § Out-of-scope.

### Slice 4 — Adapter settings self-heal (C3)
21. **C3-A (local, no PR — gitignored):** regenerate this workspace's `.claude/settings.json` so the SessionStart (`postmortem-owed --nudge`, `memory-drain-nudge`, `vexp-strip-agents-md`), Stop (`check-main-repo-clean`), and PostToolUse hooks match the template — delete + re-run `setup-harness.sh claude-code`, or hand-add the missing hooks.
22. `agents/scripts/core/setup-harness.sh` (~line 205, in `setup_claude_code()` after `copy_template … settings.json`) — add `sync_settings_hooks <tmpl> <dst>`: a jq structural merge appending only template hooks whose `.command` is absent from the matching `event`→`matcher` group; guard `command -v jq` (else one-line WARN); write-temp-then-`mv` (atomic).
23. `agents/scripts/core/clear-session-context.sh` (after the hook-script sync loop ~line 93) — call the same merge against `$PROJECT_DIR/docs/harness/claude-code/settings.json.tmpl` → `.claude/settings.json` (the auto-heal path; fires every SessionStart post-`git pull`, no re-provision needed). Silent on success, `|| true` on every external call, skip if jq/either file absent. Update the header-comment block (lines 77-81) to document the settings-hook sync.
24. `agents/scripts/core/test-setup-harness.sh` (**new** Test 7) — seed a `settings.json` with only the bootstrap hook + a `permissions` sentinel + a user-added custom hook; run the merge; assert (a) all missing template hooks present, (b) `permissions` byte-preserved, (c) user hook survives, (d) idempotent second run, (e) **no duplicate matcher-`""` group under `Stop`**.

### Slice 5 — `ui-host` agent (P3; design)
*(Was Slice 6. Old Slice 5 — the debug-detective/test-author/coderabbit-triage size shrink + contract-heading hygiene + the literal-`## Self-improvement` check — is split to its own `agent-size-reduction` plan per the grill; see § Out-of-scope.)*
25. `agents/project/ui-host.md` (**new**, ~79 lines, `sonnet`/`low`) — owns `Source/Core/{src,include}/Ui/SmatchetImGuiHost.*`, `SmatchetTheme.*` + `SmatchetThemeIds.h`, the dockspace scaffold + `SmatchetDockNodeIds.h` schema + dock-layout migration, `io.IniFilename` ordering, font atlas, `main.cpp` `BootSetupImGui`. Frontmatter `description` with explicit negative guards (NOT grid cells/columns → grid-engine; NOT DX12 backend/packaging → unreal-bridge; NOT per-panel draw → orchestrator/specialist); `capabilities: [semantic-code-search, file-skeleton, file-read, file-edit, text-search, file-glob, shell]`; `delegates-to: [unreal-bridge, perf-detective]`; `harness-hints.claude-code: {model: sonnet, effort: low}`. **Seed from the recovered design draft** (preserved in this session's design-probe transcript) rather than re-authoring blank.
26. `agents/project/grid-engine.md` — relocate OUT the final dock-migration hard-invariant bullet (self-disclaimed lane-creep) into `ui-host`; bump `version: 2`→`3`.
27. `docs/agent-rules/delegation.md` — add a `ui-host` row to § Subsystem specialists + a trigger row (`theme/dockspace/font/bootstrap/docking`) to § Trigger auto-activation.
28. `docs/agent-rules/ship-loops.md` (~visual-validation-exception path-list) — name `ui-host` as the owning agent for the `SmatchetTheme.cpp` / `Smatchet*Ui*.cpp` pause (currently ownerless).
29. `agents/scripts/core/test-agent-contract.sh:50` (**edit — dependency surfaced by grill**) — add `ui-host` to the `IMPLEMENTERS` array (it uses Implementer headings: `## Files changed` / `## Smoke-test result` / `## Manual residue`). Without this, the new agent silently escapes contract checks 1 + 5 + 10 (required-headings / banner-match / version-match). Verify the draft's banner `model/effort` + `· vN` match its frontmatter (checks 5 + 10 are byte-exact).

## Existing utilities reused

- `agents/scripts/core/postmortem-owed.sh` `has_entry()` + `JQ_ROWS` (line 50-85) — the dedupe + scan pipeline Slice 3 extends; live path kept as fallback, not replaced.
- `agents/scripts/core/memory-drain-nudge.sh` — the SessionStart deterministic-nudge precedent (`postmortem-owed.sh` already mirrors it; Slice 4 keeps the pattern).
- `agents/scripts/core/test-agent-contract.sh` (343 lines / 13 checks) — the existing agent-contract gate Slice 5 extends (add `ui-host` to `IMPLEMENTERS`); already asserts `## Outcome:` + per-class headings.
- `agents/scripts/core/setup-harness.sh` `copy_template()` + `clear-session-context.sh` hook-script sync loop — the two call sites the settings-merge hooks into; `copy_template` stays generic (Slice 4 adds a *dedicated* settings-only function, not a 3-way teach).
- `docs/harness/claude-code/settings.json.tmpl` — the canonical hook set the merge syncs toward.
- `agents/project/tracker-backend.md` — the clean project-agent frontmatter template `ui-host` copies.
- `docs/agent-rules/AGENT-VS-SKILL.md` — governs the agent-vs-skill choice for `ui-host` (an exploration/edit subsystem agent, not a bounded skill).
- `cmake/Sanitizers.cmake` + `CMakePresets.json` — ground truth for Slice 1's preset/sanitizer corrections and the new `test-agent-preset-refs.sh` gate.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no runtime impact — docs, agent prompts, shell/python scripts, gitignored adapter config. Zero product code. (`ui-host` will *steward* Pillar-1/2 surfaces when invoked later, but creating its definition is pure-docs.)
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact. Slice 4's SessionStart jq merge runs in a 3000 ms-budget hook, sub-100 ms on a ~115-line file, off the product UI thread entirely.
- **Pillar 3 (never crash)**: no impact (no product code). Indirectly strengthened — Slice 3 makes crash/perf gate-escapes more reliably detected; the preset/toolchain fixes stop agents emitting un-runnable sanitizer commands.
- **Pillar 4 (accessibility)**: no direct impact; Slice 5's `ui-host` gives the a11y/visual-validation surface a named (if still aspirational) owner where there was none.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

**N/A — no `Source/Core/` code, no C++.** The diff is `*.md` + `agents/scripts/**` (`*.sh` + `merge-watcher.py`) + a new `*.jsonl` ledger + gitignored `.claude/`. All are pure-docs-allowlisted by `is-pure-docs-diff.sh` → classifies pure-docs, so PR-fast CI / Pillar-2 scanner / dispatcher-drain / bucket-E / marker-inventory all skip. Verification is shell-lint + detector/merge dry-runs + the new selftests + doc-validation.

## Risks / non-goals

**Risks:**
- **Slice 3 over-fetch multiplier** — an extremely old branch with huge PR volume since its creation could still fall outside even a `SCAN_N*3` created-desc fetch. → mitigated: `POSTMORTEM_FETCH_N` env escape-hatch; the live-rollup trigger path is unchanged, so there is no regression vs today's behaviour. (The deferred snapshot-ledger's distributed-write-gap risk travels with it to the follow-up plan.)
- **Slice 4 settings clobber** — a structural merge could overwrite legitimate per-machine user edits. → mitigated: append-by-command-string is additive-only (never removes/reorders/edits existing hooks; never touches `permissions`); atomic temp-then-`mv`; Test 7 asserts byte-preservation + the `Stop` matcher-`""` no-dup case.
- **Slice 4 jq absence on fresh clone** — jq not guaranteed. → both call sites guard `command -v jq` and degrade to a one-line stderr drift-nudge; `clear-session-context.sh` runs `set -u` (not `-e`) so a jq failure can't abort the bootstrap hook.
- **Slice 1 over-aggressive toolchain grep** — the new retired-toolchain doc check could false-positive on legit `MSYS2` bats-install notes in `agents/scripts/**`. → the check is scoped to `agents/**/*.md` agent prompts and allowlists `agents/scripts/**`.
- **Slice 5 portfolio growth** — a 9th project agent adds a routing surface. → accepted: it closes a real ownership gap (host/theme/docking + the ownerless visual-validation pause) and removes grid-engine's self-disclaimed invariant; net lane-clarity gain.
- **Cross-plan dependency (split-out `agent-size-reduction`)** — that plan restructures `test-author` + `coderabbit-triage`, which this plan's Slice 1 also edits. → the size-reduction plan must sequence after this plan's Slice 1 PR merges (noted in its § Context); not a risk to *this* plan.

**Non-goals:**
- Touching `Source/Core/` product code — every slice is docs/scripts/config; the `ui-host` agent's *future* edits are out of this plan's scope.
- Auto-fixing the agent prompts without review — Slices 1-2 route through `mechanic` (mechanical) but still ship as reviewed PRs.
- A CI-blocking "merge-snapshot required" gate — the snapshot is advisory plumbing for the existing SessionStart nudge; blocking merges on it would re-introduce ceremony.
- Re-architecting `copy_template` into a generic 3-way merge — Slice 4 adds a dedicated settings-only function; the generic copier stays line-diff-dumb.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ logic.
- **Bucket E (ImGui Test Engine)**: N/A — no UI code.
- **Bash-driver scenario / shell selftests**:
  - Slice 1: `test-agent-preset-refs.sh` flags a seeded dangling `ninja-bogus` and passes on the corrected files; `test-docs.sh` retired-toolchain check fails on a seeded `MinGW UCRT` and passes after the fixes.
  - Slice 3: `postmortem-owed.sh --list` against real history — the mergedAt-ordered window now includes a late-merging old branch that created-desc order dropped; existing live-rollup triggers + `has_entry()` dedupe are unchanged (regression check — same escapes flagged as before). `test-shell-lint.sh` (5 rules) on `postmortem-owed.sh`.
  - Slice 4: `test-setup-harness.sh` Test 7 (missing-hooks healed / `permissions` preserved / user hook survives / idempotent / no `Stop` dup). After `setup-harness.sh claude-code` the SessionStart nudges run without error. C3-A: confirm this workspace's `.claude/settings.json` now contains `postmortem-owed.sh --nudge`.
  - Slice 5: `test-agent-contract.sh` green with `ui-host` added to `IMPLEMENTERS` (checks 1/5/10 cover the new agent); `test-skill-vs-agent-parity.sh` green (ui-host is agent-only — no skill entry needed); `agent_size_audit.py` confirms `ui-host.md` ≤ 250; delegation-table doc-anchor check resolves the new row; `grid-engine` version bumped 2→3 with banner in lockstep (check 10).
- **Build gate**: N/A — pure-docs (`is-pure-docs-diff.sh` returns true → build/ctest skipped).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script, don't hardcode sub-steps). A red doc-validation job blocks merge even though non-required. Note Slices 1/3/5 *add* steps to this suite.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms (esp. "merge-time snapshot" vs "gate verdict", "self-heal" vs "re-provision") before finalising; record the outcome. Required — do not delete.
- **Manual residue**: none designed. C3-A is a one-shot local repair (documented as a step, not silent). If any slice's verification ends up manual, add a `docs/self-improvement/categories/tooling.md` entry with a deferred-automation plan.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to the deferred items below + the relocated grid-engine dock invariant, and revise/delete them.

- **C4 (skill-parity guard "red") — VERIFIED STALE, no action.** The three skills (`adversarial-code-review`, `but-for-real`, `drain-memory`) are already in `SKILL_ONLY_HELPERS`; the script runs green (`Passed: 3 Failed: 0`). Recorded so a future reader doesn't re-open it.
- **C5 (revert-without-PR dedupe) — VERIFIED FALSE, no action.** `has_entry()` already matches both `PR #<n>` and `commit <sha>` (line 54); dedupe works (confirmed by runtime repro + `git log -L` to the introducing commit #784).
- **Accessibility (Pillar 4) substantive coverage** — Slice 5 gives the surface an owner, but real keyboard-nav / WCAG-AA work is a separate backlog item, not this plan.
- **Agent size-reduction (former Slice 5) — SPLIT to its own `agent-size-reduction` plan (grill 2026-06-05).** Shrinking `debug-detective` (419), `test-author` (268), `coderabbit-triage` (215) under the 250 cap via skill extraction (debug-detective §8/§9 → `debug-instrument`; a new `test-authoring` skill), the inline-`## Self-improvement`→literal-heading promotion (6 agents), and the new literal-`## Self-improvement` check (14th check in `test-agent-contract.sh`). Split because skill-extraction is a design call (not pure mechanical), the three agents are grandfathered (no live gate failure), and it has a cross-plan dependency on this plan's Slice 1. **Residue-sweep DONE (#873):** the `agent-size-baseline.md` grandfathered entries were reconciled (test-author retired, debug-detective recorded at 316) when that plan shipped.
- **Merge-time snapshot ledger (C2-part-1) — DEFERRED to its own plan + ADR (grill 2026-06-05).** A committed `docs/self-improvement/merge-snapshots.jsonl` written at the merge-decision instant by `merge-watcher.py` `handle_pass()` / the orchestrator's `gh api .../merge` / `git-janitor`, read ledger-first by `postmortem-owed.sh`, to losslessly capture override-labels (stripped post-merge per `merge-gates.sh:723`) + red-checks (overwritten on rollup re-run per `:273-336`). Deferred because it is a 3-actor distributed-write contract + a new committed substrate to harden an *advisory* nudge — it warrants a focused ADR (hard-to-reverse ledger format + write contract; surprising; a real lossless-but-distributed vs lossy-but-simple trade-off). Slice 3 ships only the mechanical ordering fix; the live-rollup fallback covers the interim. **Note:** `handle_pass()` (`merge-watcher.py:1753`) currently has neither override-labels nor the gated head SHA in scope at the write-site (`:1780`) — the follow-up must add a `gh pr view --json labels` + head-oid fetch, a cost the ADR should weigh.
- **The `completedAt<=mergedAt` rollup-filter fallback (C2)** — designed-out in favour of the deferred snapshot ledger (the rollup overwrites RED entries on re-run, so there's nothing left to filter); documented here so it isn't re-proposed in the follow-up.
- **A docs-specialist agent / settings-rename auto-handling** — the eval noted thin doc-refactor coverage and Slice 4 heals only *missing* hooks (not template removals/renames); both are accepted follow-ups, not designed here.

## Implementation log

- **#868** (`abbb8ad2`, 2026-06-05) — **Slices 1+2+3**: agent-doc correctness (phantom `ninja-*` presets → real `ninja-msvc-asan` / `ninja-clang-asan`; retired MinGW/MSYS2 → `MSVC + Clang` citations; `agents/{core,project}/*.md` doc-path shorthand) + routing negative-guards (`architect` / `security-review` / `code-review` / `perf-detective` / `perf-measure` / `perf-instrument` descriptions) + `postmortem-owed.sh` mergedAt-ordered scan window. Shipped one new gate `test-agent-build-facts.sh` (presets + retired-toolchain in a single check).
- **#869** (`88ffd530`, 2026-06-05) — **Slices 4+5**: settings-hook self-heal (`agents/scripts/core/sync-settings-hooks.sh`, invoked from `clear-session-context.sh` SessionStart) + new `agents/project/ui-host.md` (ImGui host/theme/dockspace/bootstrap owner). `grid-engine`'s self-disclaimed dock-migration invariant relocated into `ui-host`, `grid-engine` version 2→3; `ui-host` wired into `delegation.md`, `ship-loops.md` (visual-validation owner), and `test-agent-contract.sh` `IMPLEMENTERS`.
- **#870** (`3b64258f`, 2026-06-05) — stubbed the two deferred follow-up plans (`merge-snapshot-ledger`, `agent-size-reduction`) so their triggers don't die in this plan's prose.
- **C3-A** (local, no PR — gitignored) — this workspace's `.claude/settings.json` healed to the template hook set before Slice 4's self-heal landed.

## Deviations from plan

- **Slice 1 gate generalized + renamed.** Planned as two checks (`test-agent-preset-refs.sh` for `ninja-*` tokens + a `test-docs.sh` retired-toolchain token); shipped as a **single `test-agent-build-facts.sh`** cross-checking both `ninja-*` presets against `CMakePresets.json` AND retired MinGW/MSYS2 mentions — same coverage, one gate. It then caught a phantom preset the eval itself had missed (in `debug-instrument`) during the later `agent-size-reduction` campaign — the gate earned its keep.
- **Slice 4 shipped as a standalone script, not an inline function.** Planned as a `sync_settings_hooks()` function inside `setup-harness.sh`; shipped as a standalone `agents/scripts/core/sync-settings-hooks.sh` (65L) invoked from `clear-session-context.sh` SessionStart — cleaner separation, same additive-jq-merge (never clobbers user edits) + the Test-7 guarantees.
- **Former Slice 5 (agent size-reduction) split out and shipped separately** as the `agent-size-reduction` plan (**#873**, now in `shipped/`) per the grill — its residue-sweep (the three grandfathered baseline entries) was honoured there.
- **Merge-snapshot ledger (C2-part-1) shipped as its own plan** `docs/plans/shipped/merge-snapshot-ledger.md` (authored + grilled in #870; **shipped 2026-06-05** — committed JSONL ledger + `merge-snapshot-append.sh` helper + 3-writer wiring + ADR-0017). Slice 3 here shipped only the mechanical mergedAt-ordering fix; the lossless half (the live `statusCheckRollup` query is now the documented degraded *fallback*, not the primary read) landed in that plan.

## Verification (actual)

- **Slice 1**: `test-agent-build-facts.sh` green — cross-checks every `ninja-*` token in `agents/**/*.md` vs `CMakePresets.json` (15 preset refs) + flags retired MinGW/MSYS2 (9 refs); `build-doctor.md` / `test-author.md` presets corrected.
- **Slice 2**: routing negative-guards present in the 5 agent `description`s; `test-agent-contract.sh` green.
- **Slice 3**: `postmortem-owed.sh` carries `mergedAt` (4 refs) + the mergedAt-ordered window; `postmortem-owed: no gate escapes owed` on current history (regression-clean).
- **Slice 4**: `sync-settings-hooks.sh` (65L) additive jq merge, called from `clear-session-context.sh` (2 refs); C3-A local `.claude/settings.json` repair applied + verified.
- **Slice 5**: `agents/project/ui-host.md` (79L, ≤ 250); `grid-engine` v3; `ui-host` in `delegation.md` (3 refs) + `ship-loops.md` + `test-agent-contract.sh` `IMPLEMENTERS`; contract green (26/0).
- **Doc-validation**: full `test-docs.sh` suite green on develop post-merge. **Build gate**: N/A — pure-docs / agentic-shell (`is-pure-docs-diff.sh` → pure-docs).
