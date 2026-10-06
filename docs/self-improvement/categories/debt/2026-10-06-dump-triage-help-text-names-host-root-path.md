- 2026-10-06 · agent-surface flip review · [debt] · P3 — two command help strings still name `agents/scripts/core/dump-triage.sh` at the host root

Details:
After the flip the dump triage script lives in the `agent-layer/` mount, so it is at
`agent-layer/agents/scripts/core/dump-triage.sh` from the host root. Two help strings in
`Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp` (`debug.thread_dump`'s note and
`debug.dump_self`'s description) still give the old path, and so does a comment in
`Source/Core/include/Diagnostics/SelfDump.h`. `docs/guides/cli.md` already names the new one. The flip
PR kept product C++ out of its diff, because any `Source/Core/` change trips the test-delta gate.

Concrete next action:
- Change both strings and the `SelfDump.h` comment to `agent-layer/agents/scripts/core/dump-triage.sh`.
  In `tests/Commands/BuiltinCommandsDispatch.test.cpp`, the `debug.dump_self` case already looks the
  command up; add a check that its `Description` names the new path. That test satisfies the test-delta
  gate, so no override label is needed.

Status: open
Last-reviewed: 2026-10-06
