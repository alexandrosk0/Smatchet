# Plan — Tier-3 Commands invocation harness: table-driven builtin-command dispatch tests

> **Slug**: `commands-invocation-harness`
>
> **Status**: `active`

## Context

[`TEST_COVERAGE_GAP_MAP.md`](../../../TEST_COVERAGE_GAP_MAP.md) Tier 3: `Commands/` is a strict lint
zone and the single registry feeding four frontends (CLI, palette, MCP, Lua), yet **no
`Builtin/BuiltinCommands_*.cpp` handler TU is compiled into any test target** (24 TUs, ~4.6K LOC).
The registry plumbing (`CommandRegistry`, `Command`, `FuzzyMatch`, `PaneCommands_detail`) is tested;
the actual command tables — names, aliases, ParamSpecs, destructive flags, and the dispatch glue —
are regression-invisible. The map's prescription: one table-driven harness that registers all
builtins against the fixture backend and invokes each command with (a) valid args, (b)
missing/extra args, (c) wrong-typed args, covering the 4.6K LOC in one PR and de-risking backlog
item **N6** (splitting `BuiltinCommands.cpp`) plus the `ui-request-flag-off-thread` race class the
lint gate exists for.

## Why these TUs were never testable (and what changed)

Every `Register*Commands(CommandRegistry&, AppController&)` hook takes the `AppController`
god-object, and the handler lambdas call its methods — so linking any builtin TU into the doctest
rig drags the full AppController family closure, which `SmatchetTests` deliberately avoids
(Tier 4: "don't try to unit-test the monolith"). Two facts change the calculus:

1. `CommandRegistry::Dispatch` centralizes the **entire** pre-handler pipeline — unknown-command
   fuzzy suggestions, required/coercion/enum/bounds validation (`ValidateAndResolveArgs`), and the
   destructive-confirm gate — so the (b)/(c) matrices and the destructive-guard behaviour never
   execute a handler body and never dereference the app object.
2. The repo already compiles the **whole core** into a static archive on Linux
   (`SmatchetCore_PosixCheck`, `CORE_SOURCES` minus the GL-host TU), and `AppController`'s
   constructor is cheap (an `Impl` holding a back-reference + one `GridLiveContext`; no GL, no
   I/O, no config load). A feasibility probe (this plan's gating step) links a doctest-style
   binary against that archive, constructs a real `AppController`, registers all builtins, and
   drives `Dispatch` through the validation layer.

## Approach

Slice 1 targets the dispatch-layer surface the gap map cares most about, WITHOUT invoking
app-mutating handler bodies:

- **Registration inventory pins**: every expected category registers; no duplicate names (the
  registry throws on duplicates — registering all builtins at once is itself the test); alias
  resolution; the parameter tables of security-relevant commands (types, required flags, bounds,
  enums); `Destructive` flags on the mutation/danger commands (the destructive-confirm gate is
  only as good as the flags).
- **Table-driven (b)/(c) matrix**: for EVERY registered command with required params, dispatch
  `{}` and assert `MissingRequiredArg` naming the param; for every typed param, dispatch a
  wrong-typed value and assert `ValidationError` — all machine-enumerated from `reg.All()`, so new
  commands are covered automatically the day they are added.
- **Destructive-guard matrix**: every `Destructive` command dispatched from an automation source
  without confirmation must return `ConfirmRequired` (never reach the handler); `DryRun` bypasses.
- **Selected (a) valid-args cases**: read-only commands whose handlers touch only the cheap
  constructed state (e.g. `meta.*` help/list surfaces) execute for real; app-mutating handlers are
  out of scope for Slice 1 (they need the fixture backend wired through `GridLiveContext`, a later
  slice if wanted).

## Feasibility probe result (gating step — PASSED)

A scratch binary linked `libSmatchetCore_PosixCheck.a` + the `SmatchetTextEdit` object (the only
missing symbol cluster — 25 `TextEditor::*` references) + ImGuiLib/md4c/cpr/curl/SQLiteCpp/system
OpenSSL, constructed `AppController` headless, registered **88 builtins** with zero duplicate-name
throws, and drove `Dispatch` through unknown-command + 28 missing-required-arg failures without a
crash. The harness below is the productionised version: 6 cases / 952 assertions green locally.

## Files to modify

### Slice 1 (this PR)

1. `tests/CMakeLists.txt` — new `SmatchetCommandsTests` doctest target, guarded
   `UNIX AND SMATCHET_BUILD_POSIX_CORE_CHECK`, linking the existing full-core archive +
   `SmatchetTextEdit` + the app's third-party set — NOT added to `SmatchetTests` (which compiles
   many core TUs directly; linking the archive there would duplicate symbols). Registered in ctest
   as `smatchet_commands_tests`.
2. `tests/Commands/BuiltinCommandsDispatch.test.cpp` — the harness described above. Reality pins
   discovered while landing it: one legacy dotless command exists (`notifications`, category
   `view` — now pinned so a second dotless name can't slip in), and the real category set is 19
   strong (`app attach automation bug commands config debug fields grid offline perf scenario sync
   ticket tickets ui ui_test users view` — all pinned as the floor).
3. `.github/workflows/build-and-test.yml` — the `mobile-posix-core-check` job configures with
   `-DSMATCHET_BUILD_TESTS=ON` and gains one step building + running `SmatchetCommandsTests`
   (advisory lane; the archive it links is built by the existing compile step).
4. This plan doc — implementation log + deviations.

## Verification

- Local (this container): build + run the new target under the `posix-core-check` configure
  (`SMATCHET_BUILD_TESTS=ON`); repo lint/docs/format gates.
- CI: the Windows lanes build whatever target shape the slice lands; the harness must be wired so
  at least one blocking CI lane runs it (exact lane TBD in the slice — if only the advisory
  posix-core-check lane can host it on day one, that is recorded as a deviation and a follow-up).

## Implementation log

- `fc86b5f9` · Slice 1: `SmatchetCommandsTests` target + `BuiltinCommandsDispatch.test.cpp`
  (6 cases / 952 assertions green locally under the `posix-core-check` configure) + the
  advisory-lane CI wiring. `SmatchetTests` rebuilt and unaffected; lint 11/11, docs 14/14,
  clang-format clean.

## Deviations from plan

- Plan-lock seed push to `refs/locks/*` remains blocked by the session git proxy (403) — logged per
  the seed contract, same as the Tier-1/Tier-2 plans.
- Day-1 CI hosting is the advisory `mobile-posix-core-check` lane only (as the plan anticipated) —
  promotion to a blocking lane is the recorded follow-up once the harness proves stable in CI.
