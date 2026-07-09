# Quality Pillars

> Lifted from [`AGENTS.md`](../../AGENTS.md) § Quality Pillars per [`docs/plans/shipped/agents-md-reduction.md`](../plans/shipped/agents-md-reduction.md). AGENTS.md retains a load-bearing stub naming the pillars + their owning agents (with **UX Pillars** + **Engineering Pillars** sub-anchors) so external `AGENTS.md § <subsection>` references — including legacy `§ UX Pillars` — continue to resolve. Renamed from `ux-pillars.md` when DRY was added as an Engineering Pillar (ADR-0015). Edit this file directly — no parallel copy in AGENTS.md.

Five north-star quality invariants in two sub-groups. **UX Pillars** (1-4) are user-facing: 1-3 are **enforceable** (agents auto-fail PRs that violate them); 4 is **aspirational** today (flagged in `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` category `process`, not a merge block, until the supporting infrastructure lands). **Engineering Pillars** (5: DRY) govern code-maintainability and are enforced like UX 1-3.

## 1. Performance — sustain ≈ 144 Hz

**Pillar 1**: sustained 144 Hz on the UI thread. Frame budget = **6.94 ms** (`1000 / 144`) under representative load.

**Enforceable invariants:**
- Steady-state mean per-frame UI work `≤ 6.94 ms` measured by `perf.snapshot` over a representative scenario.
- 100 Hz floor: no single frame > **10.0 ms** in normal operation; >10.0 ms outliers are spike-tracked at p99.
- `perf-detective` regression-fails any commit that lifts steady-state mean above budget on the same scenario.
- `spike-hunter` regression-fails any commit that introduces a new p99 > 10.0 ms on the UI thread under a previously-clean scenario.
- CI-only exception: `docs/perf/regression-policy.json` `perScenario` may raise a scenario's CI ceiling above 10.0 ms when the shared 2-core runner is proven the bottleneck (first: `cell-edit-burst` at 15.0, Issue #973) — the real-hardware floor is unchanged and each override must carry a rationale + tracking Issue.

**Tools**: `SMATCHET_UI_PERF_SCOPE("perf_temp:...")` markers per `agents/core/perf-instrument.md`; `perf.reset` → `scenario.run` → `perf.snapshot` loop per `agents/core/perf-measure.md`; `docs/guides/perf-workflow.md` for full ladder. **Baseline registry + delta gate** (Slice 1 of `docs/plans/shipped/pillar-1-2-perf-review-system.md`): `bash scripts/dev/perf-run.sh <scenario>` writes a fresh snapshot; `python scripts/dev/perf-compare.py <baseline> <fresh>` exits non-zero on regression beyond `docs/perf/regression-policy.json` thresholds. Baselines live at `docs/perf/baselines/<scenario>.<host>.json` (per-host per § D1 of the plan). Manage via `bash scripts/dev/perf-baseline.sh {list|init|bump}`.

## 2. UI never freezes — predictable visual cue if it must

**Pillar 2**: zero manual verification steps; the UI thread never blocks longer than 100 ms without a visible cue. Any operation estimated **> 100 ms** moves to a worker thread. Synchronous I/O (HTTP, SQLite, p4, filesystem, blocking lock) reaching the UI thread = **code-review CRITICAL**.

**Visual cue contract** for the rare unavoidable blocking case:
- Spinner or progress widget appears within **100 ms** of op start.
- Cancelable when the underlying op supports it (HTTP, p4, long-running queries).
- Modeless when possible; modal only when the result is required to proceed.
- No silent waits — the user is never left guessing whether the app is alive.

**Enforceable invariants:**
- `code-review` flags any new synchronous call to `cpr`, `SQLite::Database`, `p4 …`, `std::ifstream`-on-disk, or `std::mutex::lock` from a function reachable from `ImGui::*`-frame as Critical.
- `spike-hunter` enforces UI-thread p99 < 100 ms on the standard scenario; cue-less hitches above that line block merge.

**Worker-thread hand-off**: post results back to the UI thread via `MainThreadDispatcher` (`Source/Core/include/MainThreadDispatcher.h`); never touch ImGui state directly from a worker.

## 3. Never crash

**Pillar 3**: the project must terminate cleanly under all observed inputs. Crashes in dev block the next merge until fixed; crashes in shipped builds are P0 regressions.

**Enforceable invariants:**
- **Pre-merge sanitizer build** mandatory on any PR that touches `Source/Core/` C++: `cmake --build --preset ninja-test-msvc` runs the doctest rig under ASan / UBSan (when toolchain supports it). `debug-detective` runs the sanitizer build for every crash-suspect investigation.
- **RAII enforced**: no raw `new` / `delete` outside the documented edge cases (sol2 user data, ImGui callback shims). Use `std::unique_ptr` + `make_unique`. `code-review` flags raw heap ops.
- **Bounds-checked**: every container index goes through `at()` / explicit length check; `cppcheck` `boundsError` / `arrayIndexOutOfBounds` blocks merge.
- **No silent UB**: dereferenced `nullptr`, unsigned wrap-around, signed overflow, use-after-free — all blocking. UBSan output during the regression gate is a fail.
- **Graceful degradation in ship builds**: assertions fire in dev (`assert(...)`); in ship builds the same condition logs `LOG_ERROR` and the calling function returns a safe default. The app never aborts on a recoverable bad state.

