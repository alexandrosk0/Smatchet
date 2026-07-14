# Plan — Command-input hardening (CLI · MCP · Palette · Lua)

> **Slug**: `command-input-hardening`
>
> **Status**: `shipped`
>
> **Owner**: command-system. **Created**: 2026-06-20.
>
> **Cross-link**: extends the shipped Unified Command System (`docs/plans/command-system-plan.md`) and the untrusted-input surface tracked in `docs/self-improvement/categories/security.md`.

## Context

The Unified Command System funnels four input surfaces — CLI (`Smatchet.exe cmd`), MCP `tools/call`, the in-app Command Palette, and Lua `commands.invoke` — through one chokepoint, `CommandRegistry::Dispatch` (`Source/Core/src/Commands/CommandRegistry.cpp:266`). That chokepoint is already strong: it traps every handler exception (`:327`), coerces argument types, and runs a source-aware destructive-confirm gate (`RequiresExplicitConfirm`).

The `2026-06-13` security campaign hardened the *byte-parser* surfaces (AI stream, markdown, tracker mappers, image dims), MCP auth / DNS-rebind, the destructive-confirm gate (#1246), and the MCP/UI cross-thread `g_ui` race (fixed 2026-06-18). What it did **not** cover is the **command-argument validation layer** itself. Two uncatchable crash classes plus a set of CLI argv-bounds gaps remain — reachable from automation (CLI / MCP / Lua) and therefore worth closing.

After this lands: a malformed argument from any of the four surfaces — a depth-bomb JSON string, an out-of-range integer, a multi-megabyte string, a bad `--mcp-port` — yields a structured `validation-error`, never a crash.

## Approach

Fix at the chokepoint first, per-surface second. Two centralized changes close the cross-surface crash classes in one place:

1. Route the `ParamType::Json` string-coercion path (and the `config.set` value parse) through the existing bounded SAX parser `json_safe::ParseBounded` instead of recursive `nlohmann::json::parse`. The recursive parser stack-overflows on a deeply-nested string *before* any try/catch can fire; the bounded parser caps depth/nodes/bytes and never throws across the boundary.
2. Add optional numeric range + max-length to `ParamSpec` and enforce them in `ValidateAndResolveArgs`. Bounds are opt-in per parameter (unset = unbounded), so existing commands are unaffected until a spec is backfilled; the integer-overflow and unbounded-allocation vectors close the moment a bound is set.

The residuals are per-surface and small: CLI argv bounds (the one surface with hand-rolled parsing), an MCP attachment-proxy URL-length cap, and a Command-Palette selection-reset nit. Test coverage reuses the already-shipped libFuzzer harness (`tests/fuzz/`, `smatchet_add_fuzz_target`) with one new driver over the arg-coercion path, plus per-fix doctests.

Trade-off named: ranges live on `ParamSpec` (declarative, one enforcement site) rather than inside each handler, so the bound is visible in `commands.help` / MCP `inputSchema` and cannot be forgotten by a new handler.

## Files to modify

Core chokepoint (Phase 0 — fixes all four surfaces):

1. `Source/Core/include/Commands/Command.h:34` — add `MinInt` / `MaxInt` / `MaxLen` (C++14-safe `shared_ptr<long long>` / sentinel) to `ParamSpec`.
2. `Source/Core/src/Commands/CommandRegistry.cpp:197` — swap recursive `json::parse` → `json_safe::ParseBounded` in `CoerceJsonValue` (`ParamType::Json`); `:214-262` — enforce range/length in `ValidateAndResolveArgs`, returning `ErrorCode::ValidationError`.
3. `Source/Core/src/Commands/Builtin/BuiltinCommands_Config.cpp:151` — swap recursive `json::parse` → `ParseBounded` for the `config.set` value.

CLI argv bounds (Phase 1 — the only hand-rolled parser):

4. `Source/Standalone/CliCommandRunner.cpp:375-397` — validate `--mcp-port` in [1,65535], `--timeout` >= 0, reject empty `--mcp-host`; `:1015` + `:1121` — clamp `frames` to `[0, INT_MAX/1000]` before the `(frames/60+30)*1000` wait math; response / `instance.json` read — add a max-bytes guard.

Residuals (Phase 2-3):

5. `Source/Plugins/Mcp/McpPlugin.cpp:308` — cap the attachment-proxy `url` query-param length + reject control chars (HTTP 414/400).
6. `Source/Core/src/Commands/CommandPaletteUi.cpp:108-110` — reset `selected_ = 0` when the filtered list empties.
7. `LUA_GUIDE.md` — document the exact instruction budgets (100k console/MCP, 500M automation).

Tests:

8. `tests/fuzz/CMakeLists.txt` + `tests/fuzz/fuzz_command_args.cpp` + `tests/fuzz/corpus/command_args/*` — new driver over `CoerceJsonValue` + `ValidateAndResolveArgs`.
9. `tests/Core/*.test.cpp` — doctests: depth-bomb `Json` param, out-of-range `Int`, oversized `String`, bad `--mcp-port` / `--timeout`, `frames` overflow clamp.

## Existing utilities reused

- `json_safe::ParseBounded` — `Source/Core/include/Json/BoundedJsonParse.h:119` — the shared depth/node/byte-bounded parser; already the ingress parser for MCP envelopes + the Lua `decode_json` sink. Reused, not forked.
- `smatchet_add_fuzz_target(...)` — `tests/fuzz/CMakeLists.txt:21` — the shipped libFuzzer target helper (preset `ninja-fuzzer-linux`, CI `fuzz-smoke.yml`). New driver slots in with no infra change.
- `CommandRegistry::Dispatch` handler try/catch — `Source/Core/src/Commands/CommandRegistry.cpp:327` — the existing safety net; range/length checks run *before* it, in `ValidateAndResolveArgs`.
- `RequiresExplicitConfirm` — `Source/Core/include/Commands/Command.h:132` — unchanged; this plan does not touch the confirm gate.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — argument validation/coercion runs once per command *dispatch* (a cold path), never in the per-frame render loop. `ParseBounded` operates on an already-in-memory string.
- **Pillar 2 (UI never blocks > 100 ms)**: no impact — no new synchronous I/O; the changed code is pure CPU parsing/validation.
- **Pillar 3 (never crash)**: this is the plan's purpose — closes two uncatchable stack-overflow classes (recursive `json::parse` on a nested string) and an integer overflow (`frames`), and converts unbounded `Int` / `String` / URL args into structured `validation-error`s.
- **Pillar 4 (accessibility)**: N/A — no UI surface change beyond the Palette selection-reset nit (no keyboard-nav / font / contrast impact).

## Perf-review-system gates

Diff touches `Source/Core/` → gates declared:

1. **PR-fast CI** — closest scenario: the `command-palette` fuzzy scenario (`CommandPaletteFuzzyScenario`), which drives the command system end-to-end. The validation path is per-dispatch (cold), so no steady-state frame cost is expected; PR-fast is a guard, not a target here.
2. **Pillar 2 static scanner** — N/A: no new sync-I/O reachable from `ImGui::*` (pure parsing/validation).
3. **Dispatcher drain** — N/A: does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A: adds no sync-stall code path > 100 ms.
5. **Marker inventory** — N/A: adds no `SMATCHET_UI_PERF_SCOPE` markers.

**Override**: `perf-out-of-band` not needed — no steady-state path changes.

## Risks / non-goals

- **Risk — over-tight bounds reject valid args.** Mitigation: `ParamSpec` bounds are opt-in (unset = unbounded); backfill only generous, already-documented limits (`limit <= 500`, `offset >= 0`) and the genuine overflow/DoS cases (`frames`). No silent behavior change for un-backfilled commands.
- **Risk — `ParseBounded` caps reject a legitimately large/deep `Json` arg.** Mitigation: the existing caps (256 depth / 200k nodes / 4 MiB) already govern MCP + Lua ingress and are far above any real command payload; the `config.set` / CLI paths are smaller still.
- **Non-goal — MCP per-token capability allowlist.** Tracked separately (`security.md` MCP-dispatch residual); destructive reach is already gated.
- **Non-goal — `FindLocked` locking.** The lockless-accessor-from-MCP-threads note is coordinated with the existing P2 item (`backlog/BACKLOG_CODE_REVIEW.md:116`), not redesigned here.
- **Non-goal — re-doing shipped hardening** (g_ui race, SSRF/userinfo, image dims, tracker mappers, Lua sandbox) — see § Out of scope.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: doctests per fix — depth-bomb `Json` param returns `validation-error` (no crash), out-of-range `Int` / oversized `String` rejected, `--mcp-port` / `--timeout` bounds, `frames` overflow clamp; `CoerceCliArgValue` + `ValidateAndResolveArgs` table-driven cases.
- **Bucket E (ImGui Test Engine)**: existing `command-palette` scenario stays green; add an empty-filter then re-widen selection-reset assertion.
- **Fuzz**: `fuzz_command_args` builds under `ninja-fuzzer-linux` and passes `-runs=0` seed smoke in `fuzz-smoke.yml`.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — Phase 0 touches gated Command-system TUs compiled into both).
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; record outcome in § Implementation log.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

