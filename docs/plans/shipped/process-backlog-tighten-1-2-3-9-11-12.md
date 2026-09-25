# Tighten 6 process-backlog rules (items 1, 2, 3, 9, 11, 12)
<!-- plan-date: 2026-05-19 -->

Six process-category entries in `docs/backlog/agent-self-improvement/process.md` each have a concrete documented next action that lands in a single agent doc (AGENTS.md or `agents/<role>.md`) or a small script. None require code in `Source_Core/`, builds, or tests. All six can ship as **doc + tiny-script edits**, batched on a single branch with one commit per slice.

| # | Date · author · prio | One-line summary | Slice |
|---|---|---|---|
| 1 | 2026-05-19 · orchestrator · P1 | Ship-loop fires commit+push during iterative UI polish before user validates | Slice 1 |
| 2 | 2026-05-19 · git-janitor · P2 | FF-clean docs-batch exception still gates on `test-all.sh` for pure-docs diffs | Slice 2 |
| 3 | 2026-05-18 · orchestrator · P2 | Pushing commits to a merged-PR branch silently orphans them | Slice 3 |
| 9 | 2026-05-16 · orchestrator · P3 | Plan-doc file-level tables drift from grep ground truth; re-verify before sealing | Slice 4 |
| 11 | 2026-05-16 · orchestrator · P3 | API-500 mid-run recovery procedure not documented | Slice 5 |
| 12 | 2026-05-16 · orchestrator · P3 · OBSERVATIONAL | `_plan-locks.md` stale-read race when concurrent orchestrators / hooks edit | Slice 6 |

Total estimated cost: ~3 h (Slice 3 is the only ~1 h piece — a small pre-push hook; everything else is doc edits ≤ 30 min each).

Ship order: **3 → 1 → 2 → 11 → 9 → 12**. Rationale: Slice 3 is the only piece with executable code (smallest blast radius validated first); Slices 1 + 2 are the highest-priority doc rules (P1 + P2); Slices 9 + 11 + 12 are P3 doc tightening with no inter-dependency.

---

## Slice 1 — Visual-validation exception to the ship-loop (item 1)

**Backlog entry**: `docs/backlog/agent-self-improvement/process.md` § 2026-05-19 orchestrator P1.

**Current state**: AGENTS.md § Autonomous ship-loop default mandates `diagnose → fix → build → commit → push → open PR` in one turn. Correct for self-verifiable changes (sanitiser, doctest, scenario coverage). Wrong for UI / visual / theme polish where the user is the only acceptance check. PR #289's chat-render arc shipped 4 broken intermediate commits before the working one because the loop didn't pause for visual verdict.

**Target state**: explicit **Visual-validation exception** in AGENTS.md § Autonomous ship-loop default. Loop pauses after the build step, presents the launched-exe handle, waits for user verdict, then resumes or stashes + iterates.

**Steps**:

