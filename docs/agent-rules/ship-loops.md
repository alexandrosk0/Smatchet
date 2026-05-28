# Ship loops

> Lifted from [`AGENTS.md`](../../AGENTS.md) § Autonomous ship-loop default per [`docs/design/agents-md-reduction.md`](../design/agents-md-reduction.md). AGENTS.md retains a load-bearing stub naming the default sequence + exceptions + post-ship turn-end menu so external `AGENTS.md § <subsection>` references continue to resolve. Edit this file directly — no parallel copy in AGENTS.md.

## Autonomous ship-loop default

**Rule**: orchestrator runs each user task end-to-end in **one turn** without pausing for confirmation at each stage. The default sequence:

```
diagnose → fix → build → commit → push → open PR → [gate-check] → squash-merge → git-janitor cleanup → backlog entry
```

`[gate-check]` is the merge-gates poller (see [`docs/agent-rules/merge-gates.md`](merge-gates.md)) — polls CI + CodeRabbit + user-comments before squash-merge. Triggered only when the user has explicitly authorised this PR for merge (post-ship option 3 "Register with watcher" or in-session "merge when green"). The `smatchet-merge-watcher` host daemon (per `docs/design/archive/smatchet-merge-watcher.md`) takes over from this point when the user picks post-ship option 3; the orchestrator's in-session role ends at register-time. Halt + `AskUserQuestion` on block / timeout / `gh` API failure / PR closed-externally / pagination overflow.

All clarifications that the orchestrator anticipates needing are batched **once at the start** via `AskUserQuestion`. Once the user answers, the loop proceeds without further prompts until completion (or until an exception below fires).

**Do-not-pause checklist** — stages where the model is most likely to incorrectly pause for confirmation. At each of these, proceed automatically unless a defined exception applies:

1. **After opening the PR**: start `scripts/dev/merge-gates.sh` polling immediately. DO NOT ask "should I poll?" or "will you check manually?"
2. **After CodeRabbit posts actionable findings**: fetch the CR comments, assess each finding, fix valid ones, push, and resume polling. DO NOT ask the user whether to address CR findings.
3. **After `GATES_PASSED`**: squash-merge immediately (when authorised). DO NOT ask "should I merge now?"
4. **After merge conflicts on rebase**: resolve conflicts autonomously (prefer the semantically correct version), force-push the rebased branch, and resume polling. DO NOT ask which side to keep unless both sides are substantive and ambiguous.
5. **After squash-merge succeeds**: proceed to git-janitor cleanup and backlog entry. DO NOT ask "anything else?"

The post-ship 4-option `AskUserQuestion` is the **first** user-facing prompt after the initial clarification batch.

**Why a default**: harnesses that drip-step every stage create N round-trips for a task that needs one. The user already chose the task; the loop is the cheapest way to deliver it. Other harnesses (Codex / Cursor / Aider) read AGENTS.md and need the rule too — user-private memory is not portable.

**Exceptions** (loop pauses or stops):

