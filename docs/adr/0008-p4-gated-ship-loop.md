# P4-gated ship-loop + force-push carve-out extension

# Status

**Accepted (2026-05-23).** Two decisions recorded in one ADR because they are coupled — the p4-task-stream-to-pr branch shape introduced by decision (a) is what motivates the force-push exclusion-list rewrite in decision (b).

# Context

Smatchet ships via git/GitHub as the canonical ship-line (CI, CodeRabbit, `smatchet-merge-watcher`). Perforce is an opt-in local layer (`SMATCHET_AGENT_VCS=p4`) per [`AGENTS.md`](../../AGENTS.md) § Dual-VCS topology, providing agentic-WIP primitives that git lacks — task streams, atomic plan-locks via counters, exclusive file locks, server-side shelves.

Two gaps surfaced once `SMATCHET_AGENT_VCS=p4` had real users:

**Gap 1 — review timing**. The default autonomous ship-loop (per [`AGENTS.md`](../../AGENTS.md) § Autonomous ship-loop default) runs `diagnose → fix → build → commit → push → open PR` end-to-end without pausing. The user only sees the change after the PR is open on GitHub — by which point CI has started, CodeRabbit has potentially started reviewing, and any post-hoc rework requires a force-push or amend cycle. In p4-mode, this leaves the powerful p4-shelf primitive (server-side, in-P4V-reviewable, modify-and-re-shelve-friendly) entirely unused. The user wants their human-review gate to be the p4 shelf, BEFORE any git push or PR creation.

**Gap 2 — force-push exclusion list shape**. The current carve-out (per [`AGENTS.md`](../../AGENTS.md) § Project rules § Force-push carve-out, previously documented by withdrawn-as-historical ADR 0005) permits `git push --force-with-lease origin claude/<id>` only during API-500 recovery on a Claude-Code-SDK-spawned worktree branch. The exclusion list reads `develop`, `main`, `chore/*`, `feat/*`, `fix/*`, `docs/*`, `wip/*`. The ambiguity: do those `*/*` patterns match the FIRST path segment only, or any nested segment? Under literal-prefix-match, a branch like `agent/perf-detective-01/feat-improve-grid-scroll` would be EXCLUDED because the slug after the agent-id starts with `feat-`. That false-exclusion blocks the p4-task-stream recovery shape introduced by decision (a) below — `p4-task-stream-to-pr.sh` produces branches under `agent/<task-stream-id>/<slug>` and slugs frequently begin with `feat-` / `fix-` / `docs-`.

The p4-task-stream branch invariants are the same as the Claude-SDK spawned branches:

1. **Single-owner** — only the orchestrator-side `p4-task-stream-to-pr.sh` invocation creates and pushes to these branches; no parallel agent or human has commit authority during the run.
2. **Pre-PR** — at carve-out time the branch is pre-PR-creation or carries a draft PR with no CI / CodeRabbit approval to invalidate.
3. **Gitignored worktree** — not in the user's interactive iteration path.

# Decision

## (a) Ship-flow semantic change — P4-gated ship-loop

Add a P4-gated ship-loop variant documented at [`AGENTS.md`](../../AGENTS.md) § P4-gated ship-loop and [`docs/perforce/AGENT_FLOWS.md`](../perforce/AGENT_FLOWS.md) § P4-gated ship-loop. Plan: [`docs/design/p4-gated-ship-loop.md`](../design/p4-gated-ship-loop.md).

Fires when `SMATCHET_AGENT_VCS=p4` at session start. Two sub-variants — the orchestrator asks the user once via `AskUserQuestion` which applies:

1. **Small-change loop**: work directly on `//smatchet/main` via canonical client. Iterate in a pending CL. Smoke build → `p4 shelve -c <CL>` → user reviews in P4V → full tests → `p4 submit` → git branch + push + PR.
2. **Multi-slice task-stream loop**: allocate `bash scripts/dev/p4-task-stream.sh <id>`. Each slice submits to the task stream's depot path. End-gate runs full test battery, then `p4-task-stream-to-pr.sh <id> "<title>" --prepare-review-cl` integrates into a pending main-stream CL + shelves. User reviews shelf. Approval → `--promote-reviewed-cl <CL>` submits + creates git branch + push + PR.

