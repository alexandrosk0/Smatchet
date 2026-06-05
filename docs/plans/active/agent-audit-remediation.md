# Plan — Agent-audit remediation (portfolio eval + infra-feedback fixes)

> **Slug**: `agent-audit-remediation` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

A two-part audit produced this plan. **Part 1** evaluated all 25 loaded agent definition files (`agents/core/*.md` ×17 + `agents/project/*.md` ×8): 22/25 grade A, 3 grade B, frontmatter + `## Self-improvement` + `## Outcome:` contract uniformly present — but it surfaced concrete defects (verified against the live repo): three agents instruct users to run **CMake presets that do not exist**, three correctness-gating reviewers cite the **retired MinGW/MSYS2 toolchain**, several agent descriptions have **overlapping routing triggers**, two agents sit **over the 250-line cap**, and the **ImGui host/theme/docking layer has no owning agent**. **Part 2** verified five pieces of external feedback (flagged "possibly obsolete"): two survived (the gate-escape detector `postmortem-owed.sh` reflects current-head status not merge-time truth, and new harness hooks never reach already-provisioned local adapters), one was partial (a stale doc-path shorthand, but not the claimed discovery break), and two were stale/false (the skill-parity guard is green; revert-dedupe already works).

Notably, both surviving feedback items sit on the **gate-escape detector itself** — the tool whose job is catching gate escapes has gate-escape-shaped holes (imprecise about merge-time, and not even wired in this adapter). That is the highest-signal "gate, don't trust" lesson in the batch and it shapes the sequencing below.

**Intended outcome — one sentence:** after this lands, every verified agent-prompt defect is corrected with a regression gate that prevents its class, the gate-escape detector reads a lossless merge-time snapshot over a merge-recency-ordered window, new harness hooks self-heal into existing local adapters, and the ImGui host layer has a dedicated `ui-host` owner.

## Approach

Six slices, sequenced by value and dependency. All are **pure-docs / agentic-shell** (agent `*.md`, `docs/*.md`, `agents/scripts/**`, `merge-watcher.py`, a new `*.jsonl` ledger, gitignored `.claude/`) — no `Source/Core/` C++, so build/ctest/perf gates skip (`is-pure-docs-diff.sh`). Each fix ships **with the gate that prevents its class** ("gate, don't trust"), not just the point-fix.

- **Slice 1 — Agent-doc correctness (P0 + P1 + C1; mechanical).** Fix the three non-existent CMake presets and the stale MinGW/MSYS2 toolchain citations, plus the `agents/*.md` doc-path shorthand. Ship **two new gates**: a selftest cross-checking every `ninja-*` token in `agents/**/*.md` against `CMakePresets.json`, and a doc-validation token failing on a retired-build-toolchain mention in agent prompts. Routes through `mechanic` (find-and-replace, no judgement). **Highest value — these are the only findings that hard-fail a user cycle.**
- **Slice 2 — Routing negative-guards (P1; mechanical).** Add one-clause negative guards to the routing-facing descriptions of `architect` (vs subsystem specialists), `security-review`/`code-review` (trust-boundary split), and the perf family (`perf-detective` vs `spike-hunter`; `perf-measure`/`perf-instrument` as helper-only). Co-shippable with Slice 1 (both pure agent-`*.md` edits) but tracked separately as a distinct concern.
- **Slice 3 — Gate-escape detector hardening (C2; design-signoff).** `postmortem-owed.sh` gains (a) **mergedAt-ordered** scan window (mechanical), and (b) a **merge-time snapshot ledger** read it consults before the live `statusCheckRollup` query, written cooperatively by every merge actor at the decision instant. The live query stays as the documented degraded fallback for un-instrumented / pre-ledger merges.
- **Slice 4 — Adapter settings self-heal (C3; mechanical local + design).** Immediately repair *this* workspace's `.claude/settings.json` (C3-A, no PR — gitignored), and add a `sync_settings_hooks()` jq structural-merge that brings missing **template** hooks into an existing `settings.json` without clobbering user edits (C3-B), invoked from both `setup-harness.sh` and the `clear-session-context.sh` SessionStart auto-heal. This is what makes Slice 3's improved detector actually fire locally — so C3-A runs first.
- **Slice 5 — Size + contract-heading hygiene (P2; mechanical).** Shrink `debug-detective` (419), `test-author` (268), `coderabbit-triage` (215) back under the 250 cap via skill extraction; promote inline `## Outcome:` / `## Self-improvement` prose to literal trailing headings (6 agents) and restore the full 5-class enum where subsetted (`test-rig`, `issue-janitor`); back it with a contract-presence selftest. Depends on Slice 1 (which edits `test-author` + `coderabbit-triage` content first).
- **Slice 6 — `ui-host` agent (P3; design).** New `agents/project/ui-host.md` owning ImGui host/theme/dockspace/bootstrap; relocate `grid-engine`'s self-disclaimed dock-migration invariant into it; register it in the delegation table and as the visual-validation-exception owner.

