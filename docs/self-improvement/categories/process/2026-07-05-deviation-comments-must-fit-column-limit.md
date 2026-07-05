# Deviation comments must fit ColumnLimit or pre-ship loops forever

- **Date**: 2026-07-05 · **Priority**: P2 · **Category**: process
- **Session**: user-facing-text session (PRs #1614/#1615)

## Friction

`scripts/dev/pre-ship.sh` whole-file-formats every changed C++ file before the
delta lint gate. Several pre-existing single-line
`SMATCHET_DEVIATION(rule=duplication; …)` comments in
`JiraIssueMutation.cpp` / `JiraIssueSearch.cpp` were ~240 chars — over the
120-col `ColumnLimit` — so clang-format re-wrapped them into multi-line
comments whose continuation lines trip `comment-commented-out-code`. Any PR
touching those files hit a fix → format → re-fail loop (three iterations this
session) until the comments were compacted to ≤ 120 cols including indent.

## Proposal

Add a check (or extend `agent_size_audit.py`/the deviation-grammar validator)
that a `SMATCHET_DEVIATION` comment line fits ColumnLimit at its indent, so the
unstable form can't be committed. Alternatively teach the comment-noise rule to
ignore continuation lines that belong to a wrapped `SMATCHET_DEVIATION` block.
This session fixed the five instances in the two Jira TUs (compact
`reason=pre-existing clone`), but other over-long deviation lines likely
remain elsewhere and will bite the next PR that touches their file.