Two new modes on `scripts/dev/p4-task-stream-to-pr.sh` (`--prepare-review-cl`, `--promote-reviewed-cl <CL>`) preserve the existing one-shot mode for callers that don't need a review gate. Promote-mode validates the CL (exists, pending, current client, `task-stream-id: <agent-id>` tag) and refuses with exit 5 + manual cleanup recipe on mismatch; never auto-cleans stranded state.

## (b) Force-push carve-out extension — exclusion list rewritten as top-level-prefix-only

Rewrite the force-push carve-out exclusion list in [`AGENTS.md`](../../AGENTS.md) § Project rules § Force-push carve-out to be **top-level-prefix-only** — an exclusion triggers only at the first path segment:

> Exclusion list: `develop`, `main`, `chore/*`, `feat/*`, `fix/*`, `docs/*`, `wip/*`. Branches under `claude/<id>/*` and `agent/<task-stream-id>/*` are permitted regardless of nested slug prefix.

Add `agent/<task-stream-id>/*` as a second permitted carve-out namespace alongside `claude/<id>/*`. The carve-out's existing conditions apply unchanged: API-500 recovery only; orchestrator amending an unpushed-since-API-500 commit; ahead-range contains zero non-self commits; `--force-with-lease` (never bare `--force`).

This re-introduces the `agent/<id>` namespace deleted post-`ClaudeCodeLocalRunner` (per v1 of [`docs/design/github-tracker-backend.md`](../design/github-tracker-backend.md)), now serving the p4-task-stream surface instead.

# Consequences

## From (a) ship-flow change

- **User reviews every p4-mode change before it reaches GitHub.** No CI cycles burned on shelves the user would reject. No CodeRabbit reviews on shelved drafts that get re-shelved 5 times before the user approves. The p4 shelf becomes the human-review primitive that git's draft-PR mechanism was trying (poorly) to be.
- **Slower cadence for trivial fixes** — even a one-line typo fix in p4-mode goes through the shelf step. Mitigated by the small-change loop's lightweight shape: smoke build + shelve + confirm + tests + submit fits in ~30 seconds for trivial changes.
- **Pause-loop interaction** — debug-detective triggers (per [`docs/agent-rules/delegation.md`](../agent-rules/delegation.md) § Debug-mode pause-loop) suspend BOTH ship-loop variants. The pause-loop continues to take precedence; p4-mode does not suppress the diagnose-first-then-confirm cycle.
- **No git-worktree-add in p4-mode** — subagent isolation uses `scripts/dev/p4-task-stream.sh` exclusively. Documented in AGENTS.md § P4-gated ship-loop § Key invariants. Doc-level rule, not script-level enforcement; the follow-up backlog item to add a runtime gate is flagged in the plan's § Out of scope.
- **Stranded pending CLs persist across sessions** — if a session dies between `--prepare-review-cl` and `--promote-reviewed-cl`, the pending CL stays on the p4 server with its shelf attached. Resume via `--promote-reviewed-cl <CL>` is safe; the user is in the loop on cleanup decisions (the script prints the manual recipe and refuses to auto-clean).
- **AskUserQuestion fires post-PR with option 3 pre-selected** — only goes away once [`docs/design/merge-gates-ci-coderabbit-comments.md`](../design/merge-gates-ci-coderabbit-comments.md) ships end-to-end. Dependency tracked in the plan's § Dependencies (sequencing).

## From (b) carve-out extension

- **`p4-task-stream-to-pr.sh --promote-reviewed-cl` can recover from API-500 mid-promotion** — if the orchestrator-side amend-and-recover loop fires on an `agent/<task-stream-id>/<slug>` branch, the carve-out applies and the recovery commit folds into the original commit rather than producing a noisy 2-commit history.
- **Exclusion-list ambiguity resolved** — the literal "top-level-prefix-only" rule means future readers don't have to guess whether `agent/<id>/feat-foo` is excluded. The explicit allow-list (`claude/<id>/*` and `agent/<task-stream-id>/*`) makes the permitted namespaces enumerable.
- **Surface broadened from one namespace to two** — the carve-out now permits force-push on a second branch shape. The single-owner / pre-PR / gitignored-worktree invariants apply equally to both, so the safety claim from ADR 0005's § Consequences carries forward. ADR 0005's `agent/<id>` half was withdrawn-as-historical because the `ClaudeCodeLocalRunner` was deleted; the same branch shape is now reintroduced for a different (and currently live) source.

