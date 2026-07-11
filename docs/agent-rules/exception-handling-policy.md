# Exception handling policy

## Tiers

| Tier | Location | Required pattern |
|------|----------|-----------------|
| Boundary | main, thread entry, plugin, Lua, MCP, CLI | `catch (const std::exception& ex)` + LOG_ERROR with context; `catch (...)` + LOG_ERROR |
| Config/parse | JSON, ini, regex, user-authored data | `catch (...)` + silent fallback OK **if** inline comment documents the default. LOG_WARN preferred |
| Cache/DB | SQLite, LocalCacheManager | `catch (const std::exception& ex)` + LOG_ERROR. Rethrow unless destructor |
| Network/API | HTTP clients, JSON response parsing | `catch (const std::exception& ex)` + LOG_WARN + return error/default |
| Core logic | Field catalog, sync, mutations | Prefer `catch (const std::exception&)` — no bare `catch (...)` without LOG |
| UI drawing | ImGui render paths | `catch (...)` OK for state rollback, must set error flag or LOG_DEBUG |

## Hard rules

1. `catch (...) {}` (empty body) is a **review CRITICAL** — must log or have inline comment justifying silence.
2. Every `catch (...)` with a log must include **operation context** (function name, key, path, backend).
3. New `catch (...)` in core logic requires inline comment citing the tier.

## Preferred pattern

```cpp
try {
    DoThing();
} catch (const std::exception& ex) {
    LOG_ERROR("DoThing failed issue=%s backend=%s: %s", issueKey.c_str(), backend, ex.what());
    return false;
} catch (...) {
    LOG_ERROR("DoThing failed issue=%s backend=%s: unknown exception", issueKey.c_str(), backend);
    return false;
}
```

## Escape hatches

Silent catch-alls are allowed with an inline comment when:

- The catch wraps a **1-line stringify/dump** used only for logging (failure means the log line says `"?"` instead of the value — acceptable).
- The catch is in a **destructor** or **thread-exit** path where throwing would call `std::terminate`.
- The catch is a **parse helper** where failure is the expected signal (e.g. `stoi` on user input).

Mark these with `// catch-all-ok: <reason>` so the lint check skips them.

## Lint enforcement

`docs/harness/claude-code/hooks/lint-catch-all.py` flags:
- `catch (...) {}` or `catch (...) { }` (empty body) as **ERROR**
- `catch (...)` with no `LOG_` call in the body as **WARNING**
- Suppressed by `// catch-all-ok:` on the catch line or in the body

The empty-body ERROR tier is also a **CI merge gate**: `test-lint-rules.sh --diff` enforces it
absolute-0 over all first-party C++ as rule `catch-all-swallow` (a comment inside the body,
`// catch-all-ok:`, or a deviation comment for rule `catch-all-swallow` — grammar in
cpp-rules.md — escapes). The no-LOG WARNING tier stays editor-hook-only.
