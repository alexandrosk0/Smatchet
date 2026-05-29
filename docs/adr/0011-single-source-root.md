# Single `Source/` root for all first-party C++

# Status

**Accepted (2026-05-29).** Implementation pending (plan-stage). Plan: [`docs/plans/active/source-root-consolidation.md`](../plans/active/source-root-consolidation.md) (grill Q1–Q3). Distinct from the already-landed [`source-core-dir-reorg`](../plans/active/source-core-dir-reorg.md), which created subsystem subdirs *inside* `Source_Core/`.

# Context

First-party C++ lived in four sibling top-level roots — `Source_Core/`, `Target_Standalone/`, `Plugins/`, `UnrealPlugins/` — plus test C++ under `tests/`. Four roots hurt navigability and made "where does the C++ live" non-obvious to humans and agents alike. This is a mechanical, zero-behaviour-change reorg.

# Decision

Consolidate all first-party C++ under a single `Source/` root, **subdir-per-component**, preserving each component's internal layout:

- `Source_Core/` → `Source/Core/`, `Target_Standalone/` → `Source/Standalone/`, `Plugins/` → `Source/Plugins/`, `UnrealPlugins/` → `Source/UnrealPlugins/`.
- The `tests/` **root stays at repo root** (it is heavily wired into coverage/CI/lint tooling and conventionally lives at root), but its product mirror `tests/Source_Core/` → `tests/Core/` to track the renamed core.
- **Flat include namespace is kept** — bare `#include "Foo.h"` resolves against CMake include-dir roots, so the move re-roots ~10 CMake `target_include_directories` entries and rewrites *zero* `#include` directives (the lone exception is one relative cross-tree include in `Logger.cpp`).
- Ships as **one atomic PR** off `develop` so no path-filtered CI gate / lint script / `CODEOWNERS` / `.coderabbit.yaml` is left pointing at stale paths in an intermediate state.

# Considered options

- **Flatten core to `Source/` directly** (`Source_Core/{src,include}` → `Source/{src,include}`, others as siblings) — rejected: more disruptive include-path rewiring and an awkward core-is-the-root asymmetry.
- **Move `tests/` under `Source/` too** — rejected: `tests/` is coupled to coverage-delta-gate, CI path filters, and the git-janitor whitelist; migrating it is separate, higher-churn work. Only the `tests/Source_Core` mirror renames.
- **Rewrite bare includes to path-qualified (`#include "Tracker/JiraClient.h"`)** — rejected: far larger blast radius and would churn the Unreal-side consumer; the goal is navigability, not include hygiene.
- **Status quo (four roots)** — rejected: the navigability cost is the reason for this change.

# Consequences

- The high-integrity lint zone globs ([`scripts/dev/test-lint-rules.sh`](../../scripts/dev/test-lint-rules.sh) `STRICT_GLOBS`) and their `--selftest` twin in `AGENTS.md` § Tiered enforcement re-root in lockstep, or enforcement silently no-ops on the moved tree. Likewise `CODEOWNERS`, `.coderabbit.yaml` path rules, and every `paths:`-filtered CI workflow.
- `SmatchetImGuiPlugin.Build.cs` (Unreal, **not** covered by the CMake dual-target gate) gains one level in its repo-root walk and points at `Source/Core` — verify at UE package time.
- `docs/high-integrity/baseline.md` is regenerated via `--catalog --refresh`; the delta gate keys on `(rule, basename, hash)`, so the move itself introduces no new violations.
- Reversing this is another full mechanical churn — treat the `Source/` shape as intentional architecture, not an implementation detail.
