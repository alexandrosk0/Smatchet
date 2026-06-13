# Ship loops

> Lifted from [`AGENTS.md`](../../AGENTS.md) § Autonomous ship-loop default per [`docs/plans/shipped/agents-md-reduction.md`](../plans/shipped/agents-md-reduction.md). AGENTS.md retains a load-bearing stub naming the default sequence + exceptions + post-ship turn-end menu so external `AGENTS.md § <subsection>` references continue to resolve. Edit this file directly — no parallel copy in AGENTS.md.

## Autonomous ship-loop default

**Rule**: orchestrator runs each user task end-to-end in **one turn** without pausing for confirmation at each stage. The default sequence:

```
diagnose → fix → build → [pre-first-push gate] → commit → push → open PR → [gate-check] → squash-merge → git-janitor cleanup → backlog entry
```

**`[pre-first-push gate]` (mandatory before the FIRST push of a feature branch — `reduce-coderabbit-review-spend` Slice 1).** Before the branch ever reaches GitHub (where CodeRabbit's `auto_review` fires on every push), the implementer MUST have run, locally, the full pre-merge gate set + a subsystem self-review — never deferring locally-knowable findings to CI/CR. The gate is:

1. `bash scripts/dev/pre-ship.sh` — clang-format + strict-zone delta + comment-noise + function-size + md_lint → green.
2. Dual-target `/WX` build when the shared core changed — the dual-target build per [`build.md`](build.md) (both the standalone and the embedded-core targets) → exit 0, warning-clean (warnings are errors).
3. The relevant test-preset `ctest` subset (build presets in [`build.md`](build.md) / `project.config.json`) when logic changed → green.
4. **Leaf-`AGENTS.md` self-review** — for every strict-zone dir touched, re-read its leaf `AGENTS.md` and self-check the diff against its invariants (e.g. the Sync audit begin/result pair, Persistence additive-schema + presence-not-emptiness, no empty `catch(...)`). These are exactly the finding classes CR raised on #854; catching them pre-push removes a whole CR round-trip.

**Why:** the last-10-PRs CR-spend audit found re-pushes (each a fresh CR auto-review) dominate review cost, concentrated in PRs that fix-cycled on locally-knowable issues after the first push. The gate lands CR's first review on near-final code. It does NOT eliminate CI-toolchain-version-only findings (e.g. MSVC-14.40+ `/WX` warnings unreproducible on an older local toolset) — those are accepted residue. Batch all CR-fix-round changes into a **single** push (don't push-per-finding). See [`docs/agent-rules/process-rules.md`](process-rules.md) § Cadence + verification for the once-per-slice build/test discipline this composes with.

`[gate-check]` is the merge-gates poller (see [`docs/agent-rules/merge-gates.md`](merge-gates.md)) — polls CI + CodeRabbit + user-comments before squash-merge. Triggered only when the user has explicitly authorised this PR for merge (post-ship option 3 "Register with watcher" or in-session "merge when green"). The `smatchet-merge-watcher` host daemon (per `docs/plans/shipped/smatchet-merge-watcher.md`) takes over from this point when the user picks post-ship option 3; the orchestrator's in-session role ends at register-time. Halt + `AskUserQuestion` on block / timeout / `gh` API failure / PR closed-externally / pagination overflow.

All clarifications that the orchestrator anticipates needing are batched **once at the start** via `AskUserQuestion`. Once the user answers, the loop proceeds without further prompts until completion (or until an exception below fires).

**Do-not-pause checklist** — stages where the model is most likely to incorrectly pause for confirmation. At each of these, proceed automatically unless a defined exception applies:

