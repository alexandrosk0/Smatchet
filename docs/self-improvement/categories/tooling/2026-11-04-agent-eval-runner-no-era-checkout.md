# The subagent-eval runner never checks out the case's `repoRef`, so a prompt that delegates its criteria to repo files is measured without them

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-11-04
- **Observed on**: the code-review eval of the pre-#18 prompt against the thinned one (the-unwilling-agentic-bunch#18 → Smatchet#2385).
- **Status**: open

## What happened

`scripts/dev/agent-eval-run.sh` runs the harness exe in the caller's working directory. It exports `SMATCHET_AGENT_PROMPT_ROOT` for the prompt, but it never checks out the case's `repoRef`, and it provides neither the host docs nor the `agent-layer/` of the side under test. The thinned code-review prompt moved its review criteria into "Read first" files (the adversarial-code-review skill and the checklist named by `docs.review_checklist`). In a plain historical checkout those files are missing or from another era, so the eval measures a different prompt than production runs. The checkout era also has to match the side: the pre-#18 prompt with today's docs, or the reverse, mixes eras.

The eval worked around this with a scratch overlay script. For each (side, case) it created a detached worktree at `repoRef`, overlaid that side's `AGENTS.md`, `CONTEXT-MAP.md`, `project.config.json`, `docs/guides/**` and leaf `AGENTS.md` files, extracted the side's layer pin at `agent-layer/`, and hid the overlay from `git status` (assume-unchanged plus a per-checkout excludes file). Runs 1 and 2, before the overlay, gave results that the production-faithful runs 3 and 4 did not reproduce.

## Concrete next action

Add `--checkout=<era-host-commit>` to `agent-eval-run.sh`. It creates a detached worktree at the case's `repoRef`, overlays the docs listed above from the era commit, extracts that commit's `agent-layer` gitlink at `agent-layer/`, hides the overlay from git, runs the trials with that worktree as the exe's working directory, and removes the worktree on exit. Record the era commit and layer pin in `runMeta`. Make it the default when the case has a `repoRef`, with the `--prompt-root` worktree's HEAD as the era commit, and document in `agent-layer/docs/agent-rules/subagent-eval.md` (the two-worktree recipe) that base and head each need their own era.