Already shipped / tracked — do not redo:

- Destructive-confirm gate across CLI/MCP/Lua (#1246); MCP DNS-rebind / Origin / token (#1228).
- MCP/UI cross-thread `g_ui` race — fixed 2026-06-18 (`security.md`).
- MCP attachment-proxy SSRF + userinfo (#1342; SSRF deferred) — this plan adds only the URL-*length* cap.
- Image-dimension caps (#1363 / #1225); tracker-mapper crash-safety (#1344); Lua sandbox + coroutine-hook (#1345).
- libFuzzer harness + the six byte-parser drivers (#1301 / #1307) — reused, not rebuilt.
- DB-corruption hardening — `docs/plans/shipped/slice-g-db-corruption.md`.

## Implementation log

- 2026-06-20 · #1438 — plan doc landed (this file).
- 2026-06-20 · #1441 — Phase 0: `ParamType::Json` coercion + `config.set` value parse routed through `json_safe::ParseBounded`; `ParamSpec` gained opt-in `MinInt`/`MaxInt`/`MaxLen` enforced in `ValidateAndResolveArgs` via the new pure `ParamBoundsPure.h`; bucket-A test `tests/Core/ParamBoundsPure.test.cpp`. MSVC build + Coverage doctest rig went green.
- 2026-06-20 · #1444 — Phases 1-3: CLI argv bounds (`--mcp-port` 1-65535, `--timeout` >= 0, non-empty `--mcp-host`) + `scenario.run --frames` overflow clamp at both driver sites, via new pure `CliArgCoercion` helpers (`IsValidMcpPort` / `ClampScenarioFrames` / `ScenarioFramesFromJson`, tested in `tests/Core/CliArgCoercion.test.cpp`); MCP `attachment_proxy` `url` length cap (8 KiB -> HTTP 414); Command Palette selection reset on empty filter.
- 2026-07-14 — **the two deferred items closed** (this PR):
  - **Phase 1.3 — CLI result-file size cap.** The scenario RESULT-file readers in `CliCommandRunner.cpp` (in-process) and `CliDispatch.cpp` (`--spawn`) read the whole file into a `std::string` *before* `json_safe::ParseBounded`'s 4 MiB cap could reject it, so a multi-GB result file OOM'd the parent first. Both now read through the extracted shared leaf `Source/Standalone/CliResultFileRead.h` (`ReadResultFileBounded` → `{Ok, OpenFailed, TooLarge}`), which stops past a 4 MiB cap and returns `TooLarge` (surfaced as a clean `handler-error` envelope, not a misleading "invalid JSON"). The extraction retires the pre-existing read-loop clone the DRY gate flagged and shrinks `SpawnAndRunHandleAsync` back under the 120-line cap; the residual per-surface error-envelope emit (differs by command var + shutdown call) stays clone-exempt. `instance.json` was already capped (#1747). Helper behaviour verified locally (Ok / OpenFailed / TooLarge paths) on a clang build; the two dispatch TUs compile only under MSVC → CI is the compile validator.
  - **`fuzz_bounded_json` driver.** Per the plan's own recommendation ("a lower-risk `fuzz_bounded_json` over the header-only `ParseBounded` is the recommended first cut"), added `tests/fuzz/fuzz_bounded_json.cpp` + 8-seed corpus + `smatchet_add_fuzz_target` wiring. It fuzzes `ParseBounded` / `ParseBoundedOrDiscarded` (the shared ingress the `ParamType::Json` coercion + `config.set` value parse route through) on adversarial bytes with input-derived depth/node/byte caps to steer the SAX abort paths. **Built + fuzz-smoked locally** under `-fsanitize=fuzzer,address,undefined` (clang 18): `-runs=0` corpus smoke passes and a 20,000-run session found no crash. `fuzz_closure_audit` PASS (16 targets). The `CoerceJsonValue`/`ValidateAndResolveArgs`-through-`Dispatch` driver the plan floated as the harder alternative stays deferred (needs the registry link-closure) — `fuzz_bounded_json` covers the shared depth-bomb parser that is the actual crash-class.

## Deviations from plan

- **Test strategy** — the new bounds checks were extracted into pure headers / helpers (`ParamBoundsPure.h`, `CliArgCoercion`) and bucket-A tested directly rather than via registry-linked integration tests, mirroring `CommandSourceTrust.test.cpp` (the doctest rig deliberately keeps the registry + handlers out of its link closure).
- **Phase 3 doc item already satisfied** — the Lua instruction budgets the plan asked to document are already in `LUA_GUIDE.md`, so no doc edit shipped.
- **Deferred items — now LANDED (2026-07-14, this PR), status flipped to `shipped`:**
  - **Phase 1.3 — CLI result-file size cap.** Done via the shared `CliResultFileRead.h` leaf (see § Implementation log). Scope note: the plan title said "response / `instance.json`"; `instance.json` was already capped (#1747), so this closed the remaining scenario-RESULT-file readers.
  - **`fuzz_command_args` driver.** Shipped as `fuzz_bounded_json` — the plan's own recommended first cut over the header-only `ParseBounded` (the shared depth-bomb parser the command-arg `Json` coercion routes through). The `Dispatch`-linked registry-closure driver stays a deferred follow-up (higher build cost, same crash-class already covered).

## Verification (actual)

- **Bucket A (doctest rig)** — `ParamBoundsPure.test.cpp` (#1441) and the new `CliArgCoercion.test.cpp` cases (#1444) built + passed under the Coverage (windows-2022 + OpenCppCoverage) lane, which also confirms MSVC compilation of the changed Core / Standalone / Mcp TUs.
- **Strict-zone lint, comment-noise, Pillar-2, Test-delta, Fuzz-smoke, TSan** — green on both PRs (one comment-noise reword needed on #1441; the heuristic flagged a comment ending in `);`).
- **Not run locally** — the implementing session had no Windows/MSVC toolchain, so CI was the sole build/test validator. Both PRs merged on green required checks; CodeRabbit + Cursor Bugbot were rate/usage-limited on #1444 and did not review it (both are PR-advisory, not required checks).

## Archive (post-ship — DO IN THIS PR, never a follow-up)

1. flip the § Status header to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
