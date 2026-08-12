# Slice G — DB-corruption robustness for the local cache
<!-- plan-date: 2026-06-16 -->

**Status:** **shipped** — Phase 1 characterization (#1327); Phase 2 corrupt-file rebuild; Phase 3 / G2 `SQLITE_BUSY` contention hardening (2026-07-14, greenlit in-session).
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

**Shipped approach (see § Implementation log):** the ctor opens `db` normally and runs `InitSchema()` in a
`try`; on `SQLite::Exception` it inspects `getErrorCode()` and rebuilds **only** for genuine on-disk
corruption (`SQLITE_NOTADB` / `SQLITE_CORRUPT`) — a transient `SQLITE_BUSY` / `SQLITE_CANTOPEN` / `SQLITE_IOERR`
(DB momentarily locked, a permission/ENOENT race) is **re-thrown**, never quarantined (CodeRabbit #1352
finding). `RebuildFreshAfterCorruption` then releases the handle (`db = SQLite::Database(":memory:")` —
SQLiteCpp 3.3.1 `Database` is move-assignable, confirmed in the vendored header), quarantines the main DB +
its `-wal`/`-shm` sidecars to `<db>.corrupt-<unixts>` (Windows can't move an open file, hence the release
first), reopens fresh on the same path, and re-inits. Empty/missing files never reach the catch (SQLite
opens them as a fresh DB and `InitSchema` succeeds). No per-open probe → zero extra work on the healthy
startup path (this is the v1 double-open that regressed `Perf PR-fast`).

### Phase 3 → Slice G2 — `SQLITE_BUSY` contention (SHIPPED 2026-07-14)

Concurrent-open / busy-timeout behaviour under contention. Split out so its concurrency surface did not
gate the Phase 1 safety net. Greenlit in-session and shipped here.

**Shipped approach.** Phase 2 deliberately RE-THREW a transient `SQLITE_BUSY` out of the ctor (never
quarantine a healthy-but-locked cache) — correct for the corruption axis, but it left a second Smatchet
opening the same on-disk cache able to crash startup on the schema-init write-lock conflict (Pillar 3).
G2 closes that:

1. **`ApplyWalPragmas` arms `busy_timeout` FIRST** (before `journal_mode=WAL`), so even the WAL pragma —
   which needs a brief exclusive lock and can itself return `SQLITE_BUSY` under a concurrent open — waits
   on the lock, and the timeout is set even if a later pragma throws. (Previously the timeout was set
   LAST, so a `SQLITE_BUSY` on the WAL pragma left `InitSchema` running with no wait → immediate crash.)
2. **Bounded backoff-and-retry around `InitSchema`.** New pure classifier `IsTransientBusyCode`
   (`SQLITE_BUSY` / `SQLITE_BUSY_SNAPSHOT` / `SQLITE_LOCKED` / `SQLITE_PROTOCOL`) + policy helpers
   `ShouldRetryBusyInit` / `BusyRetryBackoffMs` in `SqliteOpenRecoveryPure.h` (disjoint from the
   corruption axis — a transient is neither quarantined nor a permanent fault). The ctor retries
   schema-init up to 5 attempts with a capped-exponential backoff (25→50→100→200→400 ms; total < 400 ms,
   a human-imperceptible startup hiccup) on a transient code, rebuilds on corruption (unchanged), and
   re-throws anything else (`CANTOPEN`/`IOERR`) untouched.

**Tests.** Pure retry-policy + classifier cases appended to `SqliteOpenRecoveryPure.test.cpp`
(mutual-exclusion with `IsRebuildableCorruptCode`, attempt-budget boundaries, backoff shape) —
verified locally against the real `/usr/include/sqlite3.h` codes via a standalone harness. A new
`[high-risk]` concurrency doctest `tests/Core/LocalCacheManagerContention.test.cpp` drives an 8-thread
stampede of ctors on ONE on-disk path through a start-gate and asserts every open survives + the shared
cache is coherent afterwards (file-backed via `TempDbFile.h`). The doctest rig is CI-built (local VS18
`/EHsc` blocker, same posture as Phase 2).

## Files (this PR)

| File | Change |
|---|---|
| `tests/support/TempDbFile.h` | **new** — shared on-disk RAII temp-DB helper (header-only, C++14) |
| `tests/Core/LocalCacheManagerCorruption.test.cpp` | **new** — 4 `[high-risk]` characterization cases |
| `tests/Core/LocalCacheTicketsV2Migration.test.cpp` | consume shared helper; drop local class + dead includes |
| `tests/CMakeLists.txt` | wire new TU into `SmatchetTests` (LCM + SQLiteCpp already linked) |
| `docs/guides/testing-surface.md` | Gap 6 status: unbacklogged → in-progress (Phase 1) |
| `docs/plans/shipped/slice-g-db-corruption.md` | this plan |

## Perf gate

- **Phase 1: N/A** — touched `tests/**` + `docs/**` only.
- **Phase 2 (this PR): touches `Source/Core/src/Persistence/LocalCacheManager.cpp`** but only the
  **constructor** (a one-time startup cache-open path), not any steady-state per-frame / UI-thread path.
  No `SMATCHET_UI_PERF_SCOPE` added. The healthy path is **unchanged** — open once + `InitSchema` as before,
  zero extra work (the v1 pre-open probe that added a second open per construction regressed `Perf PR-fast`
  and was removed). Extra work happens only on the genuinely-corrupt path (rename + rebuild), off the render
  thread. PR-fast CI re-runs on the fix push.

## Verification

- `cmake --build` the test target + `ctest` — all four new cases green; V2 migration suite still green
  after the shared-header swap.
- No product code touched → no manual UI / visual-validation step.

## Implementation log

- Phase 1 authored: shared `TempDbFile.h` extraction, corruption test, V2-test swap, CMake wiring,
  Gap-6 doc status. Shipped #1327.
- **Phase 2 (this PR):** `LocalCacheManager.cpp` — extracted the schema-init body into a re-runnable
  `InitSchema()` + the WAL pragmas into `ApplyWalPragmas()` (both new private members); added a `dbPath_`
  member (declared before `db` so it inits first); the ctor catches `InitSchema`'s `SQLite::Exception`,
  rebuilds via `RebuildFreshAfterCorruption` only on `SQLITE_NOTADB`/`SQLITE_CORRUPT` (re-throws transient
  codes), using the no-throw anon-namespace helper `QuarantinePath` (`fs::rename`). `LocalCacheManager.h` — `dbPath_` member + `InitSchema`/
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

- **v1 shipped a pre-open probe; revised to catch-in-body after CR + perf feedback.** The first cut
  (PR #1352 initial) ran a read-only `PRAGMA schema_version` probe before opening `db`. CodeRabbit flagged
  (Major) that catching *every* `SQLite::Exception` would quarantine a healthy-but-transiently-unreadable
  DB (BUSY/CANTOPEN), and `Perf PR-fast` regressed on the per-construction double-open. Revised to the
  catch-in-body design: open once, `try { InitSchema() }`, and rebuild **only** on `SQLITE_NOTADB`/
  `SQLITE_CORRUPT` (re-throw transient codes). Handle release uses `SQLite::Database` move-assignment —
  confirmed present in the vendored SQLiteCpp 3.3.1 header (`Database& operator=(Database&&) = default`).
  Net: fixes the CR finding + removes the healthy-path perf cost; same corrupt-file behaviour.
- **Quarantines the `-wal`/`-shm` sidecars too** (not just the main DB) — a stale WAL against a fresh main
  DB is itself a corruption hazard; the plan named only `<db>.corrupt-<ts>`.
- **Phase 2 shipped against THIS doc, not a separate plan.** The doc's loop-mode note said Phases 2–3 ship
  under their own plans; the user greenlit Phase 2 in-session, so it was recorded here. Phase 3 still gets
  its own plan.
- **Doctest run deferred to CI** (see § Verification) — local rig unbuildable on this VS18 toolset.
