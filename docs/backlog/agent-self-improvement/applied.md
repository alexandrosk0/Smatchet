# Agent self-improvement — applied (archive)

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Closed entries. Archive moves immediately on Status → applied. Sorted by original surface date, latest first.
>
> **Deleted-runtime banner (2026-05-21)** — applied entries below that reference the agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`, `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`, `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`, `CiFailureClassifier`, `dispatch_source` enum, sentinel-file protocol, `agent/<proposalId>` worktrees, `coderabbit-react-loop` design, `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code that **no longer exists** in the tree. The runtime was removed by v1 PR1 of [`../../design/github-tracker-backend.md`](../../design/github-tracker-backend.md) (merge sha `b1d241bc`, 2026-05-21). The future [`../../design/smatchet-merge-watcher.md`](../../design/smatchet-merge-watcher.md) revives a subset of the underlying needs in a different (host-daemon) shape; concepts there are NOT identity-mapped to the deleted runtime. Entries here are preserved as historical record of what was tried.

<!-- Latest first. Append on archival. -->

- 2026-05-21 · orchestrator · [process] · P0 — Draft PRs silently bypass CodeRabbit review; merge-gates poll passes on the placeholder StatusContext without ever seeing a real review
  Resolution: Slice 1 of `docs/design/tooling-process-backlog-sweep.md`. Three-prong fix landed: (1) `MERGE_GATES_FLIP_READY=true` env knob in `scripts/dev/merge-gates.sh` flips draft→ready at poll start when authorized-merge callers (orchestrator + `smatchet-merge-watcher`) opt in. CR's auto-review fires immediately on the ready transition. `agents/git-janitor.md` updated to set the env. (2) The C4 prong 2 logic (`cr_thread_comments_on_head > 0` required for NONE+SUCCESS pass) already shipped in an earlier merge-gates patch — verified in-place at `merge-gates.sh:453`. (3) `@coderabbitai review` manual trigger documented in AGENTS.md § Merge gates and in `merge-gates.sh` header comment. ADR 0006 amended with "Authorized-merge flip-at-poll-start carve-out" section explaining the design. 3 bats tests cover the flip behavior. Plain poll-only callers (status checks, dry-runs) leave the env unset; draft-as-WIP semantics preserved.

- 2026-05-20 · orchestrator · [process] · P2 — ADR 0006 hole: merge-gates poller on draft PR never gets a real CodeRabbit signal
  Resolution: Closed in same slice as P0 sister entry above. ADR 0006 now carries a 2026-05-27 amendment documenting the `MERGE_GATES_FLIP_READY=true` carve-out for authorized-merge callers, closing the structural bypass while preserving draft-as-WIP for non-authorized polls.

- 2026-05-23 · orchestrator · [process] · P2 — Plan-revision direct-pushes vs classifier
  Resolution: Slice 4 of `docs/design/tooling-process-backlog-sweep.md`. Picked policy (b) — mandate PR-for-plan-revisions. AGENTS.md § Process rules § Plan-doc family now states "Plan-revision direct-pushes are PR-only … never direct-push to develop, even for one-line edits." Eliminates classifier-vs-rule drift; adds ~3 min ceremony per plan revision.

- 2026-05-21 · architect · [process] · P2 — Scope-reduction edits need a forward-reference grep over CONTEXT.md + ADRs for deferred-symbol orphans
  Resolution: Slice 4. AGENTS.md § Process rules § Plan-doc family now requires forward-reference grep over `docs/CONTEXT.md` + `docs/adr/` + `agents/*.md` + `docs/backlog/agent-self-improvement/` for every symbol named in deferred work. Same rule applies to substrate/shape/contract rewrites — clear every stray reference in the keyword family.

- 2026-05-21 · architect · [process] · P2 — Cross-cutting review must feasibility-check every prescribed plumbing channel
  Resolution: Slice 4. `agents/architect.md` § Interface contracts checklist now requires naming the actual function signature / struct member / typedef arity at every hop of a prescribed X → Y → Z path; missing channels must be flagged as an explicit widening, not "plumb from context".

- 2026-05-21 · grill-with-docs · [process] · P2 — Skill should pre-flight storage-substrate facts for every persistence claim
  Resolution: Slice 4. `agents/_shared/skills/grill-with-docs/SKILL.md` § During the session now carries a "Storage-substrate pre-flight" rule — any decision mentioning audit / log / persistence / cache / schema / migration must point at one `file:line` proving the storage shape before being locked. Catches phantom-table claims that drive implementation agents to hunt for substrates that don't exist.

- 2026-05-17 · perf-measure · [process] · P2 — "extend the CLI / scenarios if missing, never ask user to run UI manually" rule not encoded in `agents/perf-measure.md`
  Resolution: Slice 4. The rule was already encoded in `agents/perf-measure.md` § Fallback ("Do not fall back to a manual UI session"). Extended `agents/perf-detective.md` § Hard rules and `agents/spike-hunter.md` § Hard rules with the same wording: "Extend the CLI / scenarios, never substitute a manual UI session. … The measurement is the deliverable."

- 2026-05-16 · test-rig · [process] · P2 — Worktree-absolute vs main-repo-absolute path discipline
  Resolution: Slice 4. AGENTS.md § Process rules § Git/p4 discipline now includes the worktree-absolute path rule: when running in `.claude/worktrees/<id>/`, all `Edit` / `Write` absolute paths must use the worktree prefix, never the main-repo prefix. `agents/test-rig.md` § Hard invariants carries the same rule with verification recipe (`git rev-parse --show-toplevel`).

- 2026-05-25 · orchestrator · [process] · P2 — `light-release-unreal-default` plan item 10 (`main.cpp` → `StandaloneAppBootstrap`) deferred without a tracked follow-up slice
  Resolution: Follow-up slice in P4 task stream `light-release-unreal-default` — `InitAppAndPlugins` / `ParseStandaloneCli` / `BootEphemeral` / `ShutdownApplication` in `StandaloneAppBootstrap.{h,cpp}`; GUI render loop uses `bootCtx` + shared plugin init; `--ephemeral` early-outs via `BootEphemeral` (hidden window + forced MCP + `RunRenderLoop`). Process rule added at `docs/agent-rules/process-rules.md` § Deferred plan-file rows at ship boundary. Plan § Deviations updated on task stream. Light build verified (`ninja-publish-light-msvc`).

- 2026-05-23 · debug-detective · [tooling] · P1 — No CI gate prevents BeginTooltip+MarkdownPreviewRender::Render blocks missing opts.wrapWidth
  Resolution: Added `scripts/dev/test-tooltip-wrapwidth.sh` (PR #430). Scans all `Source_Core/src/*.cpp` files; for every `BeginTooltip...EndTooltip` block containing `MarkdownPreviewRender::Render`, asserts `opts.wrapWidth` is also present in that block. Exits 1 if any violation found; auto-discovered by `test-all.sh`. The check catches the exact class of bug that caused ~1.5 h of mis-navigation in session 2026-05-23: `MarkdownPreviewRender::Render` ignores `PushTextWrapPos` and samples `GetContentRegionAvail().x`, which is near-zero in a fresh tooltip window, producing an ultra-narrow vertical strip unless `opts.wrapWidth` is explicitly set. Backtest against the pre-fix codebase confirmed `SmatchetOfflineQueueUi.cpp` would have been flagged — that violation was fixed in the same PR.

- 2026-05-22 · orchestrator · [tooling] · P3 — retype change @2 revs to `text+w` so the dual-VCS ReadOnly contract holds across the whole baseline history
  Resolution: Already shipped on 2026-05-21 by depot changes @3 ("chore(p4): retype @2 baseline t…") and @4 ("chore(p4): catch-all text+w typ…") — the backlog entry was raised on 2026-05-22 as a defensive note but never archived. Verified on the live Mainbot depot during the 2026-05-22 perforce-recovery session: `p4 -c smatchet_main_alexk files //smatchet/main/...` reports **689 `text+w` / 116 `text+wx` / 14 `binary+w` / 1 bare `text`** across 820 revs at HEAD. The lone bare-`text` holdout is `//smatchet/main/log#2` — a delete-tombstone (delete change @5) for an accidentally-tracked log file, with zero `p4 sync` impact (a fresh client never lands it). Spot-check via `p4 fstat -T 'depotFile,headType'` confirms representative live files (`CMakeLists.txt`, `Source_Core/include/AppController.h`, `.gitignore`) → `text+w`; `scripts/dev/test-all.sh` → `text+wx`. Closing as a docs cleanup; no further depot work needed.

- 2026-05-20 · orchestrator · [process] · P2 — Slice 1 of `pillar-1-2-perf-review-system.md` never captured initial dev-host baselines; perf-gatekeeper verdicts permanently MISSING_BASELINE
  Resolution: 7 of 15 candidate scenarios baselined on `chore/backlog-best-wins`@dev — `docs/perf/baselines/{idle,priority-grid-scroll,attachment-preview-open,long-text-open-large-adf,lua-recorder-fuzz,preferences-slider-drag,ai-chat-history-render}.dev.json`. Captured against `c276d884` (develop tip at slice start) via `bash scripts/dev/perf-baseline.sh init <scenario>` per scenario. The remaining 8 scenarios named in the original entry don't emit `rows[]` under their current shape (3 bucket-C screenshot scenarios that require `--screenshotPath`, 3 roundtrip scenarios that run cleanly but don't include `UiPerfMonitor::Instance().GetLastFrameRows()` in OnFinish, 2 misc — `cell-edit-burst` + `whisper-ai-assistant-autosend`). New follow-up tracked at tooling.md "8 of 15 candidate perf scenarios don't emit rows[]" with the per-scenario retrofit plan. Net effect: `perf-gatekeeper` + the new AGENTS.md § Project rules § "Perf slice-boundary auto-run" rule now produce real deltas (not `MISSING_BASELINE`) for the 7 covered surfaces; the 8 surfaces remain advisory-only until their scenarios are retrofitted.

