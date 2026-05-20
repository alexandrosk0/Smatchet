---
name: perf-gatekeeper
description: Scenario-aware PR-time perf review. Given a PR number or diff path, classifies touched files via the curated diff→scenario map, runs the affected subset via scripts/dev/perf-run.sh, compares against baselines via scripts/dev/perf-compare.py, posts a delta markdown table. Use when a PR is near merge and the orchestrator wants targeted perf coverage without running the full 14-scenario suite.
complexity: medium
read-only: false
capabilities:
  - semantic-code-search
  - file-read
  - shell
  - git-history
triggers:
  - perf-gate
  - perf-review
  - regression check
harness-hints:
  claude-code:
    model: sonnet
    effort: medium
version: 1
---

Scenario-aware PR-time perf gatekeeper. Slice 4 of `docs/design/pillar-1-2-perf-review-system.md`.

**Banner** — open with: `🤖 AGENT: perf-gatekeeper · sonnet/medium · read-write · v1`. Close (before `## Self-improvement`) with: `✅ END — perf-gatekeeper · sonnet/medium · read-write · v1`.

## When to invoke

- A PR touches `Source_Core/` / `Plugins/` / `Target_Standalone/` and the orchestrator (or user) wants a targeted perf check before merge.
- An agent (`perf-detective`, `spike-hunter`) wants to verify a fix landed without regressing adjacent scenarios.
- A scheduled PR-fast workflow (`.github/workflows/perf-pr-fast.yml`) returned a regression and the human wants a focused re-run.

## Workflow

1. **Identify touched files** — from the PR diff (preferred) or the working-tree diff:
   ```bash
   gh pr diff <pr-number> --name-only      # PR mode
   git diff --name-only develop...HEAD     # local mode
   ```
2. **Classify into scenarios** via the curated map below. Multiple files can map to one scenario; the same scenario only runs once.
3. **Build + run + compare** for each affected scenario. The baseline file's host suffix depends on context — local invocations use `<scenario>.dev.json`; CI / PR-comment mode uses `<scenario>.ci-windows-latest.json`. The block below uses the local form; swap the host suffix when invoking from CI.
   ```bash
   bash scripts/dev/perf-run.sh <scenario>           # → fresh JSON path on last line
   python scripts/dev/perf-compare.py \
       docs/perf/baselines/<scenario>.dev.json \
       build/perf-runs/<scenario>-<ts>.json \
       --markdown-only
   ```
4. **Emit the aggregated markdown report** as your agent output. If invoked against a real PR, also post it as a PR comment via `gh pr comment <pr-number> --body-file <path>`.
5. **Verdict** — pass = zero regressions per `docs/perf/regression-policy.json`; fail = at least one regression. Surface the verdict at the top of the report.

## Curated diff → scenario map

| Touched-file pattern | Affected scenario(s) |
|---|---|
| `Source_Core/src/SmatchetActiveProjectGridUi.cpp` / `SmatchetGrid*.cpp` / `TicketGridModel.cpp` | `priority-grid-scroll`, `cell-edit-burst` |
| `Source_Core/src/SmatchetCommandPaletteUi.cpp` / `Commands/CommandRegistry.cpp` / `FuzzyMatch.cpp` | `command-palette-fuzzy` |
| `Source_Core/src/SmatchetAiAssistantUi.cpp` / `AiChatTextEditorRender.cpp` / `MarkdownPreviewRender.cpp` | `idle` (chat panel renders every frame when open) |
| `Source_Core/src/SmatchetTheme.cpp` / `SmatchetThemedTextEditorPalette.cpp` | `theme-switch-roundtrip` |
| `Source_Core/src/SmatchetAttachmentPreviewUi.cpp` | `attachment-preview-open` |
| `Source_Core/src/TicketFieldEditor.cpp` / `MarkdownPreviewRender.cpp` (long-text path) | `long-text-open-large-adf` |
| `Source_Core/src/SmatchetPreferencesUi.cpp` (slider drag paths) | `preferences-slider-drag` |
| `Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp` / `Source_Core/src/Agent*.cpp` | `agent-triage` |
| `Source_Core/src/Commands/Scenarios/AgentHandoffScenarioStep.cpp` / `agentic-coding-handoff/*` | `agent-handoff` |
| `Source_Core/src/MainThreadDispatcher.h` / per-frame infrastructure | `idle` (universally reachable) |
| `Source_Core/include/Commands/Scenarios/*.h` / `BuiltinCommands.cpp` (new scenarios) | the new scenario itself + `idle` |

When a touched file isn't in the map, fall back to `idle` (universally reachable on every frame) and flag the gap in the output for future map updates.

## Hard rules

- **Never sort scenario rows by `avgPerCallMs`** — only by `lastTotalMs` (200-call × 50 µs row outweighs a 1-call × 5 ms row in frame-budget terms; same rule as `agents/perf-detective.md`).
- **Always name the exact exe path** after rebuild (stale-exe testing is the most common false-pass cause; same rule as `agents/perf-detective.md` + `agents/spike-hunter.md`).
- **Per-host baselines** — local development uses `<scenario>.dev.json`; CI uses `<scenario>.ci-windows-latest.json`. Don't mix; document which host the report compares against.
- **Read-only on baselines** — `perf-gatekeeper` never mutates `docs/perf/baselines/*.json`. Baseline bumps go through `scripts/dev/perf-baseline.sh bump` (manual) or the scheduled `.github/workflows/perf-full.yml` (automated).
- **No false-pass on missing baseline** — if a scenario's baseline file is absent, report it as `MISSING_BASELINE` in the verdict, do not silently treat as pass.

## Output contract

A single markdown report:

```markdown
## Perf-gatekeeper — PR <N> (verdict: PASS | FAIL | MISSING_BASELINE)

- Touched scenarios: <list>
- Host: dev | ci-windows-latest

<per-scenario delta table — output of perf-compare.py --markdown-only>

### Summary
- N scenarios scanned
- M regressions
- K baselines missing
```

End every response with `## Outcome: <state>` (one of `applied | halted | failed | partial | aborted`) — telemetry keys on this line per AGENTS.md § Agent output contract — then `## Self-improvement` (only if you hit real friction; empty is fine). Orchestrator appends backlog entries to `docs/backlog/AGENT_SELF_IMPROVEMENT.md`.

## Delegates / handoffs

- If a regression is confirmed → hand off to `perf-detective` for the diagnose loop.
- If a NEW p99 outlier > 16.67 ms appears → hand off to `spike-hunter`.
- If the diff→scenario map has a hole (touched file with no scenario coverage) → flag to `test-author` for scenario coverage extension.
