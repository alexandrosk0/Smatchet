# Piping a gate script into `tail` reports the pipe's exit code, not the gate's

- **Category**: process
- **Priority**: P2
- **Date**: 2026-08-18
- **Found during**: verifying the #2110 fix — reading `test-all.sh` and `check-pr-intent.sh`
  results while shipping [Issue #2110](https://github.com/alexandrosk0/Smatchet/issues/2110)

## What happened

Twice in one session the orchestrator ran a verification command as
`bash <gate>.sh 2>&1 | tail -N` and read the reported status as the gate's result.

1. `bash scripts/dev/test-all.sh 2>&1 | tail -45` reported **exit 0** on a run whose own
   `AGGREGATE` line said `Failed: 36` and which printed `Missing binary — rebuild and retry`
   (the suite's documented `exit 2`). The 0 was `tail`'s.
2. `bash agents/scripts/core/check-pr-intent.sh <body> 2>&1 | tail -30` printed
   `INTENT_EXIT=0` while the checker was reporting a MISSING review verdict.

In case 1 the masked status was one step away from being recorded as a passing
§ Verification row in a plan doc, on a run that had actually failed. `tail -N` also
**discarded the failure identities**, so `Failed: 36` was unattributable and the whole
~1 h suite had to be re-run with full capture.

## Why it is not obvious

- `set -euo pipefail` is a property of the *script being run*, not of the orchestrator's
  own interactive invocation, so nothing upgrades the masked status.
- The failure is silent and looks exactly like success — there is no error text to notice.
- The habit is reinforced by every *informational* `| tail` that is genuinely harmless.

## Fix shape

For any command whose **exit code is the verdict** (gates, test suites, lint runners,
`check-pr-intent.sh`, `merge-gates.sh`):

- Redirect to a file and echo the status explicitly, rather than piping:
  `{ bash <gate>.sh > out.log 2>&1; echo "EXIT=$?" >> out.log; }` — then read `out.log`.
- If a pipe is unavoidable, `set -o pipefail` in the *invocation*, or read
  `${PIPESTATUS[0]}` instead of `$?`.
- Never truncate a failing run's output with `head`/`tail` before the failure identities
  have been extracted — capture in full, then grep.

## Candidate gate

`agents/scripts/core/` verification wrappers could refuse to be piped when their exit code
is the product — e.g. a `[ -t 1 ] || …` guard is too blunt, but a shared
`run-gate.sh <script> <logfile>` helper that owns the redirect + `EXIT=` stamp would give
the orchestrator one correct invocation shape to reach for, and make the masked-status
form the unusual one.
