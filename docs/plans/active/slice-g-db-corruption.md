# Slice G — DB-corruption robustness for the local cache

**Status:** Phase 1 **shipped** (#1327, merged 2026-06-16). Phases 2–3 not yet greenlit (ship under their own approved plans). Plan stays active to track those.
**Source:** `docs/guides/testing-surface.md` §6 Gap 6 ("persistence corruption untested").
**Routing:** `offline-sync` (owns `LocalCacheManager`); `test-rig` refuses SQLite surfaces.
**Loop mode:** `in` (plan-gated). This plan covers Phase 1 only; Phases 2–3 ship under their own approved plans.

## Problem

`LocalCacheManager`'s constructor opens the on-disk SQLite cache and initialises the schema.
Only the `PRAGMA journal_mode=WAL` / `synchronous=NORMAL` / `setBusyTimeout` block is wrapped in
`try/catch → LOG_WARN`. The `db.exec("CREATE TABLE IF NOT EXISTS tickets ...")` at
`Source/Core/src/Persistence/LocalCacheManager.cpp` (~line 145) and **all** subsequent schema-init is
**unguarded**. A corrupt or truncated on-disk file therefore throws `SQLite::Exception` (SQLITE_NOTADB)
straight out of the constructor — an **uncaught startup crash** (Quality Pillar 3, "never crash").

A zero-length file is **not** corrupt: SQLite treats an empty file as a brand-new database, so
`OPEN_CREATE` on it succeeds. Any fix must preserve that (empty ≠ garbage-to-nuke).

## Phased delivery

Split so the safety net lands before the behaviour change, and the riskier concurrency work is isolated.

### Phase 1 — characterization test (THIS PR)

Pin **today's** behaviour with a `[high-risk]` doctest suite — **no product-code change**:

- `tests/Core/LocalCacheManagerCorruption.test.cpp` — four cases:
  1. garbage (200 non-SQLite bytes) → `CHECK_THROWS_AS(LocalCacheManager(path), SQLite::Exception)`
  2. truncated 16-byte magic-only header → `CHECK_THROWS_AS(..., SQLite::Exception)`
  3. zero-byte file → `REQUIRE_NOTHROW(...)` ctor + `GetAllTicketIds(kBk).empty()` (empty = fresh DB)
  4. healthy reopen (control) → write a ticket, reopen the same path, ticket persists
- File-backed: the throw is on the on-disk header read, unreachable via `:memory:`. Uses a shared
  on-disk RAII helper `tests/support/TempDbFile.h`, **extracted** from the local copy in
  `LocalCacheTicketsV2Migration.test.cpp` (DRY Pillar 5 — avoid a new copy-paste clone) and consumed by
  both tests. The V2 test drops its now-dead `<atomic>`/`<chrono>`/`<cstdio>`/`<cstdlib>` includes.
- Cases 1–2 encode the **crash**: when Phase 2 flips them to expect a graceful rebuild, the assertion
  change is the visible, intentional diff — never a silent behaviour change.

### Phase 2 — hardening (separate later PR, NOT greenlit)

Make the ctor survive a corrupt file: on an unreadable open, log a loud user-visible cue, rename the
bad file to `<db>.corrupt-<ts>` (forensics), and rebuild a fresh cache. Flips cases 1–2 above to expect
`REQUIRE_NOTHROW` + a fresh-empty cache; keeps case 3 (empty = fresh) and case 4 (healthy) intact.
Persistence strict-zone constraints apply (additive-only schema, off-UI-thread SQLite, append-only
redacted audit). Owner: `offline-sync`.

### Phase 3 → Slice G2 — `SQLITE_BUSY` contention (separate slice, NOT greenlit)

Concurrent-open / busy-timeout behaviour under contention. Split out so its concurrency surface does not
gate the Phase 1 safety net.

## Files (this PR)

| File | Change |
|---|---|
| `tests/support/TempDbFile.h` | **new** — shared on-disk RAII temp-DB helper (header-only, C++14) |
| `tests/Core/LocalCacheManagerCorruption.test.cpp` | **new** — 4 `[high-risk]` characterization cases |
| `tests/Core/LocalCacheTicketsV2Migration.test.cpp` | consume shared helper; drop local class + dead includes |
| `tests/CMakeLists.txt` | wire new TU into `SmatchetTests` (LCM + SQLiteCpp already linked) |
| `docs/guides/testing-surface.md` | Gap 6 status: unbacklogged → in-progress (Phase 1) |
| `docs/plans/active/slice-g-db-corruption.md` | this plan |

## Perf gate

**N/A** — diff touches `tests/**` + `docs/**` only; no `Source/Core/` product code. No perf-sensitive
path changes, so no perf-gate run required this PR. (Phase 2, which edits the LCM ctor, will need one.)

## Verification

- `cmake --build` the test target + `ctest` — all four new cases green; V2 migration suite still green
  after the shared-header swap.
- No product code touched → no manual UI / visual-validation step.

## Implementation log

- Phase 1 authored: shared `TempDbFile.h` extraction, corruption test, V2-test swap, CMake wiring,
  Gap-6 doc status. Build + ctest pending.

## Deviations

- None.
