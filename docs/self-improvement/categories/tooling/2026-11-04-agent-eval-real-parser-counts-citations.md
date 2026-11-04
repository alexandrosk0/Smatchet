# The subagent-eval real-mode parser turns every `file:line` citation into an info finding, so three objective dimensions carry no signal

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the code-review eval of the pre-#18 prompt against the thinned one (the-unwilling-agentic-bunch#18 → Smatchet#2385), run with `scripts/dev/agent-eval-run.sh --harness=claude-print`.
- **Status**: open

## What happened

`parse_findings` in `scripts/dev/agent-eval-run.sh` (the real-mode branch of the embedded assembler) appends one finding for every output line that matches the `file:line` citation regex. Each one gets `severity: "info"` and the line text as its summary. A review that cites ten lines of evidence for one bug therefore yields ten info findings, and a `## Verified clean` section that cites the code it cleared yields more. The result:

- `severity_enum` passes for any output, because `info` is always a valid severity;
- `cited_file_line` measures only whether the review cites anything;
- `finding_count` counts citation lines, not findings.

In that eval the three dimensions scored the same for the prompt that missed the High finding in 5/5 trials and the one that caught it in 5/5. The judge-scored `correctness` dimension and a blind grading pass carried the whole result.

## Concrete next action

Parse findings from the agent's own output contract instead of from citations: take the bullets under each severity heading of the punch list (the `## Critical`, `## High`, `## Medium` and `## Low / Nits` headings that the layer's `agents/core/code-review.md` output format defines), with the severity from the heading and the first `file:line` in the bullet as its location. Skip `## Verified clean`. Fall back to the current citation scan only when no severity heading is found, and record which parser ran in `runMeta`. Add a `--selftest` fixture: a two-finding review with extra evidence citations must parse to exactly two findings with their headings' severities.