1. **After opening the PR**: start `agents/scripts/core/merge-gates.sh` polling immediately. DO NOT ask "should I poll?" or "will you check manually?"
2. **After CodeRabbit posts actionable findings**: fetch the CR comments, assess each finding, fix valid ones, push, and resume polling. DO NOT ask the user whether to address CR findings.
3. **After `GATES_PASSED`**: squash-merge immediately (when authorised). DO NOT ask "should I merge now?"

   **Append a merge-time gate-verdict snapshot (mandatory, all merge actors).** Immediately after the `gh api -X PUT repos/<owner>/<repo>/pulls/<N>/merge -f merge_method=squash` call returns (capture its `.sha` as `<mergeCommit>`), the in-session orchestrator merge **and** `git-janitor` MUST append a lossless snapshot line to the committed ledger `docs/self-improvement/merge-snapshots.jsonl`. This captures the override-labels + red-checks *at the merge-decision instant* — the only lossless capture, since GitHub overwrites `statusCheckRollup` contexts on re-run and override labels are stripped post-merge (`docs/adr/0017-merge-time-snapshot-ledger.md`). The `merge-watcher` daemon does this in `handle_pass()`; the two in-session actors run the shared helper directly:

   ```bash
   bash agents/scripts/core/merge-snapshot-append.sh \
       <N> <mergeCommit> <headSha> GATES_PASSED "" "<override-labels-csv>" orchestrator
   #                                              ^redChecks="" on the GATES_PASSED path
   # git-janitor passes mergeActor 'git-janitor' as the final arg instead of 'orchestrator'.
   ```

   `<headSha>` is the PR head oid (`gh pr view <N> --json headRefOid --jq .headRefOid`); `<override-labels-csv>` is the comma-joined subset of the PR's labels that are in `project.config.json` `merge_gates.override_labels`. The helper is idempotent on `pr`+`mergeCommit` (a retry is a no-op) and never aborts the merge path — a write failure is logged, not fatal (`postmortem-owed.sh` keeps the live `statusCheckRollup` fallback for un-snapshotted merges).
4. **After merge conflicts on rebase**: resolve conflicts autonomously (prefer the semantically correct version), force-push the rebased branch, and resume polling. DO NOT ask which side to keep unless both sides are substantive and ambiguous.
5. **After squash-merge succeeds**: proceed to git-janitor cleanup and backlog entry. DO NOT ask "anything else?"
6. **Closeout issue-sweep** (advisory, non-blocking): run `bash agents/scripts/core/issue-sweep.sh` (dry-run) — surfaces open-Issue triage verdicts + the top-`P0`/`P1` `[issue-propose]` line (`docs/agent-rules/issue-triage.md` § Fixing an Issue). It **proposes** the next bug; it does **not** auto-fix and does **not** pause the loop. Deeper / periodic triage is the `issue-janitor` (off-loop).
7. **Plan-archive (if this work shipped from a plan-doc)**: the post-ship PR that fills `docs/plans/active/<slug>.md` § Implementation log MUST, in the **same** PR, flip the plan's `> **Status**:` header to `shipped` AND `git mv` it to `docs/plans/shipped/` + `test-plan-index.sh --fix` (`docs/agent-rules/process-rules.md` § Archive on revision; template § Archive checklist). **No ref-sweep** — plan references are tier-less (`docs/plans/<slug>.md`, resolved against any tier; PR #890), so the move can't break them. The `git mv` is the historically-dropped step — `plan-archival-owed.sh` nags at SessionStart for any `active/` plan already marked `shipped`. DO NOT defer the move to a later PR.

The post-ship 4-option `AskUserQuestion` is the **first** user-facing prompt after the initial clarification batch.

**Why a default**: harnesses that drip-step every stage create N round-trips for a task that needs one. The user already chose the task; the loop is the cheapest way to deliver it. Other harnesses (Codex / Cursor / Aider) read AGENTS.md and need the rule too — user-private memory is not portable.

**Exceptions** (loop pauses or stops):

