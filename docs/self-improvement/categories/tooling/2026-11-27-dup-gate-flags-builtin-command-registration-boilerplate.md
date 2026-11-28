# The duplication gate flags built-in command registration boilerplate after any retype in it

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-27
- **Observed on**: PR #2523 (merged 2026-11-27), `Source/Core/src/Commands/Builtin/BuiltinCommands_Fields.cpp`.
- **Status**: open

## What happened

Retyping one local (`const TrackerField*` to `std::shared_ptr<const TrackerField>`) in the `fields.get` and `fields.icon_for` handlers re-hashed their windows, and `dup_audit.py --diff origin/develop` reported new clones against `BuiltinCommands_Attach.cpp`. The shared lines are the `MakeCommand` registration shape every command bucket writes by construction: read args, one lookup, fill `Params`, `Register`. A shared helper would couple independent command buckets, so both blocks got a `SMATCHET_DEVIATION(rule=duplication)` marker.

The marker placement was also a trap: a marker above the block, with a `// clang-format on` line between it and the clone start, did not suppress the finding. It only worked once the off / marker / on triple sat inside the block, after the opening `{`.

## Proposed fix

- In `dup_audit.py`, skip comment-only lines (including `clang-format off/on`) when looking for the nearest line above a clone, so a marker wrapped for clang-format still counts.
- Consider a per-path normaliser that masks the `MakeCommand(...)` registration skeleton in `Commands/Builtin/*.cpp`, or a shared registration builder, so a type change inside a handler does not surface the boilerplate as a new clone.

**Enumerator + replay**: `python agent-layer/agents/scripts/core/dup_audit.py --diff origin/develop` on PR #2523's head without the markers.