1. **Debug-mode pause-loop** — user prompt matches the `debug-detective` trigger row (see [`docs/agent-rules/delegation.md`](delegation.md) § Trigger auto-activation). The pause-loop in [`docs/agent-rules/delegation.md`](delegation.md) § Debug-mode pause-loop **overrides** the ship-loop for the duration of the investigation.
2. **Destructive ops outside loop** — `git reset --hard`, `git push --force` to a shared branch, `git branch -D`, `gh pr merge` of a non-self PR, `rm -rf` outside the worktree, schema drops. These require explicit confirmation per [`docs/agent-rules/process-rules.md`](process-rules.md) § Destructive git ops in shared worktrees.
3. **Cross-repo or external-service mutations** — anything that writes outside the current repo or calls a third-party API with side effects (posting to Slack, sending email, modifying a Jira ticket the user didn't ask for). Confirm before acting.
4. **Anything not previously authorised in a durable rule** — durable = recorded in AGENTS.md, CLAUDE.md, or this session's explicit user instructions. Verbal "ok in this conversation" doesn't bind future turns; encode it as a memory or doc edit if it should.
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
   - A change that touches the visual paths AND has bucket-C/E coverage — coverage is the gate; ship-loop continues. If the user disagrees with the golden after merge, the bucket-C golden is re-bootstrapped per [`docs/agent-rules/process-rules.md`](process-rules.md).

   Pillar anchor: see [`docs/agent-rules/ux-pillars.md`](ux-pillars.md) § 4 § Visual-validation acceptance for the cross-link from the pillar side.

## P4-gated ship-loop

When `SMATCHET_AGENT_VCS=p4`, the orchestrator follows a **P4-gated ship-loop** instead of the default git ship-loop. The variant exists so the user reviews a Perforce shelf in P4V before any `git push` or `gh pr create` happens — git remains the ship-line, but git is touched **once**, at the end, after the change is known-good. The pause-loop in [`docs/agent-rules/delegation.md`](delegation.md) § Debug-mode pause-loop continues to **override** this loop for debug-detective triggers (same as it does for the default git loop).

**Fires when** `SMATCHET_AGENT_VCS=p4` is set at session start AND `p4 info` confirms the local server is reachable. On `p4 info` failure → `LOG_ERROR "p4-mode requested but Perforce not bootstrapped"` + `AskUserQuestion`: (a) fall back to default git ship-loop for this session, (b) abort, (c) follow [`docs/perforce/SETUP.md`](../perforce/SETUP.md) and retry. Never silently downgrade.

**Two sub-variants** — orchestrator asks once at task start via `AskUserQuestion` which to use:

1. **Small-change loop** (default; single slice, single subsystem) — work directly on `//smatchet/main` via the canonical client. Iterate edits in a pending CL. Smoke build → shelve → user review → full tests → submit → git branch + push + PR.
2. **Task-stream loop** (multi-slice OR write set spans multiple subsystems; only when user explicitly approves) — allocate `bash scripts/dev/p4-task-stream.sh <id>`. Each slice submits to the task stream's depot path. End-gate runs the full battery, then `bash scripts/dev/p4-task-stream-to-pr.sh <id> "<title>" --prepare-review-cl` integrates into a pending main-stream CL + shelves. User reviews shelf. On approval, `--promote-reviewed-cl <CL>` submits + creates git branch + push + PR.

**Key invariants (both sub-variants):**

- `git push` / `gh pr create` happen **once**, after shelf approval AND full test-pass.
- Shelf-review gate fires **exactly once** per task. Test failures post-approval → fix → re-test without re-review. Re-review only on explicit user request.
- **No `git worktree add`** while `SMATCHET_AGENT_VCS=p4` — subagent isolation uses `scripts/dev/p4-task-stream.sh` exclusively. First git write is the `git checkout -b` inside the promote step.
- **Smoke build precedes shelf** — user never sees a non-compiling change in P4V.
- **`code-review` agent dispatched ONCE per task** at the end-gate / shelf step (cumulative diff). Not per slice.
- Pure-docs slice skip still applies. Trivial-visual-only envelope still applies, with `p4 sync` + `p4 edit -t +l` substituting for `git stash` race-recovery.
- Plan-lock backend auto-flips to `p4-counter` **only when unset** — `export SMATCHET_LOCK_BACKEND="${SMATCHET_LOCK_BACKEND-p4-counter}"` (no colon — empty-string setting is preserved per `scripts/dev/test-p4-dual-vcs.sh` scenario 2 line 149 + scenario 6 line 369).
- Post-ship `AskUserQuestion` ALWAYS fires with option 3 ("Register with watcher") pre-selected; when `docs/design/merge-gates-ci-coderabbit-comments.md` ships end-to-end the `AskUserQuestion` goes away entirely in p4-mode.

Full phase sequence + invariants + exception rules in [`docs/perforce/AGENT_FLOWS.md`](../perforce/AGENT_FLOWS.md) § P4-gated ship-loop. Plan: [`docs/design/archive/p4-gated-ship-loop.md`](../design/archive/p4-gated-ship-loop.md). ADR: [`docs/adr/0008-p4-gated-ship-loop.md`](../adr/0008-p4-gated-ship-loop.md).

## Post-ship turn-end protocol

After the loop reaches PR-opened (or the equivalent terminal state for the task), end the turn with `AskUserQuestion` offering the four canonical next steps as discrete options:

1. **Manual verify** — user wants to drive the change manually before merge.
2. **Review PR** — user wants to read the diff / comment on GitHub.
3. **Register with watcher** — orchestrator runs `merge-watch register <pr>` (per [`docs/design/archive/smatchet-merge-watcher.md`](../design/archive/smatchet-merge-watcher.md)). The `smatchet-merge-watcher` host daemon's first step on register is `gh pr ready <n>` (idempotent — no-op if already non-draft) so CodeRabbit's `auto_review.drafts: false` doesn't skip the review (per `docs/backlog/agent-self-improvement/process.md` P1 — draft PRs silently bypassed CR for 15+ session PRs before this rule landed). Then it runs the gate-check loop + CodeRabbit-triage loop + REST-squash-merge per the watcher contract. Session can close immediately; watcher persists. Halt prompts surface as Smatchet notifications via `SmatchetToastManager` (watcher Phase 4), not back to this session.
4. **Done** — no further action; PR stays draft for later.

Do **not** emit a free-form bulleted next-steps list — `AskUserQuestion` is a single click; prose is N seconds of composition.

**Skip-condition**: if the user has already said "no more changes coming" / "ship it and stop" / "merge when green" in the same turn, skip the question and enter option 3 directly (`git-janitor` invokes `merge-watch register` before walking away).

Cross-link: ship-loop reference in [`docs/agent-rules/delegation.md`](delegation.md); pause-loop override in [`docs/agent-rules/delegation.md`](delegation.md) § Debug-mode pause-loop; gate semantics + halt prompts in [`docs/agent-rules/merge-gates.md`](merge-gates.md); watcher integration in [`docs/design/archive/smatchet-merge-watcher.md`](../design/archive/smatchet-merge-watcher.md).
