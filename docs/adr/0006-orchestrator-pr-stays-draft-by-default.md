# Orchestrator opens PRs draft by default; rely on merge-gates poller for CodeRabbit signal

# Status

Accepted (2026-05-20)

# Context

PR #318 squash-merged before CodeRabbit (CR) reviewed it. Root cause was a race in the merge-gates contract:

1. Orchestrator opened the PR with `gh pr create --draft`.
2. `.coderabbit.yaml` carries `auto_review.drafts: false` — CR silently skipped the draft PR.
3. The merge-gates poller saw `CodeRabbit: NONE` and treated it as pass-through (legacy behaviour for repos that never integrated CR).
4. First poll fired ~10 seconds after `gh pr create` returned and the gate read NONE before CR had even queued the review. Gate green; squash-merge fired.

Two fixes landed back-to-back to address this:

- **PR #319** (`84f097da`) — patched AGENTS.md § Autonomous ship-loop so the orchestrator opens `gh pr create` **without** `--draft` when the user has authorised auto-merge in the same turn. The intent was to make CR fire so the gates poller would see a real review signal.
- **PR #320** (`8d27ca06`, +13 min) — patched `scripts/dev/merge-gates.sh` so the poller probes for `.coderabbit.yaml` at gate start and, when CR is installed, blocks `cr_state == NONE` until (a) CR posts a review, (b) the head-commit rollup contains a `CodeRabbit` `StatusContext` with `state == "SUCCESS"`, or (c) the configurable grace window (`MERGE_GATES_CR_GRACE_POLLS`, default 10 polls) expires.

PR #320 fixed the root cause in the correct layer (the poller). PR #319's draft-flip became redundant — and, on inspection, harmful:

- **Wrong layer.** Gate enforcement is the poller's job; the draft flag is a reviewer-UX signal, not a gate primitive.
- **Loses draft-as-WIP semantics.** A PR opened ready is industry signal for "review me now". The orchestrator's ship-loop opens the PR at the end of a one-turn diagnose → fix → build → commit → push sequence; the user has not yet had a chance to skim the diff. "Ready" overstates the readiness of an autonomous-loop output.
- **Couples ship-loop to CR config.** `auto_review.drafts: false` is a per-repo CodeRabbit setting. Encoding "open ready so CR fires" in the orchestrator contract makes the ship-loop break if a future repo flips that setting back to `true` (where draft PRs are reviewed) or drops CR entirely.
- **Drift risk.** Two mechanisms enforcing the same invariant — "don't merge before CR sees the diff" — drift over time. The poller is the authoritative source of truth; the contract should defer to it.

# Decision

Revert PR #319. The orchestrator's contract reverts to the spawned-child default: **`gh pr create` always opens `--draft`**, regardless of whether the user authorised auto-merge in the same turn. The merge-gates poller (#320) is the sole enforcement point for "CR must see the diff before merge".

Specifically:

- In AGENTS.md, the **Autonomous ship-loop default** section drops the "Draft-vs-ready at `open PR`" paragraph added by PR #319.
- The orchestrator (user's main session) opens `--draft` identically to spawned-child agents (`handoff-implementer`, `pr-iterator`). Auto-`gh pr ready` + REST squash-merge still happens at the end of the merge-gates pass for orchestrator PRs where the user authorised auto-merge — that scope boundary (§ Handoff envelope § Spawned-child PR draft requirement) is unchanged.
- The merge-gates poller (`scripts/dev/merge-gates.sh`) remains the authoritative gate. Its CR-installed detection + grace-window behaviour from #320 is the load-bearing piece.

# Consequences

- **One enforcement layer.** "CR must review before merge" is enforced exclusively by the poller. No contract-level workaround drifts out of sync with the poller's behaviour.
- **Draft-as-WIP signal preserved.** Reviewers see "ready for review" only after the orchestrator's auto-`gh pr ready` fires at the end of a passing merge-gates poll — i.e., when CR has actually reviewed and any user comments are resolved.
- **Slower visible flip to ready.** Under #319, the PR opened ready immediately; reviewers saw a green PR ~seconds after `gh pr create`. Under this ADR, the PR opens draft and only flips ready after the merge-gates poller passes (typically minutes, depending on CR latency and CI duration). The trade-off is intentional — the draft state is honest signal that the loop hasn't yet validated the change end-to-end.
- **`auto_review.drafts: false` is no longer load-bearing.** If a future repo flips that setting, behaviour is unchanged — the poller still blocks `NONE` for CR-installed repos via the grace window.
- **Repos without `.coderabbit.yaml` still pass `CodeRabbit: NONE`.** Legacy behaviour preserved; the poller's `MERGE_GATES_CR_INSTALLED` override remains available for forks / out-of-tree configs.

# Alternatives considered

- **Keep #319 as belt-and-suspenders.** Rejected — two mechanisms enforcing one invariant drift; the cost of drift exceeds the benefit of an earlier CR fire.
- **Open ready only when CR is not installed.** Rejected — would special-case the orchestrator on a per-repo basis and re-introduce the `auto_review.drafts` coupling for CR-installed repos.
- **Configurable per-repo "open ready" override.** Rejected as premature; no repo currently needs it, and the merge-gates poller's grace window already accommodates the slow-CR case uniformly.
