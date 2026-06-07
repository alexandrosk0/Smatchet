# Orchestrator policy update (user-mandated, 2026-06-07)

**Before your commit**: after staging your final diff and BEFORE `git commit`, request a code review by writing your full `git diff --staged` summary into your report and PAUSING the commit if you find on self-review any of: raw pointer captured across async, missing key/parameter threading, lint-zone violations, behaviour change in a "behaviour-identical" step. The orchestrator additionally runs the `code-review` agent (now opus/high) on your PR diff before merge — expect a findings round; leave your worktree intact after pushing.