1. **Debug-mode pause-loop** — user prompt matches the `debug-detective` trigger row (see [`docs/agent-rules/delegation.md`](delegation.md) § Trigger auto-activation). The pause-loop in [`docs/agent-rules/delegation.md`](delegation.md) § Debug-mode pause-loop **overrides** the ship-loop for the duration of the investigation.
2. **Destructive ops outside loop** — `git reset --hard`, `git push --force` to a shared branch, `git branch -D`, `gh pr merge` of a non-self PR, `rm -rf` outside the worktree, schema drops. These require explicit confirmation per [`docs/agent-rules/process-rules.md`](process-rules.md) § Destructive git ops in shared worktrees.
3. **Cross-repo or external-service mutations** — anything that writes outside the current repo or calls a third-party API with side effects (posting to Slack, sending email, modifying a Jira ticket the user didn't ask for). Confirm before acting.
4. **Anything not previously authorised in a durable rule** — durable = recorded in AGENTS.md, CLAUDE.md, or this session's explicit user instructions. Verbal "ok in this conversation" doesn't bind future turns; encode it as a memory or doc edit if it should.
5. **Visual-validation exception** — fires when **both** conditions hold (owning agent: `ui-host`, which owns the theme / dockspace / host-bootstrap surface):
   1. Diff touches at least one of: `Source/Core/src/Ui/SmatchetTheme.cpp`, `Source/Core/src/Ui/Smatchet*Ui*.cpp`, `Source/Core/include/Ui/SmatchetTheme.h`, `Locales/*.json`, ImGui style constants (`ImVec4` / `ImGuiStyle` literals), dock-layout init paths.
   2. AND no bucket-C screenshot diff or bucket-E ImGui-Test-Engine scenario covers the changed widget.

   When both fire, the loop pauses after **build** with the launched exe. The orchestrator **always auto-launches** `build/<preset>/Smatchet.exe` itself via a single `bash` call with `run_in_background: true` — it does **not** present a launch-method `AskUserQuestion` ("launch manually" / "launch in background" / "ship without verify"). Just run it, report the task id + exe path inline, then ask only the verdict question. (Standing user instruction, 2026-05-22.) Skip the launch only when `Source/Core/` was not touched — there is no visual change to verify. Orchestrator presents:
   - the `build/<preset>/Smatchet.exe` path + a one-line run command,
   - the `bash` background-task id of the launched exe (or "launched manually"),
   - the specific visual change the user is asked to evaluate (one sentence).

   Wait for the user's verdict before commit+push. On "looks good" → resume the loop and commit. On "no" → leave the working tree dirty; iterate in-place. The orchestrator does `git diff` between attempts to see what was tried. Clean-slate reset (`git checkout -- <files>`) only when the user explicitly asks for one. Never commit+push an unvalidated visual change.

   Out-of-scope (NOT a visual-validation pause):
   - A change with no test coverage but no visual-path touch — that's a Pillar-3 "needs test coverage" problem, route via the test backlog.
   - A change that touches the visual paths AND has bucket-C/E coverage — coverage is the gate; ship-loop continues. If the user disagrees with the golden after merge, the bucket-C golden is re-bootstrapped per [`docs/agent-rules/process-rules.md`](process-rules.md).

   Pillar anchor: see [`docs/agent-rules/quality-pillars.md`](quality-pillars.md) § 4 § Visual-validation acceptance for the cross-link from the pillar side.

## PR batching — logical-feature granularity

**Rule**: the orchestrator opens **one PR per logical feature, not one PR per slice/task/fix**. Related slices that serve a single coherent goal accumulate on one feature branch and ship as a **single** PR. This is the default; it overrides any reflex to open a fresh PR per stage of the ship-loop.

**Why** — CodeRabbit imposes two limits that one-PR-per-change violates from both ends:

1. **Volume / review quota** — a finite number of CR reviews per period. N tiny PRs burn N reviews for what is conceptually one change; the quota is exhausted before the real work is reviewed.
2. **Per-PR file-count ceiling** — above CodeRabbit's configured file limit, CR posts a *review-skipped — too many files* comment (marker `skip review by coderabbit.ai`) and the merge gate **blocks** (see [`docs/agent-rules/merge-gates.md`](merge-gates.md) § CodeRabbit gate). A PR batched past the ceiling ships unreviewed or stalls.

Logical-feature granularity is the band between the two: few enough PRs to respect the quota, small enough each to stay reviewable.

**What counts as one logical feature** — one coherent goal or subsystem change. A bug fix plus its test plus the doc note = one PR. Three unrelated typo fixes in three subsystems = still one PR if trivial, but a feature in subsystem A and an unrelated feature in subsystem B = **two** PRs. The seam is *conceptual cohesion*, not file count or commit count.

**Boundary rules:**

- **Default to batching.** Within a session, multiple small tasks that serve one goal commit to the **shared feature branch**; the PR opens **once** when the feature is coherent — not after each slice. Do not `gh pr create` per slice.
- **File ceiling is a hard cap.** If a single logical feature's cumulative diff would exceed CodeRabbit's file ceiling (the review-skipped threshold), split it along natural seams (e.g. by layer or by independent sub-feature) into multiple PRs and note the split rationale in each PR body. Never batch past the ceiling expecting `cr-out-of-band` to paper over it — that label is for genuinely-unsplittable over-limit PRs, not routine batching.
- **Unrelated work never shares a PR** just to cut count. Cohesion first, count second. Two unrelated changes in one PR makes review harder and couples their merge fates.
- **Stacked / sequential slices** that depend on each other still land as one PR when they form one feature; only split when an earlier slice is independently shippable AND the combined diff risks the file ceiling.
- The do-not-pause checklist, merge gates, and post-ship menu all fire **per PR** — with batching, one PR may cover several user tasks from the session. That is expected; the menu fires once per feature-PR, not once per task.

**Interaction with the default sequence** — the `commit → push → open PR` stages still run, but `open PR` is reached once per logical feature. Earlier slices of the same feature stop at `commit` (+ `push` to the shared feature branch) and defer PR creation until the feature is whole.

**P4-mode note** — the same logical-feature granularity governs the P4-gated loop: the single `gh pr create` at the end of the P4 flow covers the whole feature, and `code-review` is already dispatched once per task (cumulative diff), which aligns with one-PR-per-feature.

## P4-gated ship-loop

When `SMATCHET_AGENT_VCS=p4`, the orchestrator follows a **P4-gated ship-loop** instead of the default git ship-loop. The variant exists so the user reviews a Perforce shelf in P4V before any `git push` or `gh pr create` happens — git remains the ship-line, but git is touched **once**, at the end, after the change is known-good. The pause-loop in [`docs/agent-rules/delegation.md`](delegation.md) § Debug-mode pause-loop continues to **override** this loop for debug-detective triggers (same as it does for the default git loop).

**Fires when** `SMATCHET_AGENT_VCS=p4` is set at session start AND `p4 info` confirms the local server is reachable. On `p4 info` failure → `LOG_ERROR "p4-mode requested but Perforce not bootstrapped"` + `AskUserQuestion`: (a) fall back to default git ship-loop for this session, (b) abort, (c) follow [`docs/perforce/SETUP.md`](../perforce/SETUP.md) and retry. Never silently downgrade.

**Two sub-variants** — orchestrator asks once at task start via `AskUserQuestion` which to use:

1. **Small-change loop** (default; single slice, single subsystem) — work directly on `//smatchet/main` via the canonical client. Iterate edits in a pending CL. Smoke build → shelve → user review → full tests → submit → git branch + push + PR.
2. **Task-stream loop** (multi-slice OR write set spans multiple subsystems; only when user explicitly approves) — allocate `bash agents/scripts/project/p4-task-stream.sh <id>`. Each slice submits to the task stream's depot path. End-gate runs the full battery, then `bash agents/scripts/project/p4-task-stream-to-pr.sh <id> "<title>" --prepare-review-cl` integrates into a pending main-stream CL + shelves. User reviews shelf. On approval, `--promote-reviewed-cl <CL>` submits + creates git branch + push + PR.

**Key invariants (both sub-variants):**

- `git push` / `gh pr create` happen **once**, after shelf approval AND full test-pass.
- Shelf-review gate fires **exactly once** per task. Test failures post-approval → fix → re-test without re-review. Re-review only on explicit user request.
- **No `git worktree add`** while `SMATCHET_AGENT_VCS=p4` — subagent isolation uses `agents/scripts/project/p4-task-stream.sh` exclusively. The first (and only) git write is the **plumbing-commit promote step** ([`AGENT_FLOWS.md`](../perforce/AGENT_FLOWS.md) § Promote-to-PR in a shared tree): build the PR commit in a temp index + `git branch` + push, touching neither HEAD nor the working tree. Do **not** `git checkout -b` in the shared canonical tree — it trips `guard-shared-tree.sh` under concurrent sessions, and the `checkout` back to `develop` reverts the just-submitted files on disk (diverges the working tree from the p4 depot head until merge). The `SMATCHET_ALLOW_SHARED_SWITCH=1` override unblocks the guard but keeps the working-tree flip-flop — the plumbing recipe needs no override.
- **Smoke build precedes shelf** — user never sees a non-compiling change in P4V.
- **`code-review` agent dispatched ONCE per task** at the end-gate / shelf step (cumulative diff). Not per slice.
- Pure-docs slice skip still applies. Trivial-visual-only envelope still applies, with `p4 sync` + `p4 edit -t +l` substituting for `git stash` race-recovery.
- Plan-lock backend auto-flips to `p4-counter` **only when unset** — `export SMATCHET_LOCK_BACKEND="${SMATCHET_LOCK_BACKEND-p4-counter}"` (no colon — empty-string setting is preserved per `agents/scripts/project/test-p4-dual-vcs.sh` scenario 2 line 149 + scenario 6 line 369).
- Post-ship `AskUserQuestion` ALWAYS fires with option 3 ("Register with watcher") pre-selected; when `docs/plans/shipped/merge-gates-ci-coderabbit-comments.md` ships end-to-end the `AskUserQuestion` goes away entirely in p4-mode.

Full phase sequence + invariants + exception rules in [`docs/perforce/AGENT_FLOWS.md`](../perforce/AGENT_FLOWS.md) § P4-gated ship-loop. Plan: [`docs/plans/shipped/p4-gated-ship-loop.md`](../plans/shipped/p4-gated-ship-loop.md). ADR: [`docs/adr/0008-p4-gated-ship-loop.md`](../adr/0008-p4-gated-ship-loop.md).

## Post-ship turn-end protocol

After the loop reaches PR-opened (or the equivalent terminal state for the task), end the turn with `AskUserQuestion` offering the four canonical next steps as discrete options:

1. **Manual verify** — user wants to drive the change manually before merge.
2. **Review PR** — user wants to read the diff / comment on GitHub.
3. **Register with watcher** — orchestrator runs `merge-watch register <pr>` (per [`docs/plans/shipped/smatchet-merge-watcher.md`](../plans/shipped/smatchet-merge-watcher.md)). The `smatchet-merge-watcher` host daemon's first step on register is `gh pr ready <n>` (idempotent — no-op if already non-draft) so CodeRabbit's `auto_review.drafts: false` doesn't skip the review (per `docs/self-improvement/categories/process.md` P1 — draft PRs silently bypassed CR for 15+ session PRs before this rule landed). Then it runs the gate-check loop + CodeRabbit-triage loop + REST-squash-merge per the watcher contract. Session can close immediately; watcher persists. Halt prompts surface as Smatchet notifications via `SmatchetToastManager` (watcher Phase 4), not back to this session. **If the session does NOT close — i.e. the orchestrator registers but commits to reporting the outcome in-session — it MUST arm the session-side signal, since the daemon is silent to it (a RED check would otherwise sit unsurfaced indefinitely): `Monitor(bash scripts/dev/pr-status-watch.sh <pr> [<pr>…])`, which emits one notification per terminal state — RED / MERGED / CLOSED / green-but-stuck (the last catches a wedged daemon). Don't promise "I'll report when it lands" without arming it (`register-with-watcher-session-blind-to-red`).**
4. **Done** — no further action; PR stays draft for later.

Do **not** emit a free-form bulleted next-steps list — `AskUserQuestion` is a single click; prose is N seconds of composition.

**Skip-condition**: if the user has already said "no more changes coming" / "ship it and stop" / "merge when green" in the same turn, skip the question and enter option 3 directly (`git-janitor` invokes `merge-watch register` before walking away).

**Session-wide opt-in auto-register (`SMATCHET_WATCH_ALL_PRS`)**: when this env var is set truthy (`1` / `true` / `yes` / `on`), the orchestrator runs `agents/scripts/core/watch-register-if-enabled.sh <pr>` immediately after `gh pr create` for every PR it opens that session, auto-registering it with the watcher so no green PR sits unwatched waiting on the menu. The helper is a **no-op when the flag is unset** (the default), so the explicit-authorization model above (post-ship option 3 / "merge when green") still governs by default. Registering a PR *is* the authorization to auto-merge it, so this flag is the knowing, session-scoped way to make a whole session hands-off without weakening the per-PR gate for sessions that don't opt in. It does **not** bypass the merge gates themselves — CI + CodeRabbit + user-comment checks still must pass before the watcher squash-merges. Tests: `tests/bats/merge_watcher.bats` (`watch-register:` cases).

## Loop modes (governance — see `AI_POLICY.md`)

[`AI_POLICY.md`](../../AI_POLICY.md) § Two loop modes defines the human-selected operating mode (env var `SMATCHET_LOOP_MODE`, surfaced by the SessionStart `## === loop-mode: <on|in> ===` banner from `agents/scripts/core/clear-session-context.sh`; config default in `project.config.json` § `governance`):

- **human-on-the-loop** (`on`) — the action-biased mode defined by the **Standing user default** paragraph below: commit/push/PR autonomously, resolve reversible forks with a default + surface them, pause only on the enumerated exceptions. The agent acts; the human monitors and can interrupt.
- **human-in-the-loop** (`in`, **prerelease default**) — execute **only within an approved plan**; pause at each decision point the plan does **not** cover; do not improvise scope. The human authorises a plan, the agent runs it. Absent an explicit `SMATCHET_LOOP_MODE`, assume this.

**Escalate, don't assume — pause-exception (6), fires in BOTH modes.** Before acting, the agent must be able to *autonomously validate* the action (a gate/test/spec confirms correctness, scope is authorised, cost is bounded). If it cannot — ambiguous spec, no gate to confirm, irreversible-and-unauthorised, or cost-unbounded — it **escalates** via `AskUserQuestion` with the blocker named, never guessing. This is a *new* pause trigger layered onto both modes, **not** a weakening of on-the-loop autonomy within its authorised scope. **Cost:** token/compute is a human-governed budget (gauged by `agents/_shared/token-tracking/`); surface cost and escalate *before* an unbounded or expensive autonomous run.

The MUST-NOT-pause rule (`AGENTS.md` § Autonomous ship-loop default + the standing default below) holds **within the active mode's authorised scope** — on-the-loop grants full autonomy there; in-the-loop's scope is the approved plan; exception (6) bounds both. No rule below is deleted — they are placed under this mode spectrum.

**Standing user default — ship + auto-register, menu suppressed** (set 2026-05-30; **this is the on-the-loop mode definition**): this user's default operating mode is action-biased. Commit / push / open-PR **every** change autonomously (never ask "should I commit?"), and treat registering each PR with the watcher as **pre-authorized** — enter option 3 directly instead of presenting the 4-option menu. Decide **reversible** forks (doc wording, backlog-bucket placement, file naming) with a sensible default and surface the choice in the turn summary rather than blocking on `AskUserQuestion`. Still pause for the genuine exceptions the ship-loop already enumerates: real design forks (multiple substantive approaches where a wrong guess = real rework), destructive/irreversible ops, cross-repo / external-service mutations, anything not durably authorized. This is the per-user equivalent of `SMATCHET_WATCH_ALL_PRS` but expressed as the operator's standing instruction; it does not weaken the merge gates (CI + CodeRabbit + comments still bind).

**Register only after EVERY intended commit is pushed — the watcher merges the instant gates go green** (incident 2026-06-01, PR #681): the watcher polls and squash-merges independently of the in-session orchestrator; it does **not** wait for you to finish pushing follow-up commits. A fast-passing early commit (e.g. a one-line index regen) can trip the green gate before the substantive work lands, merging a partial PR — then the next push re-creates the deleted branch with no open PR and the late commit is orphaned, needing a fresh branch + PR. So: hold `merge-watch register` until the branch is final. If more commits are coming, do not register yet; if a commit lands after merge, open a follow-up PR for it (it is its own logical-feature slice per § PR batching).

Cross-link: ship-loop reference in [`docs/agent-rules/delegation.md`](delegation.md); pause-loop override in [`docs/agent-rules/delegation.md`](delegation.md) § Debug-mode pause-loop; gate semantics + halt prompts in [`docs/agent-rules/merge-gates.md`](merge-gates.md); watcher integration in [`docs/plans/shipped/smatchet-merge-watcher.md`](../plans/shipped/smatchet-merge-watcher.md).