# Alternatives considered

**For (a):**

- **Keep the default autonomous ship-loop in p4-mode + lean on draft PRs for review.** Pro: zero new sub-loop to maintain. Con: GitHub draft PRs don't surface in P4V, force the user to context-switch to a web browser, and don't support the "modify and re-shelve" pattern that the p4 shelf supports trivially. The whole reason to opt into `SMATCHET_AGENT_VCS=p4` is the agentic-WIP primitives p4 has and git lacks — using a git-only review primitive in p4-mode wastes the opt-in.

- **Always task-stream, never small-change.** Pro: one loop shape regardless of slice count. Con: task-stream allocation has measurable overhead (~3 seconds for stream creation + client setup + populate + sync), and 80%+ of orchestrator tasks are single-slice / single-subsystem where the overhead has no payoff. The orchestrator-asks-once-up-front shape lets the user route trivial work to the cheap loop.

- **Single shelf gate at end-of-task (no per-slice gates).** Pro: one user-review touch point. Con: the multi-slice loop already only gates at end-of-task — the inter-slice gates run automated checks (build + ctest + lint + test-all), not user review. Conflating slice-boundary build gates with user-review gates was the prior design and led to confusion about when the user is asked vs when automation passes.

**For (b):**

- **Keep `agent/<id>` excluded; force the orchestrator to do 2-commit recovery on task-stream branches.** Pro: zero ADR change; ADR 0005's withdrawn-as-historical posture stays clean. Con: every p4-task-stream API-500 produces a "stage missed files" follow-up commit identical to the recovery cost we already absorbed once on `claude/<id>` and decided not to absorb (per ADR 0005 § Alternatives § Always-new-commit recovery).

- **Make the exclusion list match-any-segment instead of top-level-only.** Pro: stricter; reduces the carve-out surface. Con: forces `p4-task-stream-to-pr.sh` to reject any PR title that would produce a slug starting with one of the protected prefixes. PR titles like "feat: improve grid scroll" are common; renaming-to-avoid-conflict at script level is fragile.

- **Mint a fresh namespace (`p4-task/<id>/`) instead of reviving `agent/<id>/`.** Pro: no overlap with the historically-deleted shape. Con: `p4-task-stream-to-pr.sh` already produces `agent/<task-stream-id>/<slug>` per its established convention (per the existing script's branch slug derivation). Renaming would invalidate any draft PRs already in flight (none in the wild yet, but the script has shipped) AND would require a parallel name in docs + tests for no semantic gain.

# Cross-references

- Plan: [`docs/design/p4-gated-ship-loop.md`](../design/p4-gated-ship-loop.md).
- Rule body: [`AGENTS.md`](../../AGENTS.md) § P4-gated ship-loop + § Force-push carve-out for Claude Code SDK-spawned recovery and p4 task-stream promotion.
- Phase reference: [`docs/perforce/AGENT_FLOWS.md`](../perforce/AGENT_FLOWS.md) § P4-gated ship-loop.
- Withdrawn prior ADR (carries the original `claude/<id>` safety analysis): [ADR 0005](0005-force-push-carve-out-for-spawned-agent-recovery.md).
- Recovery procedure (force-push only applies during this loop): [`docs/agent-rules/delegation.md`](../agent-rules/delegation.md) § API-500 mid-run recovery.
- Script: [`scripts/dev/p4-task-stream-to-pr.sh`](../../scripts/dev/p4-task-stream-to-pr.sh) (added `--prepare-review-cl` + `--promote-reviewed-cl <CL>` modes).
- Tests: [`scripts/dev/test-p4-dual-vcs.sh`](../../scripts/dev/test-p4-dual-vcs.sh) scenarios 4 (prepare-mode), 5 (promote refusal), 6 (lock-backend if-unset pattern).