1. Edit `AGENTS.md` § Autonomous ship-loop default § Exceptions. Add a 5th exception **after** the existing 4:
   ```markdown
   5. **Visual-validation exception** — fires when **both** conditions hold:
      1. Diff touches at least one of: `Source_Core/src/SmatchetTheme.cpp`, `Source_Core/src/Smatchet*Ui*.cpp`, `Source_Core/include/SmatchetTheme.h`, `Locales/*.json`, ImGui style constants (`ImVec4` / `ImGuiStyle` literals), dock-layout init paths.
      2. AND no bucket-C screenshot diff or bucket-E ImGui-Test-Engine scenario covers the changed widget.

      When both fire, the loop pauses after **build** with the launched exe. Orchestrator presents:
      - the `build/<preset>/Smatchet.exe` path + a one-line run command,
      - the `bash` background-task id of the launched exe (or "launched manually"),
      - the specific visual change the user is asked to evaluate (one sentence).

      Wait for the user's verdict before commit+push. On "looks good" → resume the loop and commit. On "no" → leave the working tree dirty; iterate in-place. The orchestrator does `git diff` between attempts to see what was tried. Clean-slate reset (`git checkout -- <files>`) only when the user explicitly asks for one. Never commit+push an unvalidated visual change.

      Out-of-scope (NOT a visual-validation pause):
      - A change with no test coverage but no visual-path touch — that's a Pillar-3 "needs test coverage" problem, route via the test backlog.
      - A change that touches the visual paths AND has bucket-C/E coverage — coverage is the gate; ship-loop continues. If the user disagrees with the golden after merge, the bucket-C golden is re-bootstrapped per AGENTS.md.
   ```

2. Add a pillar anchor — AGENTS.md § UX Pillars § 4. Accessibility gets a new sub-bullet under **Locked in-scope**:
   ```markdown
   - **Visual-validation acceptance**: when no automated check covers a visual change (palette, layout, font), the user is the verifier. See § Autonomous ship-loop default § Exceptions § Visual-validation exception for the loop-pause contract.
   ```

3. Cross-link from `agents/git-janitor.md` § Hard refusals (or § Standard cleanup loop, whichever currently discusses commit cadence) — one-line pointer to the new exception so the janitor doesn't squash-merge a draft PR whose intermediate commits are still being visually validated.

**Verification**:
- Read the diff to AGENTS.md; the 5th exception bullet is present and follows the same numbered-list shape as exceptions 1–4.
- `grep -n "Visual-validation" AGENTS.md` returns at least 2 hits (the exception entry + the pillar cross-link).
- Manual scenario walk: write a one-line "imagine the user asked you to retune the AI assistant tab background colour" and confirm the new exception flips the orchestrator's behaviour from "ship loop" to "build + pause".

**Risk**: low — doc-only. The risk is the rule being too loose ("everything is visual polish, never ship") or too tight ("only theme.cpp counts"). Mitigation: name a concrete enumeration of triggers (palette, layout, font, theme) and require the orchestrator to explicitly invoke the exception (default = ship loop).

---

## Slice 2 — Pure-docs sub-exception to the FF-clean docs-batch (item 2)

**Backlog entry**: `docs/backlog/agent-self-improvement/process.md` § 2026-05-19 git-janitor P2.

**Current state**: `agents/git-janitor.md` § FF-clean docs-batch exception precondition 4 requires `bash scripts/dev/test-all.sh` exit 0 before the FF push. For pure-docs diffs (single file under `docs/plans/active/**`, no executable code) the gate adds ~3 min wall-clock with nothing to validate.

**Target state**: a **Pure-docs sub-exception** that skips `test-all.sh` when the ahead-range diff is strictly within doc paths.

**Steps**:

1. Locate the FF-clean docs-batch exception in `agents/git-janitor.md` (the backlog entry names lines 56–97; verify with `git grep -n "FF-clean docs-batch"`).

2. Append a new sub-section after precondition 4:
   ```markdown
   #### Pure-docs sub-exception

   When the ahead-range diff touches **only** the following paths, skip the `test-all.sh` gate entirely:

   - `docs/**`
   - `backlog/**`
   - `AGENTS.md`
   - any root-level `*.md`

   The gate **must** still fire if the diff touches any of:

   - `agents/**` (changes agent behaviour)
   - `scripts/**` (changes tooling / hooks)
   - `tests/**` (changes test surface)
   - `.gitignore`, `.github/**`, `CMakePresets.json`, `CMakeLists.txt` (CI / build)
   - any C++ / Lua / Python / shell source

   Discriminator: `git diff --name-only origin/develop...HEAD | grep -vE '^(docs/|backlog/|AGENTS\.md$|[A-Z_]+\.md$)' | head -1` returning non-empty = NOT pure-docs.

   Cross-link: AGENTS.md § Trivial-visual-only change envelope is the precedent for path-prefix-based gate relaxation; this sub-exception is its pure-docs sibling.
   ```

3. If the rule was duplicated into `AGENTS.md` (likely under § Project rules or § Delegation), mirror the sub-exception there. Verify with `grep -n "FF-clean docs-batch" AGENTS.md`.

4. Add a one-line bash helper `scripts/dev/is-pure-docs-diff.sh` so the discriminator is testable + reusable:
   ```bash
   #!/bin/bash
   # is-pure-docs-diff: exit 0 if HEAD..origin/<base> diff is strictly within
   # doc paths (docs/, backlog/, AGENTS.md, root *.md). Else exit 1.
   # Usage: is-pure-docs-diff.sh <base-branch>   (default: develop)
   set -euo pipefail
   base="${1:-develop}"
   if git diff --name-only "origin/$base...HEAD" | grep -qvE '^(docs/|backlog/|AGENTS\.md$|[A-Z_]+\.md$)'; then
       exit 1
   fi
   exit 0
   ```

**Verification**:
- Manual scenario: with the current branch on a pure-docs commit (e.g. the plan-revision commits in `develop`'s recent history), `bash scripts/dev/is-pure-docs-diff.sh develop` exits 0.
- With a branch that touched `scripts/dev/foo.sh`, the script exits 1.
- Read git-janitor.md diff: the sub-exception text is under the FF-clean docs-batch heading and references the helper script.

**Risk**: low. The risk is misclassifying a "doc + tiny-script" diff as pure-docs and skipping a gate that should have run. Mitigation: the explicit deny-list (`scripts/`, `tests/`, etc.) is exhaustive vs the allow-list. Helper script is the single source of truth.

---

## Slice 3 — Refuse push to merged-PR branch (item 3)

**Backlog entry**: `docs/backlog/agent-self-improvement/process.md` § 2026-05-18 orchestrator P2.

**Current state**: pushing commits to a branch whose PR is already `MERGED` silently succeeds on the remote but never reaches CI / a re-opened PR. PR #249 wave cost one rescue PR (#258) + 5 cherry-picks + manual conflict resolution.

**Target state**: a `pre-push` git hook (or wrapped `git push` invocation) that queries `gh pr view --json state` for the current branch and refuses + warns when the PR is `MERGED` / `CLOSED`. Hook output names the exact recovery command.

**Steps**:

1. Write `scripts/dev/git-pre-push-merged-pr-guard.sh`:
   ```bash
   #!/bin/bash
   # git-pre-push-merged-pr-guard: refuse a push when the current branch's PR is
   # MERGED or CLOSED on GitHub. Commits to a merged/closed PR branch are
   # silently accepted by the remote but never reach CI nor reopen the PR.
   #
   # Wired as a pre-push hook (.git/hooks/pre-push or via core.hooksPath).
   # Idempotent: exit 0 when no PR exists, branch is on develop/main, or PR is OPEN.
   set -euo pipefail

   branch=$(git rev-parse --abbrev-ref HEAD)

   # Skip when on develop / main / detached HEAD.
   case "$branch" in
       develop|main|HEAD) exit 0 ;;
   esac

   # No PR for this branch -> allow.
   pr_state=$(gh pr view "$branch" --json state --jq .state 2>/dev/null || true)
   if [ -z "$pr_state" ] || [ "$pr_state" = "OPEN" ]; then
       exit 0
   fi

   pr_number=$(gh pr view "$branch" --json number --jq .number 2>/dev/null || echo "?")
   cat >&2 <<EOF
   git-pre-push-merged-pr-guard: REFUSING push.

     branch:    $branch
     PR #$pr_number state: $pr_state

   Pushing to a $pr_state PR branch silently lands commits the PR will never
   pick up. Open a follow-up branch from develop instead:

     git checkout -b <new-branch> origin/develop
     git cherry-pick <sha1> <sha2> ...      # or re-apply changes
     git push -u origin <new-branch>
     gh pr create --draft

   To override (rare — usually wrong): set SMATCHET_ALLOW_MERGED_PR_PUSH=1.
   EOF

   if [ "${SMATCHET_ALLOW_MERGED_PR_PUSH:-0}" = "1" ]; then
       echo "git-pre-push-merged-pr-guard: SMATCHET_ALLOW_MERGED_PR_PUSH=1 — proceeding." >&2
       exit 0
   fi
   exit 1
   ```

2. Wiring — tracked + setup-harness path (option B from grill-with-docs interview Q3=A):
   - Single executable file at `scripts/git-hooks/pre-push` (the guard logic is inline; no `scripts/dev/` sidecar — YAGNI per grill Q2=A).
   - Extend `scripts/setup-harness.sh` so the Claude Code setup branch runs `git config --local core.hooksPath scripts/git-hooks` **only if** the current `core.hooksPath` is unset or already equal to that path. If a different custom path is set, log a warning + skip (don't trample user's hook setup).
   - The single-file hook script is the deliverable; no dotted-file sidecars.

3. Add an opt-out env var (`SMATCHET_ALLOW_MERGED_PR_PUSH=1`) for the rare legitimate case (e.g. retroactive doc edit on a merge-commit branch that won't ship).

4. Write `tests/dev/test-merged-pr-guard.sh` (bash; auto-enrolled by `test-all.sh`) covering: no-PR branch passes, OPEN PR passes, develop/main pass, `SMATCHET_ALLOW_MERGED_PR_PUSH=1` override passes, MERGED PR fails with the recovery banner. Mock `gh` via a `PATH`-prepended stub.

**Verification**:
- Synthetic mock-gh test in `tests/dev/test-merged-pr-guard.sh` exercises all five branches; all pass.
- Manual: on a known-MERGED branch (e.g. revisit `chore/unblock-external-2-3-4` after re-creating locally), `git push` fails with the recovery banner.
- On `develop`, `git push` passes through (no banner).

**Risk**: medium. A pre-push hook in the path is friction for every push. Mitigation: (a) script exits 0 fast when no `gh` PR exists (cheap query, ~100ms typical), (b) opt-out env var, (c) the hook only blocks the documented bug case.

**Confirm-before-act**: wiring `core.hooksPath` is a per-clone `git config` mutation; the harness's git-config rule requires explicit user authorisation. The orchestrator asks before running `git config --local core.hooksPath`.

---

## Slice 4 — Plan-doc file-level table re-verify (item 9)

**Backlog entry**: `docs/backlog/agent-self-improvement/process.md` § 2026-05-16 orchestrator P3.

**Current state**: plan-doc § File-level changes tables ship with `git grep`-falsifiable claims. PR `ai-assistant-side-panel` Phase A listed `FieldCatalogCache.cpp` as a `NetworkUsageTracker::Instance().Record(...)` caller — grep returns zero hits. Also listed a `CORE_SOURCES` list-edit that doesn't exist (it's a `file(GLOB_RECURSE …)`). Each error costs ~10 min of downstream-agent confusion.

**Target state**: at packet-seal time, the orchestrator runs a 2-command probe — `git grep <symbol-list>` + `grep -n <cmake-variable> CMakeLists.txt` — for every symbol / CMake variable named in the file-level table. **Rule lives in `docs/agent-rules/delegation.md` § Orchestrator delegation packet** (codebase probe during grill-with-docs Q3 found that the body of that section lives in delegation.md, not AGENTS.md — AGENTS.md carries only a one-line index pointer at line 264). Helper script `scripts/dev/plan-doc-table-probe.sh` ships in v1.

**Steps**:

1. Edit `docs/agent-rules/delegation.md` § Orchestrator delegation packet. Add a new sub-section **§ File-level table re-verify (before sealing)** alongside the existing § Plan-lock pre-flight / § Shared inventory bullets:
   ```markdown
   #### File-level table re-verify (before sealing)

   Every § File-level changes table in a plan-doc names symbols (function names, `NetworkUsageTracker::Instance().Record`) and/or CMake variables (`CORE_SOURCES`, `SMATCHET_TESTS_SOURCES`). Before sealing the table, run a 2-command probe and reconcile any miss:

   1. For every symbol named as a caller / definer:
      ```
      git grep -n "<symbol>"
      ```
      A zero-hit means the symbol does not exist (renamed, removed, never landed) — fix the table or scope.

   2. For every CMake variable named as the target of a list-edit:
      ```
      grep -nE "(set|file\s*\(\s*GLOB)" CMakeLists.txt | grep -n "<variable>"
      ```
      If the variable is populated via `file(GLOB_RECURSE …)`, no list-edit is needed — drop the row.

   Mechanical shortcut: `bash scripts/dev/plan-doc-table-probe.sh <plan-doc-path>` parses the plan's § File-level changes table and runs both probes for every named symbol / CMake variable, reporting hits / misses.

   Cost: ≤ 5 min per packet (manual) or seconds (script). Skips the most common class of plan-table drift (symbols that don't exist + redundant CMake edits).
   ```

2. Cross-link from `architect`, `tracker-backend`, `grid-engine`, and `command-system` agent docs (densest plan-doc-table authors) — one-line pointer to the new sub-section.

3. **Ship `scripts/dev/plan-doc-table-probe.sh` in v1** (per grill Q3=B). Parser shape:
   - Argument: path to a `docs/plans/active/*.md` plan doc.
   - Locate the first `## File-level changes` (or `### File-level changes`) heading. Read until next heading at same-or-lesser level.
   - Within that range, parse markdown tables (lines starting with `|`); extract cells under columns matching `Symbol` / `Function` / `Caller` / `Definer` / `CMake variable` / `CMake target` (case-insensitive header match).
   - For each extracted symbol: run `git grep -nF -- "<symbol>"` (literal, not regex; symbols often contain `::`, `(`, `)`). Hit count + first file line printed.
   - For each extracted CMake variable: run `grep -nE "(set|file\s*\(\s*GLOB).*<var>" CMakeLists.txt tests/CMakeLists.txt`. Hit count + line printed.
   - Output format: TSV `<plan-doc>:<table-row>\t<symbol-or-var>\t<hit-count>\t<first-hit>` or `<plan-doc>:<table-row>\t<symbol-or-var>\tMISS`.
   - Exit code: 0 if every row resolves; 1 if any MISS.

4. Bucket-A self-test `scripts/dev/test-plan-doc-table-probe.sh` (auto-enrolled by `scripts/dev/test-all.sh`): synthetic fixture plan with one valid symbol + one invalid symbol + one GLOB-populated CMake variable. Assert exit 1 + miss line reports the invalid symbol.

**Verification**:
- Read delegation.md diff; the sub-section is present under § Orchestrator delegation packet.
- `bash scripts/dev/plan-doc-table-probe.sh docs/plans/shipped/ai-assistant-side-panel.md` reports a MISS on `FieldCatalogCache.cpp` (re-validating the original backlog entry).
- `bash scripts/dev/test-plan-doc-table-probe.sh` exits 0 (self-test passes).
- `bash scripts/dev/test-all.sh` picks up the new self-test.

**Risk**: medium. Parser fragility — plan-doc table column shapes vary. Mitigation: case-insensitive header match for known column names; if no recognised column present, script exits 0 with a `no-tables-found` log line (not a failure — some plans have no file-level table).

---

## Slice 5 — API-500 mid-run recovery procedure (item 11)

**Backlog entry**: `docs/backlog/agent-self-improvement/process.md` § 2026-05-16 orchestrator P3.

**Current state**: Wave A2 had 4/4 agents error API-500 on their final-synthesis turn after shipping 100% of file edits. Recovery is straightforward but undocumented — each instance required the orchestrator to ad-hoc inspect the worktree + commit + push + open PR. Tracker-payload required force-push amend because `git commit` only included staged files.

**Target state**: a new AGENTS.md § Delegation § API-500 recovery sub-section names the 5-step recovery procedure, with the `git add -A` gotcha called out explicitly.

**Steps**:

1. Edit `AGENTS.md` § Delegation. Add a new sub-section (place it near § Debug-mode pause-loop since both are exception-flow rules):
   ```markdown
   #### API-500 mid-run recovery

   When a delegated agent errors API-500 mid-run, the worktree state is usually complete — the agent shipped its file edits before the synthesis turn failed. Recovery procedure (orchestrator runs from the agent's worktree):

   1. **Inspect worktree state**:
      ```
      git -C <worktree-path> status --short
      git -C <worktree-path> diff --stat
      ```
      Confirm the expected files are modified / created.

   2. **Run local gates** (the agent didn't get to):
      - `cmake --build --preset ninja-iter-msvc` (and `--target SmatchetStandalone SmatchetCore_DX12` for dual-target).
      - `bash scripts/dev/test-all.sh` if the diff touches anything under § FF-clean docs-batch precondition 4's gate set.

   3. **Stage everything** — this is the gotcha. The agent may have created new files that aren't staged. Use `git add -A`, not `git add <list>`:
      ```
      git -C <worktree-path> add -A
      git -C <worktree-path> commit -m "<recovery message naming the agent + Wave>"
      ```

   4. **Push + open draft PR**:
      ```
      git -C <worktree-path> push -u origin <branch>
      gh pr create --draft --title "..." --body "..."
      ```

   5. **Mark recovery in backlog** — add an entry to `docs/backlog/agent-self-improvement/process.md` with author = the failed agent, P3, summarising the recovery (which agent, what wave / packet, files-staged-via-`add -A`-vs-`<list>`, force-push-or-not). Reuses the existing self-improvement loop so accumulating evidence surfaces a "harness-level retry-on-API-500" fix when the rate justifies it.

   If step 3's commit missed new files (symptom: PR diff is smaller than expected), `git add -A && git commit --amend --no-edit && git push --force-with-lease origin <branch>` recovers — `--force-with-lease` is safe here because the branch is the agent's own `agent/<id>` or `claude/<id>` worktree (carve-out per § Git safety protocol — see also `docs/adr/0005-force-push-carve-out-for-spawned-agent-recovery.md`).
   ```

2. Cross-link from `agents/handoff-implementer.md` § Stop conditions (line 84) — one-line pointer.

3. The "force-with-lease on spawned-agent branches is safe" claim is a partial relaxation of the harness's global force-push ban. Add a one-line carve-out in AGENTS.md § Project rules naming this case (carve-out scope per grill Q4=A — both spawned-agent branch prefixes):
   ```markdown
   - **Force-push carve-out**: `git push --force-with-lease origin agent/<id>` or `git push --force-with-lease origin claude/<id>` is permitted during API-500 recovery only, when the orchestrator is amending an unpushed-since-API-500 commit on a spawned-agent worktree branch. See § Delegation § API-500 recovery + `docs/adr/0005-force-push-carve-out-for-spawned-agent-recovery.md`. Excludes `chore/*`, `feat/*`, `fix/*`, `docs/*`, `wip/*` and any branch with non-self commits in the ahead-range.
   ```

4. Write `docs/adr/0005-force-push-carve-out-for-spawned-agent-recovery.md` (per grill Q9=A). Follows the existing `docs/adr/0001..0004` format. Sections: Title · Status · Context · Decision · Consequences · Alternatives considered (always-new-commit, full force-push ban with manual-PR rescue). ~30 lines.

**Verification**:
- Read AGENTS.md diff; both new sub-sections (§ Delegation § API-500 recovery + § Git safety protocol force-push carve-out) are present.
- Manual scenario walk: read the Wave A2 recovery commit history (e.g. `git log --all --grep "Wave A2"`) and confirm the new procedure matches what was actually done.

**Risk**: low. Risk is over-permissive force-push carve-out leaking into non-API-500 use. Mitigation: the carve-out names the specific branch pattern (`agent/<id>`) + the specific recovery context.

---

## Slice 6 — `_plan-locks.md` stale-read race (item 12, OBSERVATIONAL)

**Backlog entry**: `docs/backlog/agent-self-improvement/process.md` § 2026-05-16 orchestrator P3 OBSERVATIONAL.

**Current state**: `Edit` errors with `File has been modified since read, either by the user or by a linter` when concurrent orchestrators (multi-worktree) or PostToolUse hooks (`lint-cpp.sh` reformats) edit `_plan-locks.md` between an orchestrator's `Read` and `Edit`. Re-Read + re-Edit always recovers; backlog entry says no obvious fix.

**Target state**: even if no single mechanism fixes the race, the **canonical recovery pattern** is documented so every orchestrator + agent that hits the error already knows to apply it without rediscovering it. Status flips from `observational` → `applied`.

**Steps**:

1. Audit current AGENTS.md / agent-docs for any existing prose on the stale-read recovery:
   ```
   grep -nE "modified since read|stale.read|re-Read" AGENTS.md agents/*.md docs/agent-rules/*.md
   ```
   If nothing, this is greenfield documentation.

2. Add a sub-section to `AGENTS.md` § Project rules (or `docs/agent-rules/delegation.md` if that's where stale-state contracts live — check both):
   ```markdown
   ### Stale-read recovery on `Edit`

   `Edit` may error with `File has been modified since read, either by the user or by a linter` when:

   1. A concurrent orchestrator in a sibling worktree edited the same file.
   2. A PostToolUse hook (e.g. `lint-cpp.sh`'s `clang-format -i`) rewrote the file between your `Read` and `Edit`.
   3. The user touched the file in their editor.

   Canonical recovery — always works, no manual conflict resolution:

   1. Re-`Read` the file at the same path (and same offset/limit if you used them).
   2. Diff your intended change against the new content — verify the `old_string` you were going to pass still exists verbatim. If a hook reformatted it (e.g. trailing whitespace stripped, line wrapped), update `old_string` to the new exact form.
   3. Re-`Edit` with the refreshed `old_string`.

   Hot files (high race rate, expect to re-Read at least once per edit):

   - `_plan-locks.md` (every orchestrator that takes / releases a plan-lock touches it)
   - `AGENTS.md` (multi-agent doc edits)
   - `docs/backlog/agent-self-improvement/*.md` (parallel self-improvement appends)

   Do NOT use `replace_all: true` as a "force-write" — it amplifies race-collision risk by widening the rewrite surface. Stick to the targeted Re-Read + Re-Edit pattern.
   ```

3. Optional defensive measure — wrap `_plan-locks.md` edits in a file-lock via `scripts/dev/_lock-json.py` (which already exists, per the dir inventory). Defer to a follow-up if the existing lock-claim scripts don't already cover this; the stale-read recovery is the load-bearing fix.

4. Flip the backlog entry from `Status: observational` → `Status: applied` and move to `applied.md` once the AGENTS.md sub-section lands.

**Verification**:
- `grep -n "Stale-read recovery" AGENTS.md` returns at least one hit.
- The next orchestrator session that hits a stale-read error follows the canonical recovery (which is the same pattern they were already using — now it's documented for future agents).

**Risk**: trivially low. Doc only. Risk is over-claiming a fix when the race itself persists; mitigation is the explicit `OBSERVATIONAL` → `applied` flip naming the doc as the deliverable, not a code fix.

---

## Cross-slice considerations

- **One PR, six commits**: easier review (each commit maps 1:1 to a backlog item) + easier rollback if one slice regresses. Branch name: `chore/process-rules-tighten-1-2-3-9-11-12`.
- **Backlog hygiene**: every slice ends by moving its entry from `process.md` to `applied.md` with a resolution stanza linking back to this plan + the commit sha. Net: `process.md` shrinks by 6 entries; `applied.md` grows by 6.
- **`grill-with-docs`**: run on 2026-05-19. 9 decisions captured (Q1–Q9). Material changes — Slice 1 trigger tightened to two-part (visual-path touch AND no bucket-C/E coverage); Slice 1 "no" handling = leave-dirty in-place iteration (not stash); Slice 3 single-file hook (no sidecar split); Slice 4 target file = `docs/agent-rules/delegation.md` (not AGENTS.md — codebase probe revealed § Orchestrator delegation packet body lives there); Slice 4 ships `plan-doc-table-probe.sh` + bucket-A self-test in v1 (not deferred); Slice 5 carve-out covers both `agent/*` AND `claude/*` (not `agent/*` only); Slice 5 step 5 = backlog entry to `process.md` (concrete artifact); ADR 0005 added for the force-push carve-out (passes hard-to-reverse + surprising + real-trade-off). No new glossary terms; CONTEXT.md unchanged.
- **Slice 3 wiring decision**: §Steps 2 names option A vs option B for hook installation. Slice 3 is the only piece with a real design choice; flagged for orchestrator decision before implementation.
- **Trivial-visual-only envelope** does **not** apply (this is docs + 1 small script, not visual). Slice 3 is the only piece that needs a build / test pass; Slices 1, 2, 4, 5, 6 are pure-docs and qualify for the Slice-2-introduced pure-docs sub-exception (chicken-and-egg: ship Slice 2 first to make the pure-docs path real, OR rely on the existing FF-clean test-all.sh gate for this PR).

## Out of scope

- Items not in {1, 2, 3, 9, 11, 12}: items 4, 5, 6, 7, 8, 10 from the Process (13) list are not covered. Future plan.
- Code-level enforcement of any of these rules (linters, pre-commit hooks beyond Slice 3, CI gates). All six slices ship as doc edits; tooling-level enforcement is a separate effort tracked under category `tooling`.
- Item 12's underlying race (`_plan-locks.md` concurrent writes). Slice 6 documents the recovery; the race itself is upstream and out of scope.

## Implementation log

**Plan status: complete** (all 6 slices shipped; backfilled 2026-05-23 per
AGENTS.md § Plan revision after implementation — the original PR landed
without updating this section).

- **`bc93baf`** *(PR #309, merged 2026-05-19)* · `chore: tighten 6
  process-backlog rules (items 1/2/3/9/11/12)` — single bundled PR
  shipping every slice on this plan. Per the commit message:
  - **Slice 3** (item 3, P2) · `scripts/git-hooks/pre-push` (executable
    single-file hook — grill Q2=A collapse); `scripts/setup-harness.sh`
    `install_git_hooks()` sets `git config --local core.hooksPath` only
    when current path is unset or already equal (no trample);
    `scripts/dev/test-pre-push-merged-pr-guard.sh` (bucket-A, 9 cases,
    12/12 PASS); `SMATCHET_ALLOW_MERGED_PR_PUSH=1` override. Backlog
    entry moved `process.md` → `applied.md` in the same commit.
  - **Slice 1** (item 1, P1) · AGENTS.md § Autonomous ship-loop default
    new exception #5 (Visual-validation exception) — two-part trigger
    (visual-path touch AND no bucket-C/E coverage, per grill Q1=A).
    Pillar 4 § Visual-validation acceptance docs the contract.
  - **Slice 2** (item 2, P2) · `agents/git-janitor.md` § FF-clean
    docs-batch exception § Pure-docs sub-exception relaxes precondition
    4 (`test-all.sh`) for diffs strictly within `docs/**` / `backlog/**`
    / `AGENTS.md` / uppercase root `*.md`. Discriminator helper
    `scripts/dev/is-pure-docs-diff.sh` ships in the same commit.
  - **Slice 4** (item 9, P3) · `docs/agent-rules/delegation.md` §
    Orchestrator delegation packet § File-level table re-verify (before
    sealing) — orchestrator runs `git grep <symbol-list>` +
    `grep -n <cmake-variable> CMakeLists.txt` for every symbol / CMake
    variable named in a plan's § File-level changes table at packet-seal
    time. Mechanical shortcut: `scripts/dev/plan-doc-table-probe.sh
    <plan-doc-path>` parses + probes for every named row, exit 0 if all
    resolve / exit 1 on any miss; auto-enrolled into `test-all.sh` via
    its own self-test `scripts/dev/test-plan-doc-table-probe.sh`.
  - **Slice 5** (item 11, P3) · AGENTS.md § Delegation § API-500
    mid-run recovery (5-step procedure) — later lifted to
    `docs/agent-rules/delegation.md` § API-500 mid-run recovery by PR
    #417's AGENTS.md reduction.
  - **Slice 6** (item 12, P3 OBSERVATIONAL) · AGENTS.md (later
    `docs/agent-rules/process-rules.md`) § Stale-read recovery on
    `Edit` — canonical recovery pattern (Re-Read → diff intended change
    → Re-Edit; never `replace_all` as force-write) for the concurrent-
    orchestrator / linter-rewrite race. Backlog entry status flipped
    `observational` → `applied` in the same commit.
- **`96ab99f`** *(PR #417, merged 2026-05-23)* · `feat(agents-md-reduction):
  lift 4 topical sections from AGENTS.md to docs/agent-rules/` — moved
  the slice-1 / slice-5 / slice-6 prose blocks from AGENTS.md into the
  new topic files (`ship-loops.md` / `delegation.md` / `process-rules.md`)
  without behavioural change. AGENTS.md retains 1-line stubs that link
  out so external `AGENTS.md § <subsection>` cross-references still
  resolve.

## Deviations from plan

- **Original ship order (3 → 1 → 2 → 11 → 9 → 12) collapsed into one PR.**
  Plan prescribed one commit per slice on a single branch; the actual
  PR #309 commit history follows the spec (one commit per slice in the
  prescribed order — `pre-push` first, then visual-validation, then
  pure-docs, then API-500, then plan-doc-table-probe, then stale-read).
  Squash-merge collapsed all 6 into the single SHA above.
- **No relocation deviations** in PR #417. The four lifted topical
  sections (Merge gates / Ship-loops / Delegation / Process rules)
  carried the slice-1 / slice-5 / slice-6 content unchanged.

## Verification

All six slices verified live on develop tip 2026-05-23 (backfill
walkthrough):

| Slice | Live evidence on develop |
|---|---|
| 1 (Visual-validation) | `AGENTS.md` § Autonomous ship-loop default exception #5 + `docs/agent-rules/ship-loops.md` § Visual-validation exception |
| 2 (Pure-docs sub-exception) | `agents/git-janitor.md` § FF-clean docs-batch § Pure-docs sub-exception + `docs/agent-rules/process-rules.md` § Pure-docs slice skip + `scripts/dev/is-pure-docs-diff.sh` |
| 3 (pre-push hook) | `scripts/git-hooks/pre-push` + `scripts/dev/test-pre-push-merged-pr-guard.sh` (auto-enrolled by `test-all.sh` glob); `scripts/setup-harness.sh:install_git_hooks()` sets `core.hooksPath`. Git tracks both with mode `100644` (Windows quirk — `git config core.fileMode false` is default), so `scripts/dev/*.sh` runs via `bash <path>` and the exec bit is moot for the bats wrapper. The hook itself runs the same way on Windows via Git Bash's shebang dispatch; non-Windows operators wanting the hook to fire on plain `git push` may need to `chmod +x scripts/git-hooks/pre-push` once locally. |
| 4 (plan-doc table re-verify) | `docs/agent-rules/delegation.md` § Orchestrator delegation packet § File-level table re-verify + `scripts/dev/plan-doc-table-probe.sh` + `scripts/dev/test-plan-doc-table-probe.sh` |
| 5 (API-500 recovery) | `docs/agent-rules/delegation.md` § API-500 mid-run recovery (5-step procedure with `git add -A` gotcha called out) |
| 6 (stale-read recovery) | `docs/agent-rules/process-rules.md` § Stale-read recovery on `Edit` (Re-Read → diff intended change → Re-Edit; never `replace_all` as force-write) |

Backfill cross-walk command set (re-runnable):

```bash
grep -c 'Visual-validation exception' AGENTS.md docs/agent-rules/ship-loops.md
grep -c 'Pure-docs sub-exception' agents/git-janitor.md docs/agent-rules/process-rules.md
ls -la scripts/git-hooks/pre-push scripts/dev/test-pre-push-merged-pr-guard.sh
ls -la scripts/dev/plan-doc-table-probe.sh
grep -c 'File-level table re-verify' docs/agent-rules/delegation.md
grep -c 'API-500 mid-run recovery' docs/agent-rules/delegation.md
grep -c 'Stale-read recovery' docs/agent-rules/process-rules.md
```
