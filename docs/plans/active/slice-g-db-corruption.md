# Slice G — DB-corruption robustness for the local cache

**Status:** Phases 1–2 **shipped** — Phase 1 characterization (#1327); Phase 2 corrupt-file rebuild (this PR). Phase 3 (`SQLITE_BUSY` contention) not yet greenlit. Plan stays active to track Phase 3.
**Source:** `docs/guides/testing-surface.md` §6 Gap 6 ("persistence corruption untested").
**Routing:** `offline-sync` (owns `LocalCacheManager`); `test-rig` refuses SQLite surfaces.
**Loop mode:** `in` (plan-gated). Phase 2 was greenlit in-session and shipped against this doc; Phase 3 ships under its own approved plan.

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

### Phase 2 — hardening (THIS PR — SHIPPED)

Make the ctor survive a corrupt file: on an unreadable open, log a loud user-visible cue, rename the
bad file to `<db>.corrupt-<ts>` (forensics), and rebuild a fresh cache. Flips cases 1–2 above to expect
`REQUIRE_NOTHROW` + a fresh-empty cache; keeps case 3 (empty = fresh) and case 4 (healthy) intact.
Persistence strict-zone constraints apply (additive-only schema, off-UI-thread SQLite, append-only
redacted audit). Owner: `offline-sync`.

**Shipped approach (see § Implementation log):** rather than a try/catch-in-body + reopen (which would
need `SQLite::Database` move-assignment + a Windows file-handle release dance), the ctor runs a
**pre-open probe** in the init list — `QuarantineIfCorrupt(dbPath_)` opens a LOCAL read-only connection
and `PRAGMA schema_version` forces the header read; on a non-empty unreadable file it quarantines the
main DB + its `-wal`/`-shm` sidecars to `<db>.corrupt-<unixts>` (the probe's handle is released at scope
exit, before the rename — Windows can't move an open file), so the member `db` then opens fresh. Empty
and missing files are left untouched (empty = fresh DB).

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

- **Phase 1: N/A** — touched `tests/**` + `docs/**` only.
- **Phase 2 (this PR): touches `Source/Core/src/Persistence/LocalCacheManager.cpp`** but only the
  **constructor** (a one-time startup cache-open path), not any steady-state per-frame / UI-thread path.
  No `SMATCHET_UI_PERF_SCOPE` added. The added work is a single read-only `PRAGMA schema_version` probe on
  open (and, only on the corrupt path, a file rename + rebuild) — off the render thread, off the hot path,
  so no steady-state (144 Hz) regression. PR-fast CI fires automatically if the curated diff→scenario map
  maps this path to a scenario; no baseline bump expected. `perf-out-of-band` not needed.

## Verification

- `cmake --build` the test target + `ctest` — all four new cases green; V2 migration suite still green
  after the shared-header swap.
- No product code touched → no manual UI / visual-validation step.

## Implementation log

- Phase 1 authored: shared `TempDbFile.h` extraction, corruption test, V2-test swap, CMake wiring,
  Gap-6 doc status. Shipped #1327.
- **Phase 2 (this PR):** `LocalCacheManager.cpp` — extracted the schema-init body into a re-runnable
  `InitSchema()` + the WAL pragmas into `ApplyWalPragmas()` (both new private members); added a `dbPath_`
  member (declared before `db` so it inits first); added anon-namespace helpers `QuarantinePath` (no-throw
  `fs::rename`) + `QuarantineIfCorrupt` (pre-open read-only probe → quarantine corrupt main DB + `-wal`/`-shm`
  → return path) wired into the ctor init list. `LocalCacheManager.h` — `dbPath_` member + `InitSchema`/
  `ApplyWalPragmas` decls. `LocalCacheManagerCorruption.test.cpp` — flipped cases 1–2 to `REQUIRE_NOTHROW`
  + fresh-empty-cache + a `<path>.corrupt-*` forensic-file assertion (new `QuarantineSiblingExists` helper
  via `ghc::filesystem`); kept cases 3–4; reworded the header block to the Phase-2 behaviour.

## Verification (actual)

- **Lint** (`test-lint-rules.sh --diff origin/develop`) — PASS (strict-zone high-integrity, no-raw-new,
  comment-noise all clean after rewording two comments; `comment-ratio` on the header is WARN-only).
- **Dual-target compile** — PASS. `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
  SmatchetCore_DX12` (via `scripts/dev/with-msvc-env.sh`): 0 errors; `LocalCacheManager.cpp` compiled into
  BOTH `SmatchetCore_DX12` and `SmatchetStandalone`; `Smatchet.exe` + `SmatchetCore_DX12.lib` linked.
- **Doctest run** — NOT-RUN locally. The doctest test rig (`SmatchetTests`) cannot build on this dev box's
  VS18 / MSVC 14.38 toolset: every test TU hits doctest's `static_assert "Exceptions are disabled! … /EHsc"`
  (C2338) + C4530 — the pre-existing missing-`/EHsc`-on-the-test-rig infra blocker (tracked as B8 in
  `agentic-backlog-campaign` / `b8-bucket-e-coverage`), NOT this change. CI's `Coverage` job builds the rig
  cleanly (it ran #1261's `OmnibarInputClassifier` doctest) and is the validator for the four corruption
  cases here — same CI-is-sole-validator posture as the libFuzzer lane.

## Deviations

- **Pre-open probe instead of try/catch-in-body + reopen.** The plan sketch implied catching the
  schema-init throw and rebuilding in place. Shipped a pre-open `QuarantineIfCorrupt` probe in the ctor
  init list instead: it avoids depending on `SQLite::Database` move-assignment (unconfirmed on the pinned
  SQLiteCpp 3.3.1) and cleanly releases the probe's OS handle before the rename (Windows file-lock). Same
  observable behaviour (corrupt → quarantine + fresh rebuild; empty/healthy untouched).
- **Quarantines the `-wal`/`-shm` sidecars too** (not just the main DB) — a stale WAL against a fresh main
  DB is itself a corruption hazard; the plan named only `<db>.corrupt-<ts>`.
- **Phase 2 shipped against THIS doc, not a separate plan.** The doc's loop-mode note said Phases 2–3 ship
  under their own plans; the user greenlit Phase 2 in-session, so it was recorded here. Phase 3 still gets
  its own plan.
- **Doctest run deferred to CI** (see § Verification) — local rig unbuildable on this VS18 toolset.
