# Plan: Tighten catch-all exception handling policy
<!-- plan-date: 2026-05-27 -->

## Context

CR feedback identified that `catch (...)` is used ~70 times across `Source_Core/` and `Plugins/`. Most log, but ~25 are silent or near-silent — hiding bugs behind graceful degradation. The codebase already follows a two-level pattern (`catch (const std::exception&)` + `catch (...)`) in many places, but inconsistently. Goal: codify a policy, fix the worst offenders, and add lint enforcement so new empty catch-alls can't land.

## Policy (add to AGENTS.md § Project rules)

**Exception handling tiers:**

| Tier | Location | Required pattern |
|------|----------|-----------------|
| Boundary | main, thread entry, plugin, Lua, MCP, CLI | `catch (const std::exception& ex)` + LOG_ERROR with context; `catch (...)` + LOG_ERROR |
| Config/parse | JSON, ini, regex, user-authored data | `catch (...)` + silent fallback OK **if** inline comment documents the default. LOG_WARN preferred |
| Cache/DB | SQLite, LocalCacheManager | `catch (const std::exception& ex)` + LOG_ERROR. Rethrow unless destructor |
| Network/API | HTTP clients, JSON response parsing | `catch (const std::exception& ex)` + LOG_WARN + return error/default |
| Core logic | Field catalog, sync, mutations | Prefer `catch (const std::exception&)` — no bare `catch (...)` without LOG |
| UI drawing | ImGui render paths | `catch (...)` OK for state rollback, must set error flag or LOG_DEBUG |

**Hard rules:**
1. `catch (...) {}` (empty body) is a review CRITICAL — must log or have inline comment justifying silence
2. Every `catch (...)` with a log must include operation context (function name, key, path, backend)
3. New `catch (...)` in core logic requires inline comment citing the tier

## Changes

### Slice 1: Policy doc + AGENTS.md rule (docs-only)

- Add `docs/agent-rules/exception-handling-policy.md` with full tier table, examples, escape hatches
- Add 1-liner to AGENTS.md § Project rules referencing the policy doc
- Commit `docs/plans/shipped/policy-tighten-logging-raii.md` if it overlaps (currently untracked)

### Slice 2: Fix silent catch-alls (verified offenders)

Every `catch (...) {}` or `catch (...) { return X; }` with no log and no comment. Fixes are LOG additions only — no behavioral changes.

**Truly empty (no body at all):**
- `Source_Core/src/Commands/CommandRegistry.cpp:364` — JSON recents parse → add LOG_DEBUG
- `Source_Core/src/AppController.cpp:1485` — empty catch in init → add LOG_WARN
- `Plugins/LuaConsole/LuaConsolePlugin.cpp:184` — empty → add LOG_DEBUG

**Silent return with no log or comment:**
- `Source_Core/src/CallstackParser.cpp:65,92,118` — `return false` with no log → add LOG_TRACE (hot parser path)
- `Source_Core/include/JsonParseUtil.h:37,57` — `return fallback` → add LOG_TRACE (hot path, template helper)
- `Source_Core/src/GitHubIssueSearchMapping.cpp:33` — `return string()` → add LOG_WARN
- `Source_Core/src/AppController_LuaBindings.cpp:250` — `return "?"` → add LOG_TRACE
- `Source_Core/src/AppController_LuaBindings.cpp:1084` — sets default, no log → add LOG_DEBUG
- `Source_Core/src/AppController_LuaBindings_Draw.cpp:763` — JSON parse fallback → add LOG_TRACE
- `Source_Core/src/AppController_LuaBindings_Draw.cpp:922` — stringify fallback → add LOG_TRACE
- `Source_Core/src/AppController_CatalogAndFieldEdit.cpp:1079` — sets outError but no LOG → add LOG_WARN

**Silent with comment (add LOG anyway for diagnosability):**
- `Source_Core/src/ConfigManager.cpp:512,874,886,1076` — config parse fallbacks → add LOG_DEBUG + keep comment
- `Source_Core/src/BackendAuditTrail.cpp:260,365` — add LOG_DEBUG (audit must not block, but should log)
- `Plugins/Mcp/McpPlugin.cpp:186,321` — add LOG_DEBUG + keep comment
- `Plugins/Mcp/McpJsonRpcPure.cpp:70,248` — add LOG_TRACE

### Slice 3: Upgrade bare catch-all to two-level in core logic

Split standalone `catch (...)` into `catch (const std::exception& ex)` + `catch (...)` where the try block wraps non-trivial logic (not a 1-line parse). Targets:

- `Source_Core/src/AppController_Connectivity.cpp:33,134,155` — 3 sites
- `Source_Core/src/FieldCatalogCache.cpp:422,483,538,568,611` — 5 sites (some already two-level, verify)
- `Source_Core/src/AppController_CatalogAndFieldEdit.cpp:791,867,1038,1079` — 4 sites (JSON dump catches may stay single-level with comment)

### Slice 4: Lint enforcement for empty catch-alls

Add `docs/harness/claude-code/hooks/lint-catch-all.py` and wire into the lint pipeline. Must:

1. Flag `catch (...) {}` and `catch (...) { }` (empty body) as ERROR
2. Flag `catch (...)` with no `LOG_` call in the body as WARNING
3. Allow suppression via inline comment: `// catch-all-ok: <reason>`
4. Run on every C++ edit in `Source_Core/`, `Plugins/`, `Target_Standalone/`

## Files modified

| Slice | Files |
|-------|-------|
| 1 | `AGENTS.md`, `docs/agent-rules/exception-handling-policy.md` (new) |
| 2 | ~12 source files (add LOG lines only) |
| 3 | ~3 source files (split catch blocks) |
| 4 | `docs/harness/claude-code/hooks/lint-syntax-both.py` |

## Verification

1. Slice 1: docs-only, no build needed
2. Slice 2-3: `cmake --build --preset ninja-iter-msvc` clean
3. `grep -rn "catch (\.\.\.) {}" Source_Core/ Plugins/` returns zero hits (except test fixtures)
4. Slice 4: introduce deliberate `catch (...) {}` in a test file, verify lint flags it, remove
5. Run app, open a few panels — verify no LOG spam from newly-added TRACE/DEBUG lines under normal operation
