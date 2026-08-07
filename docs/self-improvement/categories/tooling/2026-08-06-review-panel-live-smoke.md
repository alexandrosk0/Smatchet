- 2026-08-06 · orchestrator · [tooling] · P3 — `review-panel-live-smoke`: `run-review.sh` ships with its headless harness invocations verified only against stub CLIs — no gate ever launches a real `claude` / `codex` / `cursor-agent` panel leg, so a vendor CLI flag change lands silently
  Details: Phase 2 of `docs/plans/absorb-whip-process.md` ports the Whip review-panel launcher as
    `agents/scripts/core/run-review.sh`. Its regression suite (`tests/bats/run_review.bats`, 16
    cases) covers selection, outfile naming, roster parsing, stagger, watcher/LANDED accounting,
    and guard integration — but every harness binary is a stub on `PATH`. The three real invocation
    shapes are therefore contract-encoded but unverified end-to-end: `claude -p` with the composed
    prompt; `codex exec -a never -s workspace-write`; `cursor-agent` given a prose ask citing the
    reviewer SKILL.md. If any vendor renames a flag (Codex's sandbox/approval flags have churned
    before), the launcher fails only at the next real panel run, not in CI — the exact class the
    stubbed suite structurally cannot catch. Same shape as Whip's own residue note; carried across
    with the port rather than silently dropped.
  Concrete next action: add an opt-in live smoke modelled on the credentialled-lane pattern
    (`2026-08-03-credentialled-setup-smoke.md`): a script (or bats case) that `SKIP`s unless
    `SMATCHET_LIVE_PANEL_SMOKE=1`, then runs `run-review.sh` against a throwaway one-file work item
    with a trivial prompt, one leg per harness actually installed, and asserts each selected leg
    LANDS a non-empty outfile. Never in CI (needs vendor logins + spends real tokens); value is a
    one-command reproducible check before relying on a fresh harness version. Est ~0.5 d.
  Cross-ref: `agents/scripts/core/run-review.sh`; `tests/bats/run_review.bats`;
    `docs/plans/absorb-whip-process.md` § Phase 2; `project.config.json` § review-panel.
  Status: open
  Last-reviewed: 2026-08-06