Non-obvious trade-off shaping Slice 3: the live `statusCheckRollup` is *provably lossy* (GitHub overwrites rollup contexts by name on re-run, and override labels are stripped post-merge per policy), so the only lossless capture of merge-time truth is a snapshot taken **at the decision instant** by the merge actor — hence a cooperative distributed write rather than a single-reader query.

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

### Slice 3 — Gate-escape detector hardening (C2; design-signoff)
15. `agents/scripts/core/postmortem-owed.sh:64-85` — (A) add `mergedAt` to `--json`, raise fetch to `--limit $((SCAN_N*3))` (escape hatch env `POSTMORTEM_FETCH_N`), post-sort `sort_by(.mergedAt)|reverse|.[:SCAN_N]` in `JQ_ROWS`. (B) ledger-first read: for each in-window PR, if `docs/self-improvement/merge-snapshots.jsonl` has a line keyed by `pr`+`mergeCommit`, derive the trigger from the snapshot (`overrideLabels` / `redChecks`) instead of the live rollup; fall through to the existing live `JQ_ROWS` path (now documented as the degraded fallback) only when no ledger entry exists. `has_entry()` dedupe unchanged.
16. `agents/scripts/core/merge-snapshot-append.sh` (**new** shared helper) — `append_merge_snapshot <pr> <mergeCommit> <headSha> <gatesVerdict> <overrideLabelsCsv> <mergeActor>`; composes one compact single-line JSON object and `>>`-appends it to the JSONL ledger, idempotent on `(pr,mergeCommit)` via a grep-guard. Sourceable + CLI. Passes `test-shell-lint.sh`.
17. `agents/scripts/core/merge-watcher.py` (`handle_pass()`, after `squash_merge_pr()` returns `merge_sha` ~line 1780, **before** `maybe_remove_from_registry` ~1783) — call the helper (or an inline `_append_merge_snapshot()` writing the same line to the repo-rooted ledger) with `mergeActor='merge-watcher'`, capturing the gated head SHA + any override labels present at merge.
18. `docs/agent-rules/ship-loops.md:30` (§ after GATES_PASSED squash-merge) — document that the in-session orchestrator merge (`gh api .../merge`) and `git-janitor` MUST append a snapshot line via `merge-snapshot-append.sh` (`mergeActor='orchestrator'`/`'git-janitor'`) so their merges aren't invisible to the ledger-first path.
19. `docs/self-improvement/merge-snapshots.jsonl` (**new** committed append-only ledger) — schema documented in the append-helper header + `postmortems.md`.
20. `docs/plans/shipped/gate-escape-postmortem.md` (edit) — § Implementation-log/Deviations note: primary red-check trigger now reads the merge-time snapshot first (lossless); live `statusCheckRollup` demoted to fallback; scan window mergedAt-ordered.

