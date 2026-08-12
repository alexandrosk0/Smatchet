# Plan — Tighten Logging and RAII Policy: Named Escape Hatches
<!-- plan-date: 2026-05-28 -->

> **Slug**: `policy-tighten-logging-raii`

## Context

AGENTS.md says: use `LOG_*`, avoid raw `new`/`delete`, prefer RAII. The actual codebase has exceptions — some reasonable, some copy-paste drift. The gap between stated rule and observed code weakens enforcement: future contributors see exceptions and copy them into normal app code without understanding which are intentional.

This plan converts "rules with leaks" into "rules with named escape hatches." The escape hatches are explicit, rare, commented, and mechanically checkable.

## Problem inventory

### Raw `new` / `delete`

| Site | Type | Action |
|---|---|---|
| pimpl ctors (`impl_(new Impl())`) | pimpl pattern | **Replace** → `make_unique<Impl>()` (C++14 OK) |
| Factory returns (`return new Foo()`) | factory | **Replace** → return `make_unique<Foo>()`, caller takes `unique_ptr` |
| C ABI handles (`SmatchetHost_Create`) | C API ownership | **Keep** — raw pointer is the ABI contract. Comment: `// C-ABI handle — raw pointer is the public contract` |
| Custom-deleter wraps (`PreviewPlanPtr(new X, Deleter{})`) | opaque lifecycle | **Keep** — `make_unique` is incompatible with custom deleters. Comment: `// custom-deleter — make_unique inapplicable` |
| Third-party adapters | external seam | **Keep** — don't touch third-party glue |

Status after agent sweep (2026-05-26): most pimpl + factory sites fixed. Residue at plan time: `WhisperPlugin.cpp:956,992` (denied during sweep) and `MarkdownPreviewRender.cpp:961` (custom deleter). **Update (2026-05-28)**: WhisperPlugin `new PhaseEState()` converted to `std::make_unique<PhaseEState>()`; only the documented custom-deleter wrap in `MarkdownPreviewRender.cpp` remains as an intentional exemption.

### `std::cerr` / `std::cout`

All in `Source_Core/` and `Plugins/` are violations — server-thread errors especially need to be in the app log, not stderr. **Replace all** with `LOG_*`.

Status: `McpPlugin.cpp` cerr fixed. No known remaining sites in Source_Core/Plugins (re-verify with grep).

### `fprintf` / `printf`

| Location | Use | Rule |
|---|---|---|
| `Target_Standalone/main.cpp` pre-GLFW | fatal before logger init | **Exempt** — logger not yet live. Comment: `// pre-logger-init — LOG_* unavailable` |
| `Target_Standalone/CliCommandRunner.cpp` JSON/help stdout | CLI product output (machine-readable) | **Exempt** — `LOG_INFO` would break scripts. Comment: `// CLI stdout — product output, not logging` |
| `Source_Core/` or `Plugins/` | anything | **Never** — use `LOG_*` |
| `Target_Standalone/` non-stdout paths | diagnostic in non-CLI code | **Replace** with `LOG_*` |

## Approach

### Step 1 — Tighten AGENTS.md project rules

Replace the current one-liner bans with an explicit escape-hatch table:

```text
**Raw `new`/`delete`**: banned. Use `std::unique_ptr` + `make_unique`.
Named exceptions (must be commented):
  - C ABI ownership boundary: `// C-ABI handle — raw pointer is the public contract`
  - Custom-deleter wraps: `// custom-deleter — make_unique inapplicable`
  - Third-party adapter seams

**Logging**: `LOG_{DEBUG,INFO,WARN,ERROR,TRACE}` from `Logger.h` — never `printf` / `std::cerr`.
Named exceptions (must be commented):
  - `Target_Standalone/` pre-logger-init fatal: `// pre-logger-init — LOG_* unavailable`
  - `Target_Standalone/` CLI stdout (machine-readable product output): `// CLI stdout — product output, not logging`
