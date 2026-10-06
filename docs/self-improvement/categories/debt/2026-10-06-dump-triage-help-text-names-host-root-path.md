- 2026-10-06 · agent-surface flip review · [debt] · P3 — two command help strings still name `agents/scripts/core/dump-triage.sh` at the host root

Details:
After the flip the dump triage script lives in the `agent-layer/` mount, so it is at
`agent-layer/agents/scripts/core/dump-triage.sh` from the host root. Two help strings in
`Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp` (`debug.thread_dump`'s note and
`debug.dump_self`'s description) still give the old path. `docs/guides/cli.md` already names the
new one. The flip PR left them alone because any `Source/Core/` change trips the test-delta gate, and a
help string has no unit to test.

Concrete next action:
- Change both strings to `agent-layer/agents/scripts/core/dump-triage.sh` in a PR labelled
  `tests-out-of-band` (help text only, no runtime surface).

Status: open
Last-reviewed: 2026-10-06