### Slice 4 — Adapter settings self-heal (C3)
21. **C3-A (local, no PR — gitignored):** regenerate this workspace's `.claude/settings.json` so the SessionStart (`postmortem-owed --nudge`, `memory-drain-nudge`, `vexp-strip-agents-md`), Stop (`check-main-repo-clean`), and PostToolUse hooks match the template — delete + re-run `setup-harness.sh claude-code`, or hand-add the missing hooks.
22. `agents/scripts/core/setup-harness.sh` (~line 205, in `setup_claude_code()` after `copy_template … settings.json`) — add `sync_settings_hooks <tmpl> <dst>`: a jq structural merge appending only template hooks whose `.command` is absent from the matching `event`→`matcher` group; guard `command -v jq` (else one-line WARN); write-temp-then-`mv` (atomic).
23. `agents/scripts/core/clear-session-context.sh` (after the hook-script sync loop ~line 93) — call the same merge against `$PROJECT_DIR/docs/harness/claude-code/settings.json.tmpl` → `.claude/settings.json` (the auto-heal path; fires every SessionStart post-`git pull`, no re-provision needed). Silent on success, `|| true` on every external call, skip if jq/either file absent. Update the header-comment block (lines 77-81) to document the settings-hook sync.
24. `agents/scripts/core/test-setup-harness.sh` (**new** Test 7) — seed a `settings.json` with only the bootstrap hook + a `permissions` sentinel + a user-added custom hook; run the merge; assert (a) all missing template hooks present, (b) `permissions` byte-preserved, (c) user hook survives, (d) idempotent second run, (e) **no duplicate matcher-`""` group under `Stop`**.