## 4. Accessibility — aspirational (locked scope)

**Pillar 4**: keyboard nav, font-size / zoom, WCAG AA contrast. No auto-fail gates today. Agents flag missing a11y to `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (category `process`) so it accumulates evidence; pillar hardens once the supporting infra lands.

**Locked in-scope (work on these when adjacent to current task):**
- **Keyboard navigation**: every actionable widget reachable without mouse. Tab order sane, focus indicators visible, `Ctrl+Shift+P` Command Palette as the keyboard entry point to every registered command.
- **Font size / zoom**: user-controlled `ImGuiIO::FontGlobalScale`, persisted in `smatchet_config.json`. Affects grid row heights, cell renderers, and modal sizing.
- **Color contrast**: WCAG AA minimum — 4.5:1 for body text, 3:1 for large text and UI components — on both default and dark themes. Theme audit before any palette change.
- **Visual-validation acceptance**: when no automated check (bucket-C screenshot diff, bucket-E ImGui-Test-Engine scenario) covers a visual change, the user is the verifier. See [`docs/agent-rules/ship-loops.md`](ship-loops.md) § Exceptions § Visual-validation exception for the loop-pause contract — the orchestrator must NOT commit+push an unvalidated visual change.

**Out of scope (deferred until a concrete user need):**
- Screen-reader compatibility. ImGui has no native a11y tree; wiring one is a multi-week effort. Defer.
- High-contrast / inverted-color themes beyond the WCAG AA floor.

**Why aspirational, not enforceable**: there is no automated check for "is this widget keyboard-reachable" or "does this palette meet WCAG AA contrast" today. Adding such checks is its own work-stream; pillars 1-3 already block merges where they matter most.

## 5. DRY — no new copy-paste (Engineering Pillar)

**Pillar 5** (the first **Engineering Pillar**; [ADR-0015](../adr/0015-dry-quality-pillar-duplication-gate.md)): no NEW copy-paste duplication enters first-party C++. Unlike UX 1-4, this governs code *maintainability*, not user-facing behaviour — but it is enforced like UX 1-3, delta-gated against `origin/develop`.

**Enforceable invariant (blocking):**
- `dup_audit.py --diff` (token-shingle + rolling-hash clone detector; identifier + literal normalized, so copy-then-rename is caught; min ~70 tokens / ~8 lines) flags any NEW cross-file copy-paste clone vs the merge-base. All existing duplication is grandfathered (snapshot `docs/high-integrity/dup-baseline.md`).
- **Blocking since 2026-06-21** (ADR-0015 calibration complete — graduated from the WARN-first phase): a `[dup] FAIL` fails `test-lint-rules.sh --diff` closed, and with it the required `Duplication scanner` CI context (`dup-scan.yml`); `pre-ship.sh` runs the same gate locally. Exempt a genuine new clone with a `SMATCHET_DEVIATION(rule=duplication; …)` marker on/above either occurrence.

**Double-edged-DRY guardrail (co-equal with the gate):** the gate flags **copy-paste only**, never structural similarity. An exemption is **cheap and preferred over abstracting across unrelated contexts** — `SMATCHET_DEVIATION(rule=duplication; reason=…; owner=…; revisit=…)` on/above the clone. A DRY-motivated refactor that introduces a shared helper **coupling two otherwise-independent subsystems is a code-review CRITICAL**, not an improvement. Standing exemptions: dual-target forward-decls, per-backend `*Client` boilerplate, generated code.

**Tools**: `agents/scripts/core/dup_audit.py` (`--diff` / `--scan-file` / `--baseline-md` / `--selftest`); baseline regen via `bash agents/scripts/project/test-lint-rules.sh --dup-baseline`.

## Agent ownership

| Pillar | Primary agent | Notes |
|---|---|---|
| 1. Performance | `perf-detective` (sustained), `spike-hunter` (intermittent), helpers: `perf-instrument`, `perf-measure` | See `docs/guides/perf-workflow.md`. |
| 2. UI never freezes | `code-review` (sync-on-UI sniff), `spike-hunter` (p99 enforcement), `debug-detective` (root-cause when a freeze ships) | UI-thread budget: any call reachable from `ImGui::*`-frame stack. |
| 3. Never crash | `debug-detective` (diagnose), `code-review` (RAII / bounds / nullptr review), `build-doctor` (sanitizer build gate) | Crashes block merge unconditionally. |
| 4. Accessibility | none today | Flag in backlog; reassess pillar hardening when keyboard-nav / zoom / contrast checks have automated test support. |
| 5. DRY (Engineering) | `code-review` (reviewer-of-record + exemption sign-off) | `dup_audit.py` delta-gate is blocking; duplication triage + the coupling-CRITICAL guardrail are the reviewer's. |