```

### Step 2 — Fix remaining raw `new` residue

- `Plugins/Whisper/WhisperPlugin.cpp:956,992` — `new PhaseEState()` → `make_unique<PhaseEState>()`
- Re-grep `Source_Core/ Plugins/ Target_Standalone/` to confirm no other unfixed sites

### Step 3 — Add `// CLI stdout` and `// pre-logger-init` comments to all exempt sites

Makes future greps useful: `grep -rn "printf\|fprintf\|cerr" ... | grep -v "CLI stdout\|pre-logger-init\|C-ABI handle\|custom-deleter"` should return zero hits.

### Step 4 — Add grep gate to test-build-warnings.sh (or new test script)

```bash
# Fail if any unexempted printf/fprintf/cerr in Source_Core/ or Plugins/
violations=$(grep -rn "printf\|fprintf\|std::cerr\|std::cout" \
  Source_Core/ Plugins/ --include="*.cpp" --include="*.h" \
  | grep -v "// CLI stdout\|// pre-logger-init\|// C-ABI\|// custom-deleter\|// pimpl\|ThirdParty/")
if [ -n "$violations" ]; then echo "FAIL: unexempted logging violation"; echo "$violations"; exit 1; fi
```

Similarly for raw `new`.

## Files to modify

1. `AGENTS.md` § Project rules — tighten logging + RAII rules with escape-hatch table
2. `Plugins/Whisper/WhisperPlugin.cpp` — fix 2 remaining `new PhaseEState()`
3. `Target_Standalone/main.cpp`, `CliCommandRunner.cpp` — add exempt comments to all printf/fprintf sites
4. `Source_Core/src/SmatchetImGuiHost.cpp:1105` — add `// C-ABI handle` comment (already partially done)
5. `Source_Core/src/MarkdownPreviewRender.cpp:961` — add `// custom-deleter` comment
6. `scripts/dev/test-lint-rules.sh` (new) — mechanical grep gate for the above

## UX Pillar callouts

- Pillar 3 (never crash): WhisperPlugin fix removes last known raw `new` in plugin paths — reduces OOM-on-new risk.
- Others: no impact (pure policy + comment work).

## Perf-review-system gates

N/A — no Source_Core/ hot paths modified.

## Planned verification

- `grep -rn "printf\|fprintf\|std::cerr" Source_Core/ Plugins/` → zero unexempted hits
- `grep -rn "\bnew\b" Source_Core/ Plugins/ Target_Standalone/ --include="*.cpp" --include="*.h"` → zero unexempted hits
- `cmake --build --preset ninja-iter-msvc` → PASS
- New `test-lint-rules.sh` → PASS

## Out of scope

- `MarkdownPreviewRender.cpp` custom-deleter refactor (separate task if desired)
- Converting CLI stdout to a structured output system (separate task)
- Third-party / FetchContent code

## Implementation log

- `87103037` (2026-05-27) — `fix(policy): tighten logging+RAII rules with named escape hatches (#468)` — shipped the AGENTS.md tightening, named escape hatches, and WhisperPlugin `make_unique` conversion in one PR.

## Deviations from plan

- `scripts/dev/test-lint-rules.sh` (Step 4) — verified shipped in #468 (`8710303`, file added by that PR per `git log --diff-filter=A`). Earlier draft of this section incorrectly claimed the script was deferred; corrected per CR feedback on PR #489.
- Catch-all policy split into its own follow-up plan (`docs/plans/shipped/policy-tighten-catch-all.md`, shipped as PR #471).

## Post-ship verification

- `grep -rn "\bnew\b" Source_Core/ Plugins/ Target_Standalone/ --include="*.cpp"` — only documented escape-hatch sites remain.
- AGENTS.md § Project rules now lists named exception comments for both logging and RAII rules.
- `cmake --build --preset ninja-iter-msvc` clean post-merge.