### Slice 5 — Size + contract-heading hygiene (P2)
25. `agents/core/debug-detective.md` (419→<250) — extract the §8 Crash / §9 Race-ordering workflow checklists into the `debug-instrument` skill; leave one-line judgement pointers.
26. `agents/core/test-author.md` (268→~150) — collapse the duplicated Report-format template into the Maintenance-class headings; move bucket-E wire-up gotchas + Pattern A/D bash skeletons into a new `test-authoring` skill. (After Slice 1's preset fix.)
27. `agents/core/coderabbit-triage.md` (215→<200) — de-duplicate restated override rules; extract the per-finding handoff-packet template if it can be shared. (After Slice 1's toolchain fix.)
28. Contract headings — promote inline `## Outcome:` / `## Self-improvement` to literal trailing headings in `mechanic`, `perf-measure`, `offline-sync`, `mcp-toolsmith`, `unreal-bridge`, `tracker-backend`; restore the full 5-class `## Outcome:` enum in `test-rig`, `issue-janitor`.
29. `agents/scripts/core/test-agent-contract.sh` (**new gate**) — assert every `agents/{core,project}/*.md` ends with a literal `## Self-improvement` heading and references `## Outcome:`; wire into `test-docs.sh`.

### Slice 6 — `ui-host` agent (P3)
30. `agents/project/ui-host.md` (**new**, ~79 lines, `sonnet`/`low`) — owns `Source/Core/{src,include}/Ui/SmatchetImGuiHost.*`, `SmatchetTheme.*` + `SmatchetThemeIds.h`, the dockspace scaffold + `SmatchetDockNodeIds.h` schema + dock-layout migration, `io.IniFilename` ordering, font atlas, `main.cpp` `BootSetupImGui`. Frontmatter `description` with explicit negative guards (NOT grid cells/columns → grid-engine; NOT DX12 backend/packaging → unreal-bridge; NOT per-panel draw → orchestrator/specialist); `capabilities: [semantic-code-search, file-skeleton, file-read, file-edit, text-search, file-glob, shell]`; `delegates-to: [unreal-bridge, perf-detective]`; `harness-hints.claude-code: {model: sonnet, effort: low}`.
31. `agents/project/grid-engine.md` — relocate OUT the final dock-migration hard-invariant bullet (self-disclaimed lane-creep) into `ui-host`; bump `version: 2`→`3`.
32. `docs/agent-rules/delegation.md` — add a `ui-host` row to § Subsystem specialists + a trigger row (`theme/dockspace/font/bootstrap/docking`) to § Trigger auto-activation.
33. `docs/agent-rules/ship-loops.md` (~visual-validation-exception path-list) — name `ui-host` as the owning agent for the `SmatchetTheme.cpp` / `Smatchet*Ui*.cpp` pause (currently ownerless).

## Existing utilities reused

- `agents/scripts/core/postmortem-owed.sh` `has_entry()` + `JQ_ROWS` (line 50-85) — the dedupe + scan pipeline Slice 3 extends; live path kept as fallback, not replaced.
- `agents/scripts/core/merge-gates.sh` (`statusCheckRollup` evaluation ~273-276; override-label strip policy ~723) — the merge-decision moment where the lossless snapshot exists; the override-strip policy is *why* the ledger is the only lossless override capture.
- `agents/scripts/core/merge-watcher.py` `handle_pass()` / `squash_merge_pr()` / `maybe_remove_from_registry` (~1753-1783) — the watcher write-site for the snapshot.
- `agents/scripts/core/memory-drain-nudge.sh` — the SessionStart deterministic-nudge precedent (`postmortem-owed.sh` already mirrors it; Slice 4 keeps the pattern).
- `agents/scripts/core/setup-harness.sh` `copy_template()` + `clear-session-context.sh` hook-script sync loop — the two call sites the settings-merge hooks into; `copy_template` stays generic (Slice 4 adds a *dedicated* settings-only function, not a 3-way teach).
- `docs/harness/claude-code/settings.json.tmpl` — the canonical hook set the merge syncs toward.
- `agents/project/tracker-backend.md` — the clean project-agent frontmatter template `ui-host` copies.
- `agents/_shared/skills/debug-instrument/SKILL.md` — the extraction sink for Slice 5's debug-detective shrink.
- `docs/agent-rules/AGENT-VS-SKILL.md` — governs the script/skill/agent split for the new `test-authoring` skill + the helper.
- `cmake/Sanitizers.cmake` + `CMakePresets.json` — ground truth for Slice 1's preset/sanitizer corrections and the new `test-agent-preset-refs.sh` gate.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no runtime impact — docs, agent prompts, shell/python scripts, gitignored adapter config. Zero product code. (`ui-host` will *steward* Pillar-1/2 surfaces when invoked later, but creating its definition is pure-docs.)
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact. Slice 4's SessionStart jq merge runs in a 3000 ms-budget hook, sub-100 ms on a ~115-line file, off the product UI thread entirely.
- **Pillar 3 (never crash)**: no impact (no product code). Indirectly strengthened — Slice 3 makes crash/perf gate-escapes more reliably detected; the preset/toolchain fixes stop agents emitting un-runnable sanitizer commands.
- **Pillar 4 (accessibility)**: no direct impact; Slice 6's `ui-host` gives the a11y/visual-validation surface a named (if still aspirational) owner where there was none.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

**N/A — no `Source/Core/` code, no C++.** The diff is `*.md` + `agents/scripts/**` (`*.sh` + `merge-watcher.py`) + a new `*.jsonl` ledger + gitignored `.claude/`. All are pure-docs-allowlisted by `is-pure-docs-diff.sh` → classifies pure-docs, so PR-fast CI / Pillar-2 scanner / dispatcher-drain / bucket-E / marker-inventory all skip. Verification is shell-lint + detector/merge dry-runs + the new selftests + doc-validation.

## Risks / non-goals

**Risks:**
- **Slice 3 distributed-write gap** — a merge actor that forgets to append leaves a ledger hole. → mitigated: the live `statusCheckRollup` query is kept as the documented degraded fallback, so the detector never goes blind; it only loses losslessness for un-instrumented paths until wired. All three actors (watcher / orchestrator / git-janitor) are named in Files-to-modify.
- **Slice 4 settings clobber** — a structural merge could overwrite legitimate per-machine user edits. → mitigated: append-by-command-string is additive-only (never removes/reorders/edits existing hooks; never touches `permissions`); atomic temp-then-`mv`; Test 7 asserts byte-preservation + the `Stop` matcher-`""` no-dup case.
- **Slice 4 jq absence on fresh clone** — jq not guaranteed. → both call sites guard `command -v jq` and degrade to a one-line stderr drift-nudge; `clear-session-context.sh` runs `set -u` (not `-e`) so a jq failure can't abort the bootstrap hook.
- **Slice 5 file collision with Slice 1** — Slice 5 restructures `test-author` + `coderabbit-triage`, which Slice 1 edits first. → sequenced: Slice 1 (small factual edits) merges before Slice 5 (restructure) starts.
- **Slice 1 over-aggressive toolchain grep** — the new retired-toolchain doc check could false-positive on legit `MSYS2` bats-install notes in `agents/scripts/**`. → the check is scoped to `agents/**/*.md` agent prompts and allowlists `agents/scripts/**`.
- **Slice 6 portfolio growth** — a 9th project agent adds a routing surface. → accepted: it closes a real ownership gap (host/theme/docking + the ownerless visual-validation pause) and removes grid-engine's self-disclaimed invariant; net lane-clarity gain.

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
  - Slice 3: `postmortem-owed.sh --list` against real history — mergedAt-ordered window includes a late-merging old branch; with a seeded ledger entry the trigger derives from the snapshot (override label survives even though stripped live); falls back to live query for a PR absent from the ledger; dedupes via `postmortems.md`. `test-shell-lint.sh` (5 rules) on `postmortem-owed.sh` + `merge-snapshot-append.sh`. `merge-snapshot-append.sh` idempotent on a re-run (no double line).
  - Slice 4: `test-setup-harness.sh` Test 7 (missing-hooks healed / `permissions` preserved / user hook survives / idempotent / no `Stop` dup). After `setup-harness.sh claude-code` the SessionStart nudges run without error. C3-A: confirm this workspace's `.claude/settings.json` now contains `postmortem-owed.sh --nudge`.
  - Slice 5: `test-agent-contract.sh` asserts literal `## Self-improvement` + `## Outcome:` across all 25; `agent_size_audit.py --diff origin/develop` shows the three shrunk agents under cap.
  - Slice 6: `test-skill-vs-agent-parity.sh` green (ui-host is agent-only, not a skill-only helper — no entry needed); `agent_size_audit.py` confirms `ui-host.md` ≤ 250; delegation-table doc-anchor check resolves the new row.
- **Build gate**: N/A — pure-docs (`is-pure-docs-diff.sh` returns true → build/ctest skipped).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script, don't hardcode sub-steps). A red doc-validation job blocks merge even though non-required. Note Slices 1/3/5 *add* steps to this suite.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms (esp. "merge-time snapshot" vs "gate verdict", "self-heal" vs "re-provision") before finalising; record the outcome. Required — do not delete.
- **Manual residue**: none designed. C3-A is a one-shot local repair (documented as a step, not silent). If any slice's verification ends up manual, add a `docs/self-improvement/categories/tooling.md` entry with a deferred-automation plan.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to the deferred items below + the relocated grid-engine dock invariant, and revise/delete them.

- **C4 (skill-parity guard "red") — VERIFIED STALE, no action.** The three skills (`adversarial-code-review`, `but-for-real`, `drain-memory`) are already in `SKILL_ONLY_HELPERS`; the script runs green (`Passed: 3 Failed: 0`). Recorded so a future reader doesn't re-open it.
- **C5 (revert-without-PR dedupe) — VERIFIED FALSE, no action.** `has_entry()` already matches both `PR #<n>` and `commit <sha>` (line 54); dedupe works (confirmed by runtime repro + `git log -L` to the introducing commit #784).
- **Accessibility (Pillar 4) substantive coverage** — Slice 6 gives the surface an owner, but real keyboard-nav / WCAG-AA work is a separate backlog item, not this plan.
- **The `completedAt<=mergedAt` rollup-filter fallback (C2)** — designed-out in favour of the snapshot ledger (the rollup overwrites RED entries on re-run, so there's nothing left to filter); documented here so it isn't re-proposed.
- **A docs-specialist agent / settings-rename auto-handling** — the eval noted thin doc-refactor coverage and Slice 4 heals only *missing* hooks (not template removals/renames); both are accepted follow-ups, not designed here.

## Implementation log
*(populated post-ship — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred vs the original plan, one-line rationale each)*

## Verification (actual)
*(populated post-ship — what was actually tested + result: passed / failed / not-run)*
