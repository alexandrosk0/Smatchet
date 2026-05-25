# Process rules

> Lifted from [`AGENTS.md`](../../AGENTS.md) § Project rules per [`docs/design/agents-md-reduction.md`](../design/agents-md-reduction.md). AGENTS.md retains a load-bearing stub at § Process rules naming the three sub-families + the meta-rule so external `AGENTS.md § <subsection>` references continue to resolve. Build / language / quality 1-liners stay inline in AGENTS.md § Project rules. Edit this file directly — no parallel copy in AGENTS.md.

This file groups the rules that govern **how agents move work through the pipeline** — plan-doc lifecycle, destructive-VCS-op discipline, and cadence/verification rules. The companion files are [`merge-gates.md`](merge-gates.md) (what blocks a squash-merge), [`ship-loops.md`](ship-loops.md) (the turn-level loop shape), and [`delegation.md`](delegation.md) (agent routing + scratchpad / progress / output contracts).

## Plan-doc family

**Plan location**: every plan / design doc lives under `docs/design/<slug>.md`. No plans in repo root, `backlog/`, `~/.claude/plans/`, or working-tree-only scratch. `backlog/` is for triage lists (CPPCHECK_PLAN, AGENT_SELF_IMPROVEMENT) — not new plans. Naming: kebab-case slug matching the feature (`vs-style-view-menu.md`, `remove-global-project-key.md`).

**Plan-doc safety**: as soon as a plan is written to `docs/design/<slug>.md`, `git add` + commit it immediately with a `wip(plan): <slug>` prefix before any other work or branch operation. Working-tree-only files are silently lost on `git checkout`, `git reset --hard`, or GitHub Desktop branch switches. Recovery via `git fsck --lost-found` is expensive. Never leave a plan untracked across a session boundary.

**Plan revision after implementation**: when work shipped from a plan lands (PR merged, scenario validated, or feature shipped), edit the originating `docs/design/<slug>.md` in the same or next commit to record what actually happened. Mandatory sections to append:

- `## Implementation log` — bullet per shipped commit: `<sha> · <one-line summary>`.
- `## Deviations from plan` — what was changed, removed, or deferred relative to the original plan, with one-line rationale per item.
- `## Verification` — what was actually tested + result (passed / failed / not-run).

A plan that ships without revision is a stale plan. Future agents read these docs as truth; drift between plan and shipped reality is the main cost of multi-week feature work.

**Deferred plan-file rows at ship boundary**: when the orchestrator labels a numbered row in `docs/design/<slug>.md` § Files to modify as "optional" or skips it at the ship boundary, it MUST in the **same turn** either (a) ship the row, or (b) record both: an entry under `## Deviations from plan` in that plan-doc (`Item N deferred — <one-line rationale>`), and an open row in `docs/backlog/agent-self-improvement/process.md` when the deferral is follow-up work (not a permanent non-goal). Silent "optional" without deviation + backlog is a process failure — the next session treats the feature as complete.

**Plan stress-test — `grill-with-docs` skill**: before finalising `docs/design/<slug>.md`, invoke the skill to grill the plan against `docs/CONTEXT.md` (glossary) and `docs/adr/` (ADRs). Outputs: refined plan + glossary updates + new ADRs only when hard-to-reverse + surprising + real-trade-off all fire. Smatchet file mapping in `agents/_shared/skills/grill-with-docs/SMATCHET-NOTES.md`.

**Plan template — start from `docs/design/_plan-template.md`**: every new plan-doc is copied from the template, not authored blank. The template stubs every section the project rules require (Context, Approach, Files, **Pillar 1-3 callouts**, **Perf-review-system gates**, Risks, Verification, Implementation log / Deviations placeholders). Sections that genuinely don't apply must be filled with `N/A — <one-line reason>`, not deleted — drives the "did you consider this?" forcing function.

**Plan-doc perf-gate section — mandatory when diff touches `Source_Core/`**: every plan whose recommended-approach diff touches `Source_Core/` MUST include a § Perf-review-system gates section naming which gates from `docs/design/pillar-1-2-perf-review-system.md` fire on the PR (PR-fast scenario subset + which scenario directly exercises the changed path; Pillar 2 static scanner; dispatcher drain; visible-cue bucket-E; marker inventory) AND which don't apply with a one-line reason. The section also names the recommended pre-push local check (`scripts/dev/perf-run.sh <scenario>` + `perf-compare.py`) so the author catches regression before CI burns runner time. Orchestrator self-checks this section is present before `ExitPlanMode`; missing section = plan not ready.

## Git/p4 discipline

