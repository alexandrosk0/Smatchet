# ADR 0019 — Runtime UI-thread-affinity assert for wrapper-shaped sync-I/O; static scanner stays for direct primitives

- Status: accepted
- Date: 2026-06-09
- Plan: docs/plans/close-gate-gaps.md (Slice 1a, Gap A)
- Relates to: Quality Pillar 2 (UI never freezes); `scripts/dev/pillar2-scan.sh`; `Source/Core/include/UiThreadAffinity.h`

## Context

Pillar 2 (no UI-thread block > 100 ms) is enforced by the static scanner
`scripts/dev/pillar2-scan.sh`, whose regex matches **direct** blocking primitives
on a render path — `cpr::Get/Post`, `std::ifstream`, `popen(`, `system(`,
`SQLite::…`, `.lock()`. For a direct primitive the heuristic "appears on a render
path ⇒ suspect" is sound.

The 2026-06-08 historical-review sweep found that the entire surviving sync-I/O
cluster — #565 (`ConfigManager::SaveAnnotateAnalysis`), #611
(`LoadPersistentViewsFromDisk`), #732 (`ConfigManager::Save*`), #761
(`P4RunCommand`) — slipped past the scanner because each blocks **through the
project's own wrapper function**, not a direct primitive. The scanner never sees
the wrapper.

The obvious fix — make the scanner wrapper-aware (flag `ConfigManager::Save*` etc.
on a render path) — does not work for this class, and the code proves why:

- These wrappers are **legitimately called both on and off the UI thread**. A
  `ConfigManager::Save` inside a `LaunchBackgroundTask` lambda is correct; the same
  call on the render path is the bug. A file-level grep **cannot see lexical
  scope**, so it cannot tell them apart.
- The cost is concrete: **55** legitimate `ConfigManager::Save*` call sites already
  live under `Source/Core/src/Ui/` against only **9** total worker-only
  annotations. A wrapper-aware regex would demand a 55-site annotation sweep, bury
  the real violators under a large grandfather baseline, and rubber-stamp the
  annotation into noise — degrading, not improving, the gate.

## Decision

Use **two complementary gates**, picked by the *shape* of the blocking call:

1. **Static scanner (`pillar2-scan.sh`) — unchanged — for DIRECT primitives.**
   Where "on a render path = suspect" holds (cpr / ifstream / popen / system /
   SQLite / `.lock()`), the compile-/PR-static scanner remains the gate. No
   wrapper-awareness is added.

2. **Runtime UI-thread-affinity guard — for the WRAPPER chokepoints.** A tiny
   low-layer registry (`Source/Core/include/UiThreadAffinity.h`: `SetUiThread()`
   called once at UI init, `IsOnUiThread()`, `WarnIfOnUiThread(context)`) is queried
   at the three blocking wrapper chokepoints every violator funnels through:
   `ConfigManager::WriteConfigJson` (the universal config-write funnel),
   `ConfigManager::LoadPersistentViewsFromDisk`, and `P4RunCommand` (after its
   `P4RunOverride` test-seam). A call that *executes on the UI thread* trips the
   guard; an off-thread call never does.

The registry is a standalone low-layer util (not `AppController::IsOnUiThread()`,
which is private to a layer *above* the Config/P4 wrappers) so both layers can
query it without a layering inversion. It mirrors `AppController`'s publish-once
discipline (`uiThreadId_` set once before any worker spawns) and **fails open**
during startup (before `SetUiThread()` runs) so a pre-registration call never
false-fires.

## Why this is the right kind of gate (the trade-off)

For a function that is *legitimately called both on and off the UI thread*, the
runtime guard has **zero false positives** (an off-thread call cannot trip it) and
**zero annotation burden** (no 55-site sweep, no grandfather wall). It catches the
bug **where it actually manifests** — the wrapper *executing* on the UI thread —
under the existing bucket-E + ASan/UBSan harness.

The accepted cost: the runtime guard is **coverage-dependent**. It only fires when
a test / bucket-E / sanitizer run actually drives the violator path on the UI
thread — weaker than a compile-/PR-static gate, which fails before any run. We
accept this because a static gate *provably cannot* see lexical scope for a
both-on-and-off-thread wrapper (the 55-site swamp), so runtime is the only gate
that can exist for this class without degrading. Slice 1b's "drive-the-path test
first" discipline (add a bucket-E/scenario test that makes the guard fire pre-fix)
converts each violator from theoretical to actually-gated.

## Consequences

- **Warn-first, not hard-assert (current state).** `WarnIfOnUiThread` is a
  `LOG_ERROR`-once, no-abort guard, not a `SMATCHET_ASSERT` (which does not exist in
  the tree). A hard assert added before the live violators were fixed would crash
  any existing test that drives a violator path on the UI thread — breaking the
  build the gate protects. Warn-first is purely additive.
- **Hardening to a debug/sanitizer-build assert is a deliberate later step**, taken
  only *after* Slice 1b clears the live violators. As of this ADR the last residual
  is #611 site #7 (the per-frame `RefreshTrackerAppendCache` disk read), routed to
  a `perf-detective` async conversion; the warn → debug-assert promotion is blocked
  on it.
- The static scanner and the runtime guard are **non-overlapping** — the scanner
  owns direct primitives, the guard owns the wrappers — so neither needs to grow to
  cover the other's class.
- New blocking **wrapper** chokepoints (a future universal funnel for some sync I/O)
  should add a `WarnIfOnUiThread` call rather than a scanner regex; new **direct**
  primitives extend the scanner. The decision rule for "which gate" is the call's
  shape (direct primitive vs project wrapper), not the subsystem.