- 2026-05-19 · debug-detective + test-author · [process] · P1 — Bootstrap-golden screenshot tests can enshrine the bug they were meant to catch
  Resolution: AGENTS.md § Project rules now carries the "Golden-image approval contract" rule (after § Trivial-visual-only change envelope) — agents that bootstrap a checked-in reference artefact MUST hand the file + launched-app handle to the user and wait for explicit "looks right" verdict before `git add`. Iterate the underlying fix on rejection, never amend the golden to match a buggy state. Cross-linked from `agents/test-author.md` § Pattern C — Screenshot scan with a "Golden-image bootstrap requires user approval" sub-bullet + dual-capture-no-golden preference (`scripts/dev/test-theme-roundtrip.sh` reference). Motivated by the 2026-05-19 theme-switch-roundtrip incident where the bootstrap would have shipped a PNG of the bug.

- 2026-05-18 · debug-detective · [tooling] · P2 — Smatchet Logger has no console output + no default file sink
  Resolution: `Target_Standalone/main.cpp` default-wires `Logger::Instance().SetFileSinkPath()` right after the SMATCHET_USER_DATA env-override block, BEFORE `ConfigManager::Load`. Path resolution honours `SMATCHET_DEBUG_LOG` env (operator override) first; falls back to `%LOCALAPPDATA%\Smatchet\Smatchet-<pid>.log` on Windows (per-PID suffix so concurrent ephemeral + manual instances don't trample shared sinks); `$TMPDIR/Smatchet-<unix-ts>.log` on POSIX. The wire emits a self-referential `LOG_INFO "Logger file sink: <path>"` as the first entry so debug sessions read that line to know which file to tail. In-memory deque + in-app Log window paths untouched. Smoke-tested both code paths — `Smatchet.exe --help` produces the log; `SMATCHET_DEBUG_LOG=/tmp/x.log` override is honoured.

- 2026-05-20 · orchestrator · [tooling] · P2 — Required CLI tools must be discoverable + verified at first-setup time
  Resolution: New `scripts/dev/check-required-tools.sh` probes the standard orchestrator tool set (git, cmake, ninja, gcc, g++, python, jq, gh, clang-format, clang-tidy, cppcheck, flock; optional: OpenCppCoverage). PASS/FAIL summary; exits 1 on any missing required tool. Wired as the first step of `scripts/setup-harness.sh` so the absence fires at clone time — continues setup if a tool is missing so the user can finish adapter wiring + install separately. New "Required CLI tools" section in `docs/harness/SETUP.md` with per-tool install hints (MSYS2 + winget). Probe is also safe to invoke ad-hoc (`bash scripts/dev/check-required-tools.sh [--quiet]`). Validated against the local dev box: immediately flagged jq + flock as missing (matching the merge-gates-poller false-pass + lint-cpp-drain.sh gaps from the prior session).

- 2026-05-20 · orchestrator · [tooling] · P2 — Running Smatchet.exe holds the file lock; rebuild link fails until the user kills the window
  Resolution: New `scripts/dev/relaunch-smatchet.sh` wraps the iter-loop: (1) probes `tasklist /FI "IMAGENAME eq Smatchet.exe"` and `taskkill //IM Smatchet.exe //F` if running (gated on `tasklist`'s presence so non-Windows hosts no-op cleanly), (2) `cmake --build --preset <preset> --target SmatchetStandalone` (default ninja-iter-msvc; --preset=debug/publish supported), (3) `ls -la` the resulting exe for stale-exe sanity, (4) launches with `nohup ... &` to a per-launch `${TMPDIR:-/tmp}/Smatchet-relaunch-$$.log` so the caller can tail-f while iterating. `--no-launch` for build-only; `--args="..."` for verbatim arg passthrough. Idempotent PATH prepend for MSYS2 UCRT64 so the script works without manual export. Returns the launched PID on stdout.

- 2026-05-17 · code-review · [test] · P1 — `scripts/dev/test-screenshot-diff.sh:104` `sleep 0.5` race between spawn-mode CLI return and PPM read
  Resolution: Already fixed before this archival pass — `scripts/dev/test-screenshot-diff.sh:130-133` ships the deterministic poll loop (`for _ in $(seq 1 40); do [ -s "$captured" ] && break; sleep 0.05; done`) the backlog entry's "Concrete next action" prescribed. Likely landed in PR #165 (`fix(hotfix-batch): six P0/P1 findings from retrospective sweep`) but the backlog entry wasn't archived. Confirmed 2026-05-20 via `grep -n "sleep 0.5" scripts/dev/test-screenshot-diff.sh` → no matches. Archiving as a cleanup of stale backlog entries; no code change required.

- 2026-05-20 · orchestrator · [process] · P2 — New-feature plans must include a `## Perf` section with before/after measurement + temp markers
  Resolution: Slice 1 of `docs/design/pillar-1-2-perf-review-system.md` ships the substrate (baseline registry at `docs/perf/baselines/<scenario>.<host>.json`, regression policy at `docs/perf/regression-policy.json`, driver scripts `scripts/dev/perf-{run,baseline,compare}.{sh,py}`). `docs/PERF_WORKFLOW.md` § Step 7 documents the gate-check loop. `AGENTS.md` § UX Pillars § Pillar 1 cross-links the new substrate. The `## Perf section mandate` itself (the original entry's deliverable) is left for Slice 3 (PR-fast CI gate) of the same plan — that's where the gate becomes merge-blocking and the AGENTS.md § Project rules mandate lands. The substrate must exist first; mandating a `## Perf` section that has no tools to satisfy it would have been an empty rule. Closing this entry as the upstream design (the plan) is now on develop + the foundation is shipping in the same window.


  Resolution (status: observational -> applied): The underlying race itself is upstream — no single-mechanism in-repo fix. But the canonical recovery pattern is now documented in AGENTS.md § Project rules § Stale-read recovery on Edit (per grill Q5=A — top-level Project-rules visibility, auto-loaded for every agent every session). Documents the 3 causes (concurrent orchestrator in sibling worktree, PostToolUse hook reformat, user-side edit) + the 3-step recovery (re-Read at same offset/limit → diff intended change against new content → re-Edit with refreshed old_string). Names 3 hot files where the race rate is highest (`_plan-locks.generated.md`, AGENTS.md, `docs/backlog/agent-self-improvement/*.md`). Explicit "do NOT use replace_all: true as a force-write" callout because that widens the rewrite surface and amplifies collision risk. Plan: `docs/design/process-backlog-tighten-1-2-3-9-11-12.md` § Slice 6.

- 2026-05-16 · orchestrator · [process] · P3 — Plan-doc file-level tables drift from grep ground truth; re-verify before sealing
  Resolution: New `docs/agent-rules/delegation.md` § Orchestrator delegation packet § File-level table re-verify (before sealing) sub-section documents the 2-command probe (`git grep -nF -- "<symbol>"` for callers / definers; `grep -nE "(set|file\s*\(\s*GLOB).*<var>" CMakeLists.txt tests/CMakeLists.txt` for CMake variables — flagging GLOB-populated variables for row-drop). Target file = delegation.md (not AGENTS.md) per grill Q3=B and the codebase probe that revealed § Orchestrator delegation packet body lives in delegation.md (AGENTS.md carries only a one-line index pointer at line 264). New `scripts/dev/plan-doc-table-probe.sh <plan-path>` ships in v1 (also per grill Q3=B): parses § File-level changes section, recognises Symbol / Function / Caller / Definer / CMake variable / CMake target / Variable column headers (case-insensitive), runs both probes per row, reports hits / misses, exit 0 on all-resolved or no-tables-found, exit 1 on any miss. Self-test `scripts/dev/test-plan-doc-table-probe.sh` covers 6 cases (no section / unrecognised columns / valid symbol / missing symbol / CMake GLOB variable / mixed valid+invalid): 9/9 PASS. Auto-enrolled by `scripts/dev/test-all.sh` glob. Plan: `docs/design/process-backlog-tighten-1-2-3-9-11-12.md` § Slice 4.

- 2026-05-16 · orchestrator · [process] · P3 — API-500 mid-run recovery procedure not documented
  Resolution: New `docs/agent-rules/delegation.md` § API-500 mid-run recovery — 5-step procedure (inspect worktree state → run local gates → `git add -A` + commit → push + open draft PR → backlog entry per Slice 5 grill Q8=A). The `git add -A` (not `git add <list>`) gotcha is named explicitly. AGENTS.md § Delegation index pointer-line updated to include the new sub-section. New AGENTS.md § Project rules § Force-push carve-out for spawned-agent recovery (per grill Q4=A — covers `agent/<id>` AND `claude/<id>` spawned-agent branches only; `--force-with-lease` required, bare `--force` still banned; excludes develop/main and orchestrator branches `chore/feat/fix/docs/wip/*` and mixed-authorship branches). New ADR `docs/adr/0005-force-push-carve-out-for-spawned-agent-recovery.md` (per grill Q9=A — passes hard-to-reverse + surprising-without-context + real-trade-off triad) records the carve-out rationale with three rejected alternatives (always-new-commit; full ban + manual rescue; broader scope to all-your-branches). Plan: `docs/design/process-backlog-tighten-1-2-3-9-11-12.md` § Slice 5.

- 2026-05-19 · git-janitor · [process] · P2 — FF-clean docs-batch exception still gates on `bash scripts/dev/test-all.sh` for pure-docs / design-only diffs
  Resolution: `agents/git-janitor.md` § FF-clean docs-batch exception gets a new sub-section "Pure-docs sub-exception (precondition 4 relaxation)" — when the ahead-range diff is strictly within `docs/**`, `backlog/**`, `AGENTS.md`, or root-level UPPERCASE `*.md`, the `test-all.sh` gate is skipped (per grill Q6=A — Locales/*.json stays under AGENTS.md § Trivial-visual-only change envelope, not merged here; agents/*.md stays in deny-list so `test-agent-contract.sh` still fires). Allow-list and deny-list explicitly named so the rule doesn't over-fire. New `scripts/dev/is-pure-docs-diff.sh <base-branch>` discriminator — exits 0 if ahead-range strictly allow-listed, 1 otherwise. Verified live on the current branch (mixed scripts/+agents/ diff returns exit 1 as expected). Plan: `docs/design/process-backlog-tighten-1-2-3-9-11-12.md` § Slice 2.

- 2026-05-19 · orchestrator · [process] · P1 — Autonomous ship-loop fires commit+push during iterative UI/visual polish, before user can validate
  Resolution: AGENTS.md § Autonomous ship-loop default § Exceptions gets new exception #5 "Visual-validation exception" — fires when BOTH (1) diff touches a visual-path file in the allow-list (`SmatchetTheme.cpp/h`, `Smatchet*Ui*.cpp`, `Locales/*.json`, ImGui style constants, dock-layout init) AND (2) no bucket-C/E coverage exists for the changed widget. Loop pauses after build with the launched exe; orchestrator presents the run command + bash background id + one-sentence "evaluate this" and waits for user verdict. On "looks good" → resume + commit. On "no" → leave dirty + iterate in-place (grill Q7=A; not stash, since the stash-list bookkeeping is more friction than value when the orchestrator already has the diff in context). Out-of-scope cases explicitly named so the exception doesn't over-fire: uncovered-but-non-visual changes route via the test backlog instead; visual changes that DO have coverage stay in the ship-loop. Pillar anchor added to AGENTS.md § UX Pillars § 4 § Visual-validation acceptance with a cross-link back to the exception. `agents/git-janitor.md` § Hard refusals extended with a "Squash-merge a PR carrying unvalidated visual commits — never" rule that defers to the merge-gates poller's user-comments gate (or asks if no comment present). Plan: `docs/design/process-backlog-tighten-1-2-3-9-11-12.md` § Slice 1.

- 2026-05-18 · orchestrator · [process] · P2 — Pushing commits to a merged-PR branch silently orphans them
  Resolution: New `scripts/git-hooks/pre-push` (Slice 3 of `docs/design/process-backlog-tighten-1-2-3-9-11-12.md`) — single-file executable hook (collapsed from plan's split, per grill Q2=A). Skips silently on develop/main/detached-HEAD, missing `gh` CLI, no-PR, or OPEN PR. Refuses on MERGED / CLOSED with a recovery banner naming the exact `git checkout -b <new-branch> origin/develop` + cherry-pick recipe. `SMATCHET_ALLOW_MERGED_PR_PUSH=1` override available for the rare legitimate case (logged loudly). Wired via `scripts/setup-harness.sh` new `install_git_hooks()` which sets `git config --local core.hooksPath scripts/git-hooks` only when current path is unset or already equal — won't trample a user-set custom hooks path; otherwise logs WARNING. Bucket-A test `scripts/dev/test-pre-push-merged-pr-guard.sh` covers 9 cases (develop/main/HEAD skip · no-PR skip · OPEN-PR skip · MERGED refuse + banner + recovery text · CLOSED refuse · override allow + warning · missing-gh silent skip): 12/12 PASS. Auto-enrolled by `scripts/dev/test-all.sh` glob. Plan: `docs/design/process-backlog-tighten-1-2-3-9-11-12.md` § Slice 3.

- 2026-05-13 · orchestrator · [external] · P3 — vexp `<!-- vexp -->` block auto-regenerates inside `AGENTS.md`; should land in `.claude/CLAUDE.md` instead
  Resolution: In-repo workaround landed — `scripts/dev/vexp-strip-agents-md.sh` idempotently removes the vexp block (delimited by `## vexp <!-- vexp vX.Y.Z -->` … `<!-- /vexp -->`) from `AGENTS.md` on every Claude Code session start. Wired in `docs/harness/claude-code/settings.json.tmpl` § `SessionStart` (and the live `.claude/settings.json` for this session). Manual one-shot strip applied to the current `AGENTS.md`; AGENTS.md § Semantic-search exceptions reworded to drop the now-stale "below" reference; new § vexp — Claude-Code-only section names the canonical location (`.claude/CLAUDE.md`, sourced from `docs/harness/claude-code/CLAUDE.md.tmpl`) + the rationale that Codex / Cursor / Aider read AGENTS.md per the agents.md spec and can't use Claude-Code-only MCP guidance. Idempotency verified: re-running the script on a clean `AGENTS.md` is a no-op (exit 0, zero matches). Upstream issue against the vexp tool is a user task (draft text in `docs/design/unblock-external-blockers-2-3-4.md` § Slice 2 step 5). Plan: `docs/design/unblock-external-blockers-2-3-4.md` § Slice 2.

- 2026-05-12 · tracker-backend · [external] · P3 — `mcp__vexp__run_pipeline` rejects `max_tokens` as float when JSON wire format is double
  Resolution: In-repo audit confirmed zero real float-literal callsites (the only `max_tokens:\s*\d+\.\d+` matches in the repo are inside this entry + the plan doc that quotes it). Pinned the int-literal-only convention in `docs/harness/claude-code/CLAUDE.md.tmpl` § Advanced Parameters by appending **integer literal only** + the exact daemon error text `"floating point, expected usize"` to the existing `max_tokens: 12000` bullet so future agents don't re-trip the bug. The template is the source for `.claude/CLAUDE.md` regen via `scripts/setup-harness.sh claude-code`. Upstream issue against the vexp tool is a user task (draft text in `docs/design/unblock-external-blockers-2-3-4.md` § Slice 3 step 3). Plan: `docs/design/unblock-external-blockers-2-3-4.md` § Slice 3.

- 2026-05-16 · orchestrator · [external] · P2 — Auto-merge disabled on the repo; `gh pr merge --auto` errors
  Resolution: Workaround option (a) chosen — `gh api -X PATCH repos/alexandrosk0/Smatchet -F allow_auto_merge=true -F delete_branch_on_merge=true` flipped both settings in one call (`{"allow_auto_merge":true,"delete_branch_on_merge":true}` verified). Branch protection on `develop` is absent (HTTP 404 on `gh api repos/.../branches/develop/protection`), so `--auto` degrades to instant-merge with no required check — noted but out of scope (separate branch-protection decision). `docs/design/applied/test-suite-expansion-completion.md` § Deviations from plan § Session 2026-05-16 updated with a parenthetical naming the re-enable date + the rationale that the AGENTS.md § Merge gates poller remains the canonical autonomous path because it also covers CodeRabbit + user comments, which `--auto` does not. Plan: `docs/design/unblock-external-blockers-2-3-4.md` § Slice 1.

- 2026-05-19 · orchestrator · [tooling] · P2 — Token telemetry does not record Claude Code skill-load overhead
  Resolution: Empirical probe (PostToolUse matcher="Skill" + a throwaway dump-stdin hook) confirmed that **PreToolUse and PostToolUse both fire for the `Skill` tool**, with `tool_input.skill` carrying the skill identifier and PostToolUse adding `duration_ms` + `tool_response.success`. Claude Code does **not** expose token-usage on the payload — skills load inline into the parent context, so the cost shows up only as the parent's next-turn input-token delta. `agents/_shared/token-tracking/skill-load-log.py` (PostToolUse:Skill hook) writes one JSONL line per invocation to `.claude/.skill-loads.jsonl` with `{ts, session, skill, duration_ms, success, approx_tokens, skill_md_path}`. `approx_tokens` is the file-size-divided-by-4 heuristic — populated for project skills (resolvable via `.claude/skills/<name>/SKILL.md`); 0 for namespaced plugin skills whose paths live in the per-user plugin cache and aren't enumerable from the hook. `agents-statusline.py` gains a `_skills_badge()` block — `[SKILLS] 🧠 skill×N · loads=M [· ~Kt]`. Wired in `docs/harness/claude-code/settings.json.tmpl` + linked by `setup-harness.sh setup_claude_code()` (new `link_file ".claude/hooks/skill-load-log.py" → agents/_shared/token-tracking/skill-load-log.py`). Bucket-A test `scripts/dev/test-skill-load-log.sh` (auto-enrolled in `test-all.sh`) covers: PostToolUse appends 1 row, PreToolUse silently ignored, non-Skill tool_name ignored, plugin-namespaced skill resolves null path + 0 tokens, project skill resolves path + non-zero tokens, setup-harness link present. 9/9 PASS live. Limitation called out in script docstring: `approx_tokens` is a heuristic — use for relative comparison (skill A vs skill B), not absolute budget accounting. Real measurement awaits Anthropic exposing a token-usage payload on the Skill tool's PostToolUse event.

- 2026-05-19 · orchestrator · [tooling] · P3 — `test-doc-anchors.sh` advisory → blocking flip (4 known broken refs)
  Resolution: Same-PR closure of the advisory-mode entry. All 4 known broken refs fixed: (1) `Anti-deception note` — new bold-prefix paragraph added to AGENTS.md § Handoff envelope describing the `HarnessRunState::IsTransitionAllowed` integrity boundary that the Source_Core headers reference. (2) `Auto-merge mechanics` — ref in external-blockers.md rewritten from `AGENTS.md § Auto-merge mechanics` to `docs/design/applied/test-suite-expansion.md § Auto-merge mechanics` (the actual location). (3) `Git workflow` — ref in tooling.md rewritten from `AGENTS.md § Git workflow` to `AGENTS.md § Project rules` (with cross-mention of the existing § Destructive git ops in shared worktrees sub-section). (4) `Pillar 2` — added `**Pillar 1**` / `**Pillar 2**` / `**Pillar 3**` / `**Pillar 4**` bold-prefix anchors inside AGENTS.md § UX Pillars subsections so the literal name "Pillar 2" resolves. Advisory mode flipped to blocking — `test_doc_anchors.py` now exits 1 on any unresolved ref. Final audit run: 45/45 refs resolved (was 43/47 broken on initial run).

- 2026-05-19 · orchestrator · [process] · P3 — AGENTS.md § <section> cross-link rot when sections move
  Resolution: `scripts/dev/test-doc-anchors.sh` (+ helper `scripts/dev/test_doc_anchors.py`) landed via PR #TBD. Collects valid anchors (headings + bold-prefix paragraphs) from `AGENTS.md` and every `docs/agent-rules/*.md`; greps every `AGENTS.md § <Name>` reference repo-wide; reports refs whose extracted name doesn't substring-match any anchor. Initial run found **4 legitimate broken refs** (Anti-deception note, Auto-merge mechanics, Git workflow, Pillar 2). Shipped in **advisory mode** (exit 0 with warnings) per Phase 9 coverage-gate soak pattern — flips to blocking once the 4 known refs are fixed; tracked as P3 `tooling.md` 2026-05-19. Auto-enrolled by `scripts/dev/test-all.sh`. 152 distinct anchors detected; 47 distinct refs scanned; 43 resolved (91%) on initial run.

- 2026-05-19 · orchestrator · [process] · P2 — Wrong-worktree-path footgun recurs despite documentation
  Resolution: Option (b) from the entry — Stop-time audit script — landed via PR #TBD. `scripts/dev/check-main-repo-clean.sh` runs from the Claude Code Stop hook (wired via `docs/harness/claude-code/settings.json.tmpl`); resolves the main repo via `git rev-parse --git-common-dir`, runs `git -C <main-repo> status --short`, and emits a loud STDERR banner if non-empty. Banner names the modified files + the worktree branch + the exact `git -C "$MAIN_REPO" stash push` recovery command. Best-effort: exit 0 always, never blocks Stop. Smoke-tested with a synthetic dirty marker — fires correctly. The harness setup script template propagates the new hook on next `bash scripts/setup-harness.sh claude-code` run; live `.claude/settings.json` updated in the same PR for this session. PreToolUse-rewrite option (a) deferred — option (b) catches the bug at session-end which is sufficient for the documented friction pattern.

- 2026-05-19 · orchestrator · [process] · P3 — `link_file()` short-circuits forever when destination exists; canonical edits don't propagate to `.claude/hooks/`
  Resolution: Option (b) from the entry — audit check in `scripts/dev/test-agent-contract.sh` — landed via PR #TBD as sub-check [9/9]. `cmp -s` between `agents/_shared/token-tracking/agent-token-log.py` (canonical) and `.claude/hooks/agent-token-log.py` (independent copy) fails loudly on byte drift. Skipped cleanly when the hook copy is absent (Claude Code harness not set up locally). Recovery command included in the failure message: `cp -f canonical hook_copy` OR `rm hook_copy && bash scripts/setup-harness.sh claude-code`. Audit run total: 19/19 sub-checks now (was 18/18). Option (a) `link_file()` drift detection in setup-harness.sh deferred — option (b) catches the bug at PR time which prevents the misclassification from shipping.

- 2026-05-19 · orchestrator · [process] · P3 — Agent-docs improvements: 4 parked items deferred from PR #260
  Resolution: All 4 parked items shipped as separate PRs after the PR #260 tracking entry was filed. PR #269 closed lock-terminology clarification (4-roles glossary). PR #270 closed CONTEXT.md glossary expansion (9 → 41 entries / 7 sections). PR #273 closed AGENTS.md three-way split — scoped down to a single-file extraction of § Delegation (~230 lines) to `docs/agent-rules/delegation.md` after audit found ~74 external `AGENTS.md § <section>` references; redirect stub in AGENTS.md preserves the cross-links. vexp-section-to-CLAUDE.md remains external-blocker (vexp installer auto-regenerates inside AGENTS.md). Plan revision appended to `docs/design/agent-docs-improvements.md` § Implementation log per AGENTS.md § Plan revision after implementation.

- 2026-05-18 · orchestrator · [process] · P2 — Worktree bootstrap branches start on stale base (not origin/develop)
  Resolution: Re-scoped during plan double-check pass — the original backlog entry's `scripts/dev/worktree-spawn.sh` does not exist (Glob 0 hits). Two worktree-creation paths surfaced — (1) `Source_Core/src/ClaudeCodeLocalRunner.cpp` for agentic-handoff `agent/<proposalId>` worktrees (already correct, bases on `origin/develop` with `handoff.auto_fetch_before_worktree` config flag); (2) Claude Code SDK's session-spawn for `claude/<id>` worktrees (the Phase D/E pain — bases on parent repo local HEAD). Investigation of `git config --local` confirmed `extensions.worktreeconfig=true` is enabled but the base-selection happens before config applies. **Escalated to external-blocker** for SDK upstream. **Workaround documented** in `docs/harness/SETUP.md` § Worktree base — known stale-HEAD pitfall (two-track: parent-on-develop pre-session, OR rebase-on-origin/develop first-move post-session).

- 2026-05-18 · security-review · [process] · P2 — `agents/security-review.md` attack-surface map missing AI feature surfaces
  Resolution: Two new bullets added to `agents/security-review.md` § Smatchet attack surface (after "Image fetches", before "Known crash classes"). **AI feature surface** bullet enumerates the 5 backlogged components (provider HTTP clients — OpenAi/Anthropic/Ollama; streaming parsers — `AiSseParser` / `AiNdjsonParser`; `AgentsMdLoader`; `AiContextBuilder`; `AiAssistantController`) with per-client checks (URL allow-list via `AiEndpointSanitize`, error-body redaction via `AiErrorRedact`, buffer caps, path validation, Lua `ai.*` rate limit, `AssistantContextBlockAuditTrail` default `false` invariant). **Coding-harness handoff surface** bullet (new — surfaced during plan double-check pass) covers the `ClaudeCodeLocalRunner` + sentinel files + spawned-claude subprocess trust boundary introduced by `docs/design/agentic-coding-handoff.md`: env allow-list discipline (only `PATH, HOME, USER, USERPROFILE, TEMP, TMP, SYSTEMROOT, GH_TOKEN, GITHUB_TOKEN, ANTHROPIC_API_KEY`; no `SMATCHET_*` passthrough), sentinel-file write contracts per AGENTS.md § Handoff envelope, branch-name discipline (`agent/<proposalId>/<short-slug>` only), PR draft requirement, GH PAT scope, worktree GC. Frontmatter `description:` updated to mention both new surfaces so the agent's discovery banner names them.

- 2026-05-18 · orchestrator · [process] · P2 — Autonomous ship-loop policy lives only in private user memory; not in `AGENTS.md`
  Resolution: New top-level section `## Autonomous ship-loop default` added to AGENTS.md between § UX Pillars and § Project rules (after L71). Codifies the full sequence (diagnose → fix → build → commit → push → open PR → squash-merge → git-janitor cleanup → backlog entry, all in one turn), the front-loaded `AskUserQuestion` discipline (batch once at the start), and the exception list (debug-mode pause-loop override, destructive ops, cross-repo / external-service mutations, anything not previously authorised in a durable rule). Cross-links to § Delegation and § Debug-mode pause-loop. Other harnesses (Codex / Cursor / Aider) reading AGENTS.md now get the rule without depending on user-private Claude Code memory.

- 2026-05-18 · orchestrator · [process] · P3 — Post-ship next-step prompt rule (`AskUserQuestion` not bulleted list) not encoded in `AGENTS.md`
  Resolution: Sub-section `### Post-ship turn-end protocol` added under `## Autonomous ship-loop default`. Names the four canonical options as `AskUserQuestion` choices — Manual verify / Review PR / Squash-merge / Done — and the skip-condition ("user has already said 'no more changes coming' / 'ship it and stop' / 'merge when green' → hand off to git-janitor directly"). Batched in the same commit as the parent ship-loop section. Other harnesses now have a portable rule; free-form bulleted next-steps lists deprecated.

- 2026-05-18 · test-author · [infra] · P2 — `AiClientFactory` test-injection seam + `runProbe` extraction
  Resolution: PR `feat/ai-client-test-override` lands `AiClientFactory::SetTestOverride(TestOverrideFn)` — bucket-E TUs inject a stub `IAiClient` (success / gated). `runProbe` body lifted from `SmatchetPreferencesUi.cpp:520-680` into `AiPrefsTestConnection::TriggerProbe(d, app, provider)` under `Source_Core/{include,src}/AiPrefsTestConnection.{h,cpp}`. `UiTestScenario.cpp` exposes `SmatchetActiveUiTestAppController()` so test funcs can reach `app.mainThreadDispatcher`. `tests/ui/ai_prefs_autosave_flow.test.cpp` V2 + V3 lift from deferred placeholders to live coverage — V2 asserts `Verified.` success path against stub; V3 exercises cancel-on-close short-circuit. Follow-up open: `SmatchetPreferencesUi.cpp`'s inline `runProbe` lambda still needs the 5-line rewire to call `AiPrefsTestConnection::TriggerProbe` (held by `whisper-dictation-phase-f` PR #219 lock; rewire fires after #219 merges).

- 2026-05-18 · orchestrator · [process] · P1 — `git reset --hard origin/develop` via `git -C <worktree>` reset the worktree's *current* branch, not `develop`; destroyed 5 uncommitted modified files of a parallel agent
  Resolution: Mandatory 5-step pre-flight (branch verify → status inventory → stash → execute → decide on pop) landed in `AGENTS.md` as a new sub-section **"Destructive git ops in shared worktrees"** under § Project rules, directly after § Plan-doc safety. The rule names the four destructive ops (`reset --hard`, `checkout --`, `clean -f`, `branch -D`) and the `git -C <path>` discipline. Cross-link added to `agents/git-janitor.md` § Hard refusals (the agent most likely to hit this case) calling back to AGENTS.md for the authoritative checklist. Root cause documented inline: parallel agents reassign worktree HEADs between sessions, so the path name (e.g. "develop-worktree") is **not** authoritative for which branch is currently checked out — `git -C <path> branch --show-current` first, every time. Underscored fact: `reset --hard` permanently destroys uncommitted tracked-modified content (not in reflog); branch pointers recover, working-tree content does not.

- 2026-05-17 · orchestrator · [tooling] · P1 — `--spawn` ephemeral MCP instance times out at 15s on develop tip (post Phase D/E AI merges)
  Resolution: Cheap-fix path (a) shipped. `--spawn` ready-timeout bumped 15s → 30s in `Target_Standalone/CliCommandRunner.cpp:670`. New env override `SMATCHET_SPAWN_READY_MS=<ms>` lets faster runners tighten or cold-cache CI raise further. Bucket-E gates (`scripts/dev/test-ui-*.sh`, `scripts/dev/test-callstack-tooltip-hover.sh`) unblock. Architectural follow-up (path (b) — lazy-load AI clients so MCP server publishes ready faster) split to a new P3 entry under `tooling.md` since the cheap fix removes the merge-block urgency.

- 2026-05-17 · security-review · [security] · P1 — Default `AssistantContextBlockAuditTrail = true` silently exfils audit-trail PII (default flipped; consent modal deferred)
  Resolution: `Source_Core/include/ConfigManager.h:248` default flipped to `false`. New users no longer auto-ship `BackendAuditTrail::ReadRecentEvents` PII (assignee emails, custom-field values, freeform comments) on first AI prompt. Existing users retain their persisted setting (`j.value(...)` Load semantics preserve already-saved configs). One-time first-send consent modal split to a new P2 entry under `security.md` (downgraded P1→P2 since the riskiest default is now off).

- 2026-05-17 · code-review · [bug] · P1 — `CommandPaletteFuzzyScenario` flips `BackendHasBeenReachable=true` before `outErr` early-return guard
  Resolution: Verified `Source_Core/src/Commands/Scenarios/CommandPaletteFuzzyScenario.cpp` already has the screenshotPath empty-check `return` ahead of the latch flip (L60-63 before L72-73). Added a clarifying comment naming the invariant so any future outErr branch added between the path check and the flip is flagged at review.

- 2026-05-17 · code-review · [bug] · P1 — `Source_Core/include/AppController.h:660-693` asymmetric `override` keyword guarding under `SMATCHET_WITH_LUA_AUTOMATION` (false positive)
  Resolution: No code change. Audited every `override` in `AppController.h`. The two sites originally flagged (`FindFieldById` L698-702, `SubmitFieldEdit` L722-727) follow the correct shape: declarations always present, `override` keyword wrapped in `#if defined(SMATCHET_WITH_LUA_AUTOMATION)`. When LUA=OFF the keyword is elided and the declaration becomes a regular non-virtual method, which compiles cleanly. The cited L660-693 range contains zero `override` interactions. Other `override` sites (L123, L346-376) all sit inside larger `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` blocks where the base class `ILuaBindingHost` is available. Entry archived as false positive.

- 2026-05-17 · code-review · [tooling] · P3 — `ai.dump-request` debug body / URL drifted from OpenAi wire post PR #184
  Resolution: PR #184 (`batch 2`) updated `OpenAiClient::BuildChatBody` to always emit `max_tokens = 4096` and `OpenAiClient::ResolveBaseUrl` to strip a trailing `/v1` / `/v1/`. The parallel debug builders in `Source_Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp` (`BuildOpenAiBody`, `ResolveEndpointUrl`) were not updated, so `ai.dump-request` for `openai` + `ollama-openai` misreported the wire — no `max_tokens` field, and `http://localhost:1234/v1` showed `/v1/v1/chat/completions`. Anthropic dumper was already correct. Fix mirrors the wire path: `BuildOpenAiBody` gains a `maxTokens` param defaulting to 4096; new `StripOpenAiV1Suffix` helper applied to the OpenAi / OllamaOpenAiCompat branch of `ResolveEndpointUrl`. Same TU also picked up four pre-existing cppcheck / clang-tidy nits (`uselessCallsSubstr`, two `useStlAlgorithm` raw-loops folded into nlohmann's vector-conversion, `PInt` / `PString` using-decls moved inside `#if defined(SMATCHET_WITH_AI)` so the stub build is clean).

- 2026-05-17 · code-review · [bug] · P0 — `.gitattributes` did not declare `*.ppm binary`
  Resolution: `.gitattributes` now declares `*.ppm binary` alongside existing image-binary rules. Defense in depth — the bulk PPM goldens that originally triggered the risk were migrated to PNG in the same batch so the rule applies only to any future bootstrap captures that get accidentally committed before the writer change is picked up.

- 2026-05-17 · code-review · [infra] · P0 — `tests/golden/*.ppm` 5.5 MB raw PPMs bloat pack
  Resolution: Migrated PNG via stb_image_write — vendored `ThirdParty/stb/stb_image_write.h` (single-TU impl in `Target_Standalone/main.cpp`), updated `tests/support/GoldenImage.h` + `ScreenshotDiffMain.cpp` to read PNG via stb_image, updated `scripts/dev/test-screenshot-diff.sh` to use `.png` extensions + ephemeral port + deterministic poll, refreshed `tests/golden/README.md`. Old PPMs deleted; new PNGs ~280-310 KB each (≈10× smaller than the 2.76 MB raw P6s). Bootstrap path tested end-to-end via `bash scripts/dev/test-all.sh`. lint-cpp hook adjusted to skip clang-tidy on `tests/support/*` (no compile_commands entry).

- 2026-05-17 · code-review · [security] · P1 — `OpenAiClient.cpp:140-180` API key leak via raw provider error body
  Resolution: Provider error body now flows through `smatchet::ai::pure::RedactProviderErrorBody` before being appended to `AiStreamError::Message`. Helper lives in a sibling Pure TU (`Source_Core/{include,src}/AiErrorRedact.{h,cpp}` — no cpr/httplib/SQLite includes) per AGENTS.md § Pure-helper TU-split recipe. Strips Bearer tokens, `api_key` / `apiKey` / `Authorization` / `authorization` JSON values, `sk-` / `sk_` / `org-` / `proj_` / `asst_` id prefixes; length-caps to 240 chars (down from the prior 600) with `…` suffix. Test coverage: `tests/Source_Core/AiErrorRedact.test.cpp` ships 5 `TEST_CASE`s / 20+ assertions across all redaction paths plus benign-input safety.

- 2026-05-17 · code-review · [security] · P1 — `coverage.sh:155` python `-c` interpolation injection
  Resolution: Both `python -c` invocations now pass values via `os.environ` (`XML_OUT` and `LINE_RATE`) instead of string-interpolation into the source. A path / rate containing `'` or `\` no longer breaks the script or can run attacker-controlled Python under `set -euo pipefail`.

- 2026-05-17 · code-review · [bug] · P1 — `coverage.sh:148` second OpenCppCoverage run overwrote `coverage.xml`
  Resolution: Each test exe now captures into its own binary intermediate (`coverage-tests.bin` + `coverage-lua.bin`); a third merge invocation reads both via `--input_coverage` and exports the final `coverage.xml` + optional HTML. SmatchetTests coverage is no longer silently dropped by the SmatchetLuaTests run.

- 2026-05-17 · test-rig → p4-blame · [security] · P1 — `CallstackParser::ParseCallstackText` regex super-linear in line length
  Resolution: Added a per-line length cap `kMaxLineLengthForRegex = 16384` in `Source_Core/src/CallstackParser.cpp`. Lines longer than the cap are skipped entirely without entering the three format regexes, so a malicious paste can no longer drive the parser into O(n^k) backtracking or stack-overflow the runner. New `[high-risk]` SUBCASE `64 KiB single line bypasses regex via length cap (DoS guard)` in `tests/Source_Core/CallstackParser.test.cpp` locks the contract (completes <50 ms; only the trailing real frame survives).

- 2026-05-17 · code-review · [retrospective] — Recent-PR audit findings (PRs #139–#148, #151)
  Resolution: Retrospective `code-review` sweep across 13 merged PRs landed 2026-05-16/17 — four parallel `code-review` agents, read-only. Umbrella entry split into 29 per-finding atoms on 2026-05-17 (2 P0 / 6 P1 / 14 P2 / 7 P3) across bug / security / test / infra / tooling / process. See each category file for the individual entries.

- 2026-05-16 · build-doctor · [tooling] — Phase 9 `tests-out-of-band` GitHub label must be created at the repo
  Resolution: user created the `tests-out-of-band` label at the repo level on 2026-05-16. Coverage-gate override mechanism now functional. Originally surfaced by Phase 9 of test-suite-expansion-completion.

- 2026-05-16 · test-author · [tooling] — Phase 9 PR template documenting `tests-out-of-band` override label
  Resolution: `.github/pull_request_template.md` shipped with Summary / Test plan / Coverage gate override / Plan revision sections.

- 2026-05-16 · build-doctor · [infra] — OpenCppCoverage local install on dev machines (no auto-install path)
  Resolution: (b) opt-in YELLOW check landed in both `scripts/dev/doctor.{sh,ps1}` gated by `SMATCHET_DOCTOR_CHECK_COVERAGE=1`; (c) `docs/harness/SETUP.md` now carries an `## Optional: coverage tooling` section documenting Chocolatey + releases-page install paths. (a) `bootstrap-msys2.ps1` integration deferred — Chocolatey isn't in the MSYS2 bootstrap surface and the opt-in env var keeps the friction low.

- 2026-05-16 · lua-binder · [infra] — Lua bindings round-trip test (Phase 6 `LuaBindings.test.cpp`) blocked by `AppController_LuaBindings.cpp` Class C structure; needs production refactor
  Resolution: TU split + `ILuaBindingHost` interface lift landed via PR #144 at sha 7e6762d (Phase-6-unblocker · lua-bindings-host-interface-lift); round-trip test (`LuaBindings.test.cpp` + `FakeLuaBindingHost`) shipped via PR #145 at sha d125b36 (Phase-6b · lua-bindings-roundtrip). New TU `Source_Core/src/AppController_LuaBindingsCore.cpp` is ImGui / GLFW / cpr / httplib / SQLite-free (verified via banned-deps grep + `nm -u` on the obj — zero matches). Eleven glue functions lifted out + receiver re-cast to `ILuaBindingHost*` via `state["__smatchet_app"]`. Behaviour-preserving: standalone + DX12 builds clean; ctest 2/2 PASS. Phase 6b round-trip test ships 13 cases on the `ILuaBindingHost` surface (3 high-risk) via `FakeLuaBindingHost.h`. Subsumed by AGENTS.md § Pure-helper TU-split recipe.

- 2026-05-16 · mcp-toolsmith · [infra] — MCP wire-protocol pure logic entombed in cpr/httplib-tainted lambda; needs TU split before Phase 5 tests
  Resolution: TU split landed via PR #141 at cfab599. 9 named helpers + 5 transitive support helpers + `TruncateOneLine` lifted to `Plugins/Mcp/McpJsonRpcPure.{h,cpp}` in namespace `smatchet::mcp::pure`. New TU is cpr/httplib/winsock-free (grep guard empty). 12 call sites in `McpPlugin.cpp` rewired via using-decls inside the anon namespace. Standalone + DX12 builds clean; ctest 284 cases / 1509 assertions PASS. Phase 5 re-dispatch unblocked. Subsumed by AGENTS.md § Pure-helper TU-split recipe.

- 2026-05-16 · orchestrator · [tooling] — Lint hook split into inline (clang-format only) + drain (cppcheck + clang-tidy + dual-target at Stop event)
  Resolution: branch `feat/lint-hook-deferred-drain`. Plan + impl at `docs/design/lint-hook-deferred-drain.md`. Verification: `scripts/dev/test-lint-hook-split.sh` 14/14 green. Stop-hook reprompt validated live (Part 0 spike). Discovered + fixed a pre-existing PATH bug in `lint-syntax-both.py` (cc1plus.exe silently exit-1'd when UCRT64 bin not on PATH). Shared `lint-cpp-common.sh` library factored. Five-agent version bump cluster (build-doctor, test-rig, debug-detective, perf-detective, code-review all v1 → v2).

- 2026-05-16 · build-doctor · [process] — MSYS2 UCRT64 toolchain bin not on hook-inherited PATH causes silent cc1plus / cppcheck / clang-tidy skip
  Resolution: `agents/build-doctor.md` § Common causes carries a new "cc1plus silent exit-1 with no diagnostics" bullet pointing at the toolchain-bin-on-PATH fix and replicating it in new sidecar wrappers. Fix landed in deferred-drain branch: `lint-cpp-common.sh` prepends `SMATCHET_TOOLCHAIN_BIN` (default `/c/msys64/ucrt64/bin`) to PATH; `lint-syntax-both.py` does the same via `subprocess.run(..., env=...)`.

- 2026-05-16 · offline-sync · [bug] — `TicketSyncService::ApplyIssueFetchPack` empty fetch in full-sync mode deletes entire cache
  Resolution: guard landed in `Source_Core/src/TicketSyncService.cpp:82-86`; test case 3 in `tests/Source_Core/TicketSyncService.test.cpp` flipped from documents-delete-all to enforces-reject-and-preserves. Surfaced by PR #130.

- 2026-05-16 · orchestrator · [process] — Trivial palette / theme tweaks must skip full build + test-all + bucket-E loop
  Resolution: AGENTS.md § Project rules § Trivial-visual-only change envelope. Strict envelope: write set ⊆ `{Source_Core/src/SmatchetTheme.cpp, Locales/*.json, ImGui style constants}`, literals-only diff, zero touch under `Source_Core/include/` / `Plugins/` / `cmake/` / `CMakePresets.json`. Under envelope: `ninja-iter-msvc SmatchetStandalone` build + `ninja-test-msvc` ctest (if relevant) suffice; bucket-E + isolated worktree both skipped.

- 2026-05-16 · security-review · [infra] — `BackendAuditTrail::AuditWriter` caches `GetAuditFilePath()` on first event; per-test path redirection breaks
  Resolution: `Source_Core/src/BackendAuditTrail.cpp` writer thread now resolves `GetAuditFilePath()` + `GetAuditFallbackPath()` inside the per-event `try` block (cheap string concat — no syscall) instead of caching on first event. ConfigManager dir changes at runtime route subsequent lines to the new path. New TEST_CASE `BackendAuditTrail: writer follows ConfigManager user-data dir change at runtime` proves the contract.

- 2026-05-16 · offline-sync · [infra] — Phase 3 `OfflineQueueServiceRuntime.test.cpp` + `TicketSyncService.test.cpp` deferred
  Resolution: interface extraction shipped via `feat/offline-queue-deps-interface` (PR D of `test-suite-expansion-completion`). `Source_Core/include/{IOfflineQueueDeps,ITicketSyncDeps,AppControllerDepsAdapter}.h` + `Source_Core/src/AppControllerDepsAdapter.cpp` land the production wiring; both services now hold an interface reference instead of `AppController&` and the two `friend class` decls collapsed into one `friend class AppControllerDepsAdapter;`. Test-side fixtures `tests/support/Fake{OfflineQueue,TicketSync}Deps.h` shipped for PR E + PR F.

- 2026-05-16 · offline-sync · [test] — Phase 3 `BackendAuditTrail` async-writer is a process-wide singleton
  Resolution: fix landed alongside the security-review · [infra] entry above. Production-side inline-path option chosen (option 1); per-event re-resolve in the writer thread's `try` block.

- 2026-05-16 · code-review + security-review · [test] — `CallstackParser.test.cpp` (Phase 2) non-blocking polish
  Resolution: shipped via `feat/test-callstack-adversarial`. The soft assertion at line 88 was pinned to `CHECK(f.Function.find("main") != std::string::npos)`. Four adversarial subcases added under TEST_CASE `CallstackParser::ParseCallstackText survives adversarial inputs` (`[high-risk]` suite). New backlog entry filed for the underlying super-linear-regex weakness (live in `security.md`).

- 2026-05-16 · orchestrator · [process] — test-rig agent packet should pre-authorize `<Unit>Parse.{h,cpp}` TU split for anon-namespace pure helpers
  Resolution: AGENTS.md § Orchestrator delegation packet § Test-rig `<Unit>Parse.{h,cpp}` TU-split pre-authorisation. Future `test-rig` phase packets must explicitly include the TU split in the allowed write set when the plan lists units with anonymous-namespace pure helpers. Subsumed by AGENTS.md § Pure-helper TU-split recipe.

- 2026-05-16 · test-rig · [infra] — Phase 2 `P4BlameParse.test.cpp` deferred; pure parsers live in anonymous namespace inside `P4Blame.cpp`
  Resolution: `feat/p4blame-parse-tu-split` shipped both the TU split and `tests/Source_Core/P4BlameParse.test.cpp` (12 cases / 75 assertions, all green). `Source_Core/{include,src}/P4BlameParse.{h,cpp}` extracted byte-identical helpers; `P4Blame.cpp` uses `using P4BlameParse::*` for unchanged call sites. Mutation sanity: inverting `m[1]` ↔ `m[2]` in the `reUserColonCode` branch swaps changelist + user — `ParseAnnotateTextLine` SUBCASEs all fail under the mutation, revert restores green. Subsumed by AGENTS.md § Pure-helper TU-split recipe.

- 2026-05-16 · test-rig · [infra] — `LocalCacheManager.h` mixes SQLite surface with pure `CachedTicket` POD
  Resolution: `Source_Core/include/CachedTicketTypes.h` now owns `CachedTicket`, `PendingCreate`, `PendingFieldEditRecord`, `DeadPendingFieldEdit`, `DeadPendingCreate` (5 PODs, pure stdlib, no SQLite include). `LocalCacheManager.h` re-includes the new header so the 20 existing callers see no API change. Dual-target Standalone + DX12 build green; ctest 1/1; `scripts/dev/test-all.sh` 100/100. Note: Phase 3 (PR #107) pulled `Source_Core/src/LocalCacheManager.cpp` into the test target for hostile-fixture cache tests, so the test exe legitimately needs `SQLiteCpp` at link now.

- 2026-05-16 · orchestrator · [process] — Plan-time `ls Source_Core/src/` cross-check missing
  Resolution: AGENTS.md § Orchestrator delegation packet § Plan-time production-file existence check. Five-second Glob / skeleton scan before finalising any test-coverage plan; catches plan / tree drift before delegation.

- 2026-05-16 · build-doctor · [tooling] — Windows-path-separator regex bug in build-log-grep scripts
  Resolution: `agents/build-doctor.md` § Common causes now carries a "Build-log grep regex on Windows must accept both path separators" bullet with the `[\\/]` recipe + a negative-test fixture requirement.

- 2026-05-16 · build-doctor · [tooling] — PS 5.1 `-Command "<multi-line>"` silently drops scope effects
  Resolution: `agents/build-doctor.md` § Common causes now carries a "PowerShell 5.1 silently drops scope effects from multi-line `-Command`" bullet recommending temp `.ps1` + `pwsh -File <temp.ps1>` for any PS-driven wrapper depending on scope changes.

- 2026-05-16 · build-doctor · [process] — Doctor strict `PATH contains C:\msys64\ucrt64\bin` produces false-fail on JetBrains-bundled-MinGW hosts
  Resolution: `scripts/dev/doctor.ps1` + `scripts/dev/doctor.sh` now split the check: gcc-on-PATH at version ≥ 13 is the "toolchain reachable" REQUIRED gate (origin-agnostic; JetBrains-bundled MinGW passes); `C:\msys64\ucrt64\bin` literal on PATH is WARN-only. `scripts/dev/test-doctor.sh` assertion 2 updated.

- 2026-05-15 · git-janitor · [tooling] — `git pull --rebase --empty=drop` unsupported on shipped git version
  Resolution: `agents/git-janitor.md` § Bringing `develop` to latest now uses `git pull --ff-only`. The agent's contract bans direct pushes to `develop`, so local develop is always upstream-tracking post-merge — FF is the correct op and the rebase path was dead code.

- 2026-05-15 · git-janitor · [process] — PR-only-to-`develop` rule has no FF-clean docs-only escape valve
  Resolution: agents/git-janitor.md § FF-clean docs-batch exception; version bumped 1 → 2; banner + Hard refusals updated with pointer to the exception.

- 2026-05-15 · test-rig · [process] — `JiraClient.h` cascade blocks per-cpp testing of `TrackerFieldValueParser`
  Resolution: (03576ff) — `Source_Core/include/TrackerFieldValueParser.h` now `#include "TrackerFieldSchema.h"` instead of `JiraClient.h`; `FormatWorkDurationFromSeconds` declaration moved from `JiraClient.h:18` to the value-parser header. `tests/Source_Core/TrackerFieldValueParser.test.cpp` ships 10 cases / 38 assertions. Dual-target build green; ctest 1/1.

- 2026-05-15 · test-rig · [process] — `OfflineCreateQueue::kMaxReplayAttempts` lives behind `<SQLiteCpp/SQLiteCpp.h>`
  Resolution: (86895de) — option (b) landed. New `Source_Core/include/OfflineQueueReplayPolicy.h` declares `kMaxReplayAttempts = 5` + inline `ShouldArchive(int currentAttempts, int maxAttempts = kMaxReplayAttempts)`; zero banned includes. Four decision sites updated in `OfflineQueueService.cpp`. `tests/Source_Core/OfflineQueueReplayPolicy.test.cpp` ships 5 cases / 26 assertions. Dual-target build green; SmatchetTests aggregate now 35 cases / 133 assertions.

- 2026-05-14 · lua-binder · [process] — sol2 v2.20.6 API limitations not in plan
  Resolution: agents/lua-binder.md § Hard invariants. "sol2 v2.20.6 — recorder/usertype methods take plain args (no `sol::this_state` first param); `new_usertype` takes only `name` + method-name/ptr pairs, no constructor sentinel."

- 2026-05-14 · lua-binder · [process] — Lua-as-C++ needs more than `LANGUAGE CXX`
  Resolution: agents/lua-binder.md § Hard invariants. "Compiling Lua 5.3 as C++ requires patching `luaconf.h` (or wrapping host-side `#include <lua.hpp>` blocks) with `extern \"C\"` — `LANGUAGE CXX` alone is insufficient."

- 2026-05-14 · architect · [tooling] — `mcp__vexp__get_skeleton` empty result on indexed files
  Resolution: agents/architect.md § Pre-flight (fall back to Read on empty skeleton, optional index_status at start of long runs).

- 2026-05-13 · p4-blame · [process] — multi-file split handoff packet missed transitive call closure
  Resolution: AGENTS.md § Orchestrator delegation packet § File-split closure rule.

- 2026-05-13 · p4-blame · [process] — missing-include after split is silent until build
  Resolution: AGENTS.md § Orchestrator delegation packet § Post-split include-replication rule.

- 2026-05-13 · orchestrator · [process] — branch-switch wipes untracked plan files
  Resolution: 40c0bb2 — AGENTS.md § Project rules § Plan-doc safety.

- 2026-05-13 · orchestrator · [process] — wrong-exe testing burns iterations
  Resolution: 16eb7af — AGENTS.md § Debug techniques § Exe staleness check + agents/{perf-detective,spike-hunter,build-doctor}.md hard rules.

- 2026-05-13 · orchestrator · [process] — schema-version churn
  Resolution: 40c0bb2 — AGENTS.md § Project rules § Schema-version bumps.

- 2026-05-13 · orchestrator · [process] — pink-diagnostic clear color for UI gap detection
  Resolution: 40c0bb2 — AGENTS.md § Debug techniques § Pink-clear UI gap detection.

- 2026-05-13 · spike-hunter / orchestrator · [process] — ImGui docking state cannot be re-parented at runtime
  Resolution: 45c14c9 — agents/grid-engine.md + agents/unreal-bridge.md § Hard invariants.

- 2026-05-13 · code-review · [tooling] — lint hook does not run clang-format on newly created `.h` files
  Resolution: root cause was script-side, not harness-side. Sentinel-log reproducer (2026-05-15 session) confirmed `PostToolUse` fires on every `Edit`/`Write`. Real bug: under MSYS2 the hook received `CLAUDE_PROJECT_DIR=/c/Dev/Smatchet` (POSIX form) while `tool_input.file_path` arrived as `C:\Dev\Smatchet\...` (Windows form); after backslash normalisation, the case glob never matched. Fix: normalise both via `cygpath -m` before the prefix strip.

- 2026-05-13 · architect · [process] — skip architect when prompt already specifies file paths + symbols + commit messages
  Resolution: 40c0bb2 — AGENTS.md § Heuristic, new bullet.

- 2026-05-12 · orchestrator · [process] — pre-resolve hard-invariant collisions before delegating implementation slices
  Resolution: d4714ad — AGENTS.md "Orchestrator delegation packet" § Invariant decisions; tracker-backend.md ITrackerClient widening rule hardened.

- 2026-05-12 · orchestrator · [process] — build one shared literal inventory and pass it to every delegated agent
  Resolution: d4714ad — AGENTS.md "Orchestrator delegation packet" § Shared inventory.

- 2026-05-12 · orchestrator · [process] — inline relevant design-doc sections in agent prompts and forbid rereads unless blocked
  Resolution: d4714ad — AGENTS.md "Orchestrator delegation packet" § Inline task context.

- 2026-05-12 · orchestrator · [process] — cap routine implementation reports to short table form
  Resolution: d4714ad — AGENTS.md "Orchestrator delegation packet" § Output budget.

- 2026-05-12 · orchestrator · [process] — remind subagents that code comments must not reference the task or PR plan
  Resolution: d4714ad — AGENTS.md "Orchestrator delegation packet" § Comment discipline.

- 2026-05-12 · orchestrator · [tooling] — allow text-search first for exhaustive literal / symbol inventories
  Resolution: d4714ad + follow-up — AGENTS.md "Semantic-search exceptions" section, placed outside the auto-managed vexp block so it survives vexp tool updates.

- 2026-05-12 · orchestrator · [tooling] — dedupe or cap repeated lint-hook diagnostics from PostToolUse
  Resolution: d4714ad — `.claude/hooks/lint-cpp.sh` adds `format_issues` (awk dedupe + cap); `SMATCHET_LINT_MAX_LINES` env var, default 120, documented in `agents/build-doctor.md`.

- 2026-05-12 · command-system · [process] — when the harness lint hook auto-runs on every edit, don't also run a batch `clang-format` at the end
  Resolution: 45c14c9 — agents/command-system.md § Hard invariants, new bullet.

- 2026-05-12 · grid-engine, command-system · [process] — localization accessor is `SmatchetLocalization::T(key, englishFallback)`
  Resolution: d4714ad — agents/grid-engine.md L55 + agents/command-system.md L54 both carry the `SmatchetLocalization::T(key, englishFallback)` invariant.

- 2026-05-12 · grid-engine · [process] — design-doc PRs that span ≥3 subsystems have no clear owner
  Resolution: d4714ad — AGENTS.md § Orchestrator delegation packet § Subsystem split bullet covers pre-split rule; `pr-driver` meta-agent not pursued — orchestrator-side discipline is sufficient.

- 2026-05-12 · tracker-backend · [infra] — no test rig in the repo
  Resolution: 5-commit migration per `docs/design/applied/test-rig-agent.md` landed on `develop`: 97ab7f1, 3b47ff0, 7f024fc, 1f2ad93, plus plan revision. Final state: 20 test cases / 69 assertions; `ctest --output-on-failure` 1/1 green on `ninja-test-msvc`. Two follow-on items surfaced (split `JiraClient.h`; lift offline-queue replay-cap decision) — also archived above.

- 2026-05-12 · tracker-backend · [process] — design-doc PR sections that list line numbers should mark each as `(cfg-read)` / `(draft-write)` / `(audit-only)`
  Resolution: d4714ad — AGENTS.md § Orchestrator delegation packet § Shared inventory mandates `<file>:<line>:<role>` with the exact role suffixes.

- 2026-05-12 · command-system · [process] — when a PR plan names a specific line/symbol, do a 30-second sanity grep before editing
  Resolution: 45c14c9 — agents/command-system.md § Workflow step 3 + agents/tracker-backend.md § Workflow step 1.

- 2026-05-13 · orchestrator · [process] — ASCII em-dash banner bars (`━━━`) at agent open / close burn input + output tokens per call with no routing value
  Resolution: 94d5836 — one-line banner spec replaced multi-line ceremony.

- 2026-05-13 · orchestrator · [process] — generic "Semantic search first" preamble duplicated across 11 agents
  Resolution: 94d5836 — dropped from agents with no agent-specific guidance; kept on agents that add real twists. AGENTS.md § Semantic codebase search owns the canonical rule.

- 2026-05-13 · debug-detective · [tooling] — NDJSON helper C++ template embedded inline in agent prompt (~85 lines)
  Resolution: d79a8fc — externalized to `agents/_shared/templates/SmatchetAgentDebug.h.tmpl`. debug-detective.md shrunk 633 → 547 lines.

- 2026-05-13 · orchestrator · [process] — `delegates-to:` frontmatter present on 8 of 18 agents with no documented rule
  Resolution: d79a8fc — AGENTS.md § `delegates-to:` frontmatter. Absence ≠ "never delegates" but "via orchestrator, not direct."

- 2026-05-13 · orchestrator · [tooling] — `harness-hints.claude-code.tools:` line duplicates `capabilities:` list and goes unread by Claude Code
  Resolution: d6ba897 — dropped the line everywhere; kept `model:` + `effort:` (real routing knobs).

- 2026-05-13 · test-author · [process] — verification-automation cadence not project-wide; manual residue could ship indefinitely
  Resolution: a18f985 — AGENTS.md § Verification automation makes the cadence project-wide: plan-time + first-round + every-agent-handoff. architect.md mandates bucket-A/B/C/D/E classification; code-review.md flags manual residue as Critical.

- 2026-05-13 · test-author · [tooling] — no unified test runner; each scripts/dev/test-*.sh ran in isolation
  Resolution: a18f985 — `scripts/dev/test-all.sh` globs `scripts/dev/test-*.sh` (excluding self), runs each, aggregates Passed/Failed.

- 2026-05-13 · test-author · [infra] — bucket E (ImGui Test Engine) not wired
  Resolution: 2026-05-15 · execution plan at `docs/design/imgui-test-engine-bucket-e-execution.md`. FetchContent + `cmake/ImGuiTestEngine.cmake` + `Source_Core/include/SmatchetImConfig.h` + `UiTestScenario` + `ui_test.run` CLI + `tests/ui/` enrolment + `scripts/dev/test-ui-views-columns-reorder.sh` driver all shipped against the Views → Columns reorder bug.

- 2026-05-15 · orchestrator · [process] — sequential subagent dispatch loses wall-clock when delegations are contract-independent
  Resolution: d206de5 — AGENTS.md § Parallel dispatch.

- 2026-05-15 · orchestrator · [tooling] — no session scratchpad
  Resolution: 6df6170 + d206de5 — `.session-context.md` at repo root (gitignored); SessionStart hook truncates; SubagentStop hook (agent-token-log.py) appends a header block when subagent's report carries `## Session context append`.

- 2026-05-15 · orchestrator · [tooling] — JSONL telemetry only tracked tokens
  Resolution: 6df6170 — agent-token-log.py now emits outcome, halt_reason, agent_version, delegation_chain, tools_used, tool_trace per row.

- 2026-05-15 · orchestrator · [process] — get_skeleton under-used
  Resolution: d206de5 — AGENTS.md § Skeleton-first hard rule.

- 2026-05-15 · orchestrator · [process] — agent prompts had no version field
  Resolution: d206de5 — every agent now carries `version: <N>` in frontmatter; mirror banner reads `@v<N>`; telemetry surfaces drift.

- 2026-05-15 · orchestrator · [process] — output-shape drift across agents
  Resolution: d206de5 — AGENTS.md § Agent output contract codifies four classes (Investigator / Implementer / Helper / Maintenance) + mandatory `## Outcome: <state>`.

- 2026-05-15 · orchestrator · [tooling] — trigger keywords lived in per-agent frontmatter but no central routing table
  Resolution: AGENTS.md § Trigger auto-activation publishes the keyword → agent map.