**Destructive git ops in shared worktrees**: before running any destructive git op (`reset --hard`, `checkout --`, `clean -f`, `branch -D`) against a worktree the orchestrator did not personally check out earlier in the same session, run a mandatory 5-step pre-flight via `git -C <path>` from the orchestrator's main worktree (do not `cd`). Parallel agents in other worktrees can — and do — switch the target worktree's HEAD to a different branch between sessions; a stale assumption about "the develop worktree" is what destroys uncommitted work.

1. `git -C <path> branch --show-current` — verify the actual current branch matches the user-named target. If it doesn't, **stop**; the worktree has been reassigned.
2. `git -C <path> status --short` — inventory tracked-modified + untracked files. Any non-empty result means the worktree is load-bearing for a parallel agent; treat as a refusal condition unless explicitly authorised.
3. `git -C <path> stash push -m "<reason>" -- <modified-files>` for any tracked-modified files, regardless of apparent relevance to the asked task. Untracked files survive `reset --hard` but `clean -f` is fatal — pass `--include-untracked` when `clean -f` is part of the plan.
4. Run the destructive op only after 1-3 succeed.
5. Decide whether to `stash pop` (recovers the user's work), leave the stash for the human (when unrelated to current PR), or report the stash hash so the human can pop manually if conflicts arise.

`reset --hard` permanently destroys uncommitted tracked-modified content; it is not in reflog. Branch pointers are reflog-recoverable; uncommitted changes are not. Cross-link: `agents/git-janitor.md` § Destructive-op pre-flight (authoritative checklist).

**Destructive `p4` ops in p4-mode**: when `SMATCHET_AGENT_VCS=p4`, the same defensive principle applies to destructive Perforce verbs (`p4 revert -k`, `p4 obliterate`, `p4 unshelve -f`). Five-step pre-flight in [`docs/perforce/AGENT_FLOWS.md`](../perforce/AGENT_FLOWS.md) § Destructive p4 ops pre-flight: confirm `p4 -ztag info` (client + user), inventory `p4 opened -c default //smatchet/...`, `p4 shelve -c <CL>` any pending unrelated CLs, run the destructive op, then `p4 changes -c <client>` to confirm state. `p4 revert` on a freshly-added file removes it from the workspace too — coordinate before running across shared depot paths.

**Force-push carve-out for Claude Code SDK-spawned recovery and p4 task-stream promotion**: the global `git push --force` ban (and the harness's banned `--no-verify` / `--no-gpg-sign` flags) gets two narrow carve-outs — `git push --force-with-lease origin <branch>` is permitted **only** during API-500 recovery (see [`docs/agent-rules/delegation.md`](delegation.md) § API-500 mid-run recovery) when the orchestrator is amending an unpushed-since-API-500 commit on:

1. A Claude Code SDK-spawned worktree branch matching `claude/<id>/*`, OR
2. A Perforce-task-stream-promoted branch matching `agent/<task-stream-id>/*` (created by `scripts/dev/p4-task-stream-to-pr.sh`).

The `agent/<id>` carve-out was deleted post-`ClaudeCodeLocalRunner` (per v1 of `docs/design/github-tracker-backend.md`) and is reinstated here for the p4-task-stream surface: those branches are recovery-style throwaways, created by the script after a successful shelf submit, and they should never carry non-self commits. The `smatchet-merge-watcher` host daemon (per `docs/design/smatchet-merge-watcher.md`) runs in-process, not as a spawned subprocess, so it has no worktree branch that would need this carve-out.

**Exclusion list — top-level-prefix-only** (an exclusion triggers only at the first path segment): `develop`, `main`, `chore/*`, `feat/*`, `fix/*`, `docs/*`, `wip/*`. Branches under `claude/<id>/*` and `agent/<task-stream-id>/*` are permitted regardless of the nested slug prefix (a slug like `feat-perf-fix` under `agent/perf-detective-01/` is fine — the `feat-` prefix is below the protected first segment).

Additional conditions for the carve-out to apply: ahead-range contains zero non-self commits; `--force-with-lease` (never bare `--force`). ADR [0005](../adr/0005-force-push-carve-out-for-spawned-agent-recovery.md) (Withdrawn as historical) covers the `claude/<id>` rationale; ADR [0008](../adr/0008-p4-gated-ship-loop.md) records the `agent/<task-stream-id>` extension + exclusion-list rewrite.

## Cadence and verification

**Verification automation — zero manual steps**: `test-author` converts every manual verification step into a deterministic CLI / scenario / screenshot / sanitizer / ImGui-Test-Engine assertion. Three invocation points: (1) plan-time audit of `docs/design/<slug>.md` § Verification, (2) post-first-round sweep, (3) every agent handoff that mentions a manual step. Unified runner: `bash scripts/dev/test-all.sh` (auto-enrols `scripts/dev/test-*.sh`). Manual residue without a `docs/backlog/AGENT_SELF_IMPROVEMENT.md` entry (category `tooling`) is a fail. "Truly interactive" is never the final answer — bucket E (ImGui Test Engine) is wired (see `docs/design/applied/imgui-test-engine-bucket-e-execution.md`; first test at `tests/ui/views_columns_reorder.test.cpp`; run via `cmake --build --preset ninja-ui-test-msys2`). Bucket details in `agents/test-author.md`.

**Schema-version bumps**: when a feature requires a config / cache schema-version bump, hold the bump until the feature is verified end-to-end. Do not commit interim version bumps as the feature evolves — squash or amend. The shipped version should be exactly one higher than the previous shipped version, not N higher because of intermediate iterations.

**Trivial-visual-only change envelope**: a change qualifies as **trivial-visual** when **every** condition holds — (a) write set is a strict subset of `{Source_Core/src/SmatchetTheme.cpp, Locales/*.json, ImGui style constants (ImVec4 / ImGuiStyle literals)}`; (b) diff shape is literals-only (no API surface, no header touch, no schema, no control flow, no new symbols); (c) zero touch under `Source_Core/include/` / `Plugins/` / `cmake/` / `CMakePresets.json`. Under this envelope the orchestrator may ship after: (1) `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` builds, (2) `ninja-test-msys2` ctest passes **if** a pure-logic test touches the changed file, (3) **NO** `ninja-ui-test-msys2` bucket-E run, (4) **NO** isolated worktree — use the main worktree with `git stash` race-recovery if a concurrent agent appears. Bucket-E coverage gets deferred to a single post-batch run before merge, or to the test backlog. Saves ~10× wall-clock on visual-only PRs (palette retunes, locale string fixes). Any condition fails → fall back to the full build + test-all + bucket-E loop.

**Build / ctest cadence — slice-boundary only**: within a single agent turn (= one logical slice), invoke `cmake --build` and `scripts/dev/test-all.sh` **at most once each**, and only after the implementation is complete. Mid-slice rebuilds and mid-slice ctest runs are wasted work — Ninja is already incremental and the doctest rig is fast at the slice boundary but expensive when amortised across N edits.

**P4-gated loops carve-out**: when running the small-change variant of [`docs/agent-rules/ship-loops.md`](ship-loops.md) § P4-gated ship-loop, the loop sequence has a legitimate `[smoke build] → [shelf] → [full tests]` phase split. The shelf-review boundary is a real phase transition (user-in-the-loop), so the pre-shelf smoke build (`ninja-iter-msys2` target `SmatchetStandalone`) and the post-shelf test-rig build (`ninja-test-msys2`) count as **distinct gates within the slice** — at-most-once per gate, not at-most-once total. The "wasteful mid-implementation rebuilds" rule the cadence targets still applies; the user-review boundary is not "mid-implementation".

**Perf slice-boundary auto-run — scenario-aware**: when a slice's write set touches files in the curated diff→scenario map at [`agents/perf-gatekeeper.md`](../../agents/perf-gatekeeper.md) § "Curated diff → scenario map", the orchestrator runs the affected scenario(s) at slice boundary (right after the build + ctest pass) and surfaces the top-N rows inline. No need for the user to ask "how is the performance" — Pillar 1 evidence shows up automatically next to the regular slice output. Mechanically:

```bash
bash scripts/dev/perf-run.sh <scenario>  # writes build/perf-runs/<scenario>-<ts>.json
# When docs/perf/baselines/<scenario>.dev.json exists, also:
python scripts/dev/perf-compare.py docs/perf/baselines/<scenario>.dev.json \
    build/perf-runs/<scenario>-<ts>.json --markdown-only
```

When no `dev`-host baseline exists yet (the common case during phase rollouts before bootstrap), the perf-run output is reported on its own as evidence and the orchestrator notes `MISSING_BASELINE` rather than silently passing — same contract as `agents/perf-gatekeeper.md` § Hard rules. The CI gate at `.github/workflows/perf-pr-fast.yml` handles the per-PR enforcement (auto-bootstraps the `ci-windows-latest` baseline on first run, fails subsequent PRs on regression beyond `docs/perf/regression-policy.json`). Local slice-boundary runs are for fast feedback; the CI gate is the merge-block. Skip the local run when the slice ALSO qualifies for the pure-docs skip below — there's no code to measure either way.

**Pure-docs slice skip**: a slice whose write set is strictly within the pure-docs allow-list (`docs/**`, `backlog/**`, `AGENTS.md`, uppercase root `*.md`) skips **both** `cmake --build` and `scripts/dev/test-all.sh` entirely — there is no executable code to verify. Discriminator (mirrors the end-of-session FF-clean gate at `agents/git-janitor.md` § FF-clean docs-batch exception § Pure-docs sub-exception):
```bash
bash scripts/dev/is-pure-docs-diff.sh develop && echo "pure-docs (skip build + test-all)" || echo "needs full gate"
```
Exit 0 → skip both gates. Exit 1 → falls back to the standard slice-boundary cadence above.

The same allow-list governs both surfaces (in-session orchestrator + end-of-session `git-janitor`) so a slice that qualifies in-session also qualifies for the FF push without re-evaluation. Deny-list paths (`agents/**`, `scripts/**`, `tests/**`, `.gitignore`, `.github/**`, `CMakePresets.json`, `CMakeLists.txt`, any C++/Lua/Python/shell source) keep the full gate — even a single deny-list file in the diff disqualifies the slice. CI ([`.github/workflows/build-and-test.yml`](../../.github/workflows/build-and-test.yml) `paths-ignore`) and CodeRabbit ([`.coderabbit.yaml`](../../.coderabbit.yaml) `auto_review.enabled: true`) handle the same diff orthogonally — CI skips on doc-only paths via its own `paths-ignore`; CodeRabbit reviews every PR including doc-only ones.

**Stale-read recovery on `Edit`**: `Edit` may error with `File has been modified since read, either by the user or by a linter` when (a) a concurrent orchestrator in a sibling worktree edited the same file, (b) a `PostToolUse` hook (e.g. `lint-cpp.sh`'s `clang-format -i`) rewrote the file between your `Read` and `Edit`, or (c) the user touched the file in their editor.

Canonical recovery — always works, no manual conflict resolution:

1. Re-`Read` the file at the same path (and same `offset` / `limit` if you used them).
2. Diff your intended change against the new content — verify the `old_string` you were going to pass still exists verbatim. If a hook reformatted it (trailing whitespace stripped, line wrapped), update `old_string` to the new exact form.
3. Re-`Edit` with the refreshed `old_string`.

Hot files (high race rate; expect to re-Read at least once per edit):

- `docs/design/_plan-locks.generated.md` (every orchestrator that takes / releases a plan-lock touches it)
- `AGENTS.md` (multi-agent doc edits)
- `docs/backlog/agent-self-improvement/*.md` (parallel self-improvement appends; concurrent agents shipping different slices in the same session)

Do NOT use `replace_all: true` as a "force-write" — it amplifies race-collision risk by widening the rewrite surface. Stick to the targeted Re-Read + Re-Edit pattern.

The harness maintains a `.claude/.tree-dirty` sentinel file written by `.claude/hooks/lint-cpp.sh` on every first-party `.cpp` / `.h` edit and cleared automatically by the `PreToolUse:Bash` hook (`clear-tree-dirty.sh`) the moment any `cmake --build …` invocation is about to run. Agents reading the sentinel know edits have happened since the last build — if your implementation isn't done yet, defer the build.

The deferred lint pipeline (`.claude/hooks/lint-cpp.sh` PostToolUse → `.claude/hooks/lint-cpp-drain.sh` Stop) follows the same principle for `cppcheck` / `clang-tidy` / dual-target syntax: heavy passes drain once at end-of-turn against the dedup'd set of edited files, not after each Edit/Write. `clang-format -i` still runs inline. Escape hatches: `SMATCHET_LINT_INLINE=1` reverts to per-edit, `bash scripts/dev/lint-flush.sh` drains explicitly mid-turn. The trivial-visual-only envelope above is a special case of this rule.

## Where new rules go

Authored fresh per [`docs/design/agents-md-reduction.md`](../design/agents-md-reduction.md) D7 — the meta-rule that drives where future rules land so the AGENTS.md ↔ `docs/agent-rules/` split doesn't drift.

1. **Build / language / quality 1-liner that every agent must see** (Build, Language, Layout, Logging, Lint, etc. shape) → `AGENTS.md` § Project rules inline. These are 1-line invariants; cross-link follow would cost more than inline reading.
2. **A rule that fits within an existing extracted topic** (`merge-gates.md`, `ux-pillars.md`, `ship-loops.md`, `process-rules.md`) → add to that file. Update the corresponding `AGENTS.md` stub only if the new rule changes the load-bearing essence (the WHAT + must-know invariants the stub names).
3. **A new topic that doesn't fit any existing file AND is > 30 lines** → create new `docs/agent-rules/<topic>.md` (kebab-case) + add a 5-12 line load-bearing stub to AGENTS.md per the Stub format rule (D6 in the reduction plan).
4. **A new topic that doesn't fit AND is ≤ 30 lines** → put it in `process-rules.md` (the catch-all) rather than fragmenting into another file. Fragmentation has a fixed per-file overhead (stub maintenance, cross-link rot, agent context switch); keep it bounded.
