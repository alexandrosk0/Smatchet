# Plan — Full vsync toggle (config · command · CLI · preference · env · live)

> **Status**: `shipped`
>
> **Slug**: `vsync-toggle` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

<!-- index-summary: Full vsync toggle exposed across persisted config, the config.set command (CLI/MCP/Lua/palette), a --no-vsync CLI flag, a Preferences checkbox, the SMATCHET_FPS_VSYNC env var, applied live via a Core VsyncControl hub. -->

## Context

vsync is hardcoded `glfwSwapInterval(1)` in both standalone boot paths (`main.cpp:509`,
`StandaloneAppBootstrap.cpp:468`). The only toggle today is the ad-hoc `SMATCHET_FPS_VSYNC` env var
added with the FPS-measure feature (#904) — gated, launch-only, no persistence, no in-app control.
Goal: a **first-class vsync toggle** reachable from every surface and applied **live**, not just at boot.

**Intended outcome — one sentence:** after this lands, vsync is a persisted `TrackerConfig` setting
toggleable live from a Preferences checkbox, the `config.set vsync` command (so CLI + MCP + Lua +
palette all reach it), a `--no-vsync` / `--vsync` launch flag, and the `SMATCHET_FPS_VSYNC` env var —
all funnelling through one Core `VsyncControl` hub the render loop reads each frame.

## Approach

A tiny GLFW-free Core hub is the live runtime source of truth; the persisted config is the durable
source of truth that seeds it at boot.

- **`Source/Core/include/VsyncControl.h` (new)** — `namespace smatchet::vsync`: `std::atomic<bool>`
  + `SetEnabled(bool)` / `Enabled()`. No GLFW (Core compiles into DX12 too). The render loop (which
  owns the GL context) reads `Enabled()` each frame and calls `glfwSwapInterval` only on change.
- **Persisted config** — `TrackerConfig::VsyncEnabled = true` + a `kBoolFields` entry
  (`vsync_enabled`) so Load/Save round-trips it. `ConfigManager::Load` seeds `VsyncControl` from it.
- **Live apply** — `RunFrameLoop` (main.cpp) tracks `lastAppliedVsync`; when `vsync::Enabled()`
  differs, calls `glfwSwapInterval(enabled?1:0)`. Same one-line apply added to the ephemeral/Unreal
  boot path's loop where applicable.
- **Surfaces all funnel through `VsyncControl` + persist where appropriate:**
  - **Preferences checkbox** (Appearance tab) → sets `d.cfg.VsyncEnabled` + `vsync::SetEnabled` (live) + `MarkPrefsDirty` (persisted on Save).
  - **`config.set vsync on|off`** → add `{"vsync","vsync_enabled","applies immediately"}` to the config-set table; `RunConfigSet` writes the config JSON AND calls `vsync::SetEnabled` so a running app applies live (not just next launch).
  - **`--no-vsync` / `--vsync` CLI flag** → `CliOverrides.HasVsync/Vsync`; applied to `VsyncControl` at boot (launch override, not persisted).
  - **`SMATCHET_FPS_VSYNC` env** → routed through `VsyncControl` at boot (replaces the FpsMeasure-gated copy); highest precedence.
- **Precedence at boot:** env `SMATCHET_FPS_VSYNC` > `--no-vsync`/`--vsync` flag > persisted config. Runtime: Preferences / `config.set` mutate `VsyncControl` live.

## Files to modify

1. `Source/Core/include/VsyncControl.h` (new) — atomic hub + Set/Get (header-only).
2. `Source/Core/include/Config/ConfigManager.h` — `bool VsyncEnabled = true;` on `TrackerConfig`; `bool HasVsync; bool Vsync;` on `CliOverrides`.
3. `Source/Core/src/Config/ConfigManager.cpp` — `kBoolFields` `{"vsync_enabled", &TrackerConfig::VsyncEnabled}`; seed `vsync::SetEnabled(cfg.VsyncEnabled)` at end of `Load`.
4. `Source/Core/src/Commands/Builtin/BuiltinCommands_Config.cpp` — config-set table entry; `RunConfigSet` calls `vsync::SetEnabled` for the vsync key (live apply).
5. `Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp` — "Enable vsync" checkbox in `DrawAppearanceUpdatesSection`/typography area (Appearance tab); sets cfg + `vsync::SetEnabled` + `MarkPrefsDirty`.
6. `Source/Standalone/StandaloneAppBootstrap.cpp` — parse `--no-vsync`/`--vsync` in `ParseStandaloneCli`; apply config+flag+env precedence to `VsyncControl`; the ephemeral/boot `glfwSwapInterval` reads `VsyncControl`.
7. `Source/Standalone/main.cpp` — boot precedence apply + `RunFrameLoop` live-apply loop; reconcile the existing `FpsMeasure` `SMATCHET_FPS_VSYNC` handling to route through `VsyncControl`.

## Existing utilities reused

- `ConfigManager` `kBoolFields` `FieldDesc<bool>` table (Load/Save round-trip) — the field plugs in with one row.
- `BuiltinCommands_Config` config-set table + `RunConfigSet` — one row makes the key reachable from CLI/MCP/Lua/palette ("register once, surface everywhere").
- SmatchetUI theme/density live-apply pattern (`lastAppliedTheme_` in `SmatchetUI::Draw`) — mirrored for `lastAppliedVsync` in the render loop.
- `SmatchetPreferencesUi_Local` Appearance tab section helpers (from shrink-over-100-line-functions Batch A) — the checkbox slots into a section helper.
- `ConfigManager::CliOverrides` + `ParseStandaloneCli` — the `--no-vsync` flag follows the existing `--mcp-allow-remote` bool-flag pattern.

## UX Pillar callouts

- **Pillar 1 (perf)**: this IS a perf control. `glfwSwapInterval` is called only on change (not per frame); the render loop's per-frame check is one atomic load + an int compare. vsync-off raises CPU/GPU (uncapped) — that is the user's explicit choice, surfaced honestly.
- **Pillar 2 (UI never freezes)**: no new sync I/O; `config.set` already writes config off the hot path.
- **Pillar 3 (never crash)**: `glfwSwapInterval` is safe to call any frame with a current context; the atomic hub is lock-free.
- **Pillar 4 (accessibility)**: the Preferences checkbox is keyboard-navigable like the sibling Appearance toggles.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

Touches `Source/Core/`. **PR-fast CI**: `app-cold-start` (boot seeds VsyncControl) + `idle` (render-loop per-frame check is the only steady-state addition — must show no regression). Pillar-2 scanner N/A (no sync I/O). Dispatcher drain N/A. Marker inventory N/A (no new perf scope). Pre-push: run `idle` scenario before/after; the per-frame atomic-load + compare must not move the number.

## Risks / non-goals

**Risks:**
- **Strict-zone lint** (Config/, Commands/) — extracted code obeys RAII/logging. → run `test-lint-rules.sh --diff` before push.
- **Live config.set apply** — `config.set vsync` from the CLI/MCP targets a *running* instance via the command bus, so `vsync::SetEnabled` there applies to that process live; the file write covers next launch. The standalone-only render loop is the consumer; DX12/Unreal has its own present path (VsyncControl still toggles, the Unreal host honours it where it manages the swapchain). → document that DX12 swapchain vsync is host-owned.
- **Precedence confusion** — env vs flag vs config. → documented order (env > flag > config) + a one-line LOG at boot stating the resolved source.
- **Dual-target** — `VsyncControl.h` is GLFW-free so it compiles into `SmatchetCore_DX12`; only the `glfwSwapInterval` call sites stay in Standalone. → dual-target build gate.

**Non-goals:**
- Per-monitor / adaptive (G-Sync/FreeSync) vsync modes — just on/off (`swapInterval` 0/1).
- A DX12-side present-interval rewrite — Unreal owns its swapchain; VsyncControl exposes intent, host honours it where it can.
- Removing `SMATCHET_FPS_VSYNC` — kept (reconciled through VsyncControl) for headless/measure use.

## Verification

- **Bucket A (ctest)**: `VsyncControl` Set/Get round-trip; `ConfigManager` vsync_enabled Load/Save round-trip (extend the config round-trip test); `config.set vsync` writes `vsync_enabled` + flips `VsyncControl`.
- **Bucket E**: N/A (no new window/interaction; the checkbox rides the existing Appearance-tab smoke).
- **Build gate**: dual-target `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Lint gate**: `test-lint-rules.sh --diff origin/develop` (strict Config/ + Commands/).
- **Live smoke**: launch standalone, toggle the Preferences checkbox → FPS uncaps/caps live (verify via `SMATCHET_FPS_MEASURE_SECONDS`); `--no-vsync` launch + env both honoured (boot LOG states resolved source).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test the precedence + live-apply design before finalising; record the outcome.
- **Manual residue**: none designed.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray vsync refs.

- Adaptive/per-monitor vsync, frame-rate-cap (target-FPS limiter) — separate feature.
- DX12/Unreal swapchain present-interval control — host-owned; only intent is exposed.

## Implementation log
- `6f76e7c2` · full vsync toggle in one slice: `VsyncControl.h` hub + `vsync_enabled`
  config field + `config.set/get vsync` (live apply) + Preferences "Display →
  Enable vsync" checkbox + `--vsync`/`--no-vsync` flags + env precedence +
  `RunFrameLoop` live apply + both boot paths seeded via `SeedFromBootSources`.

## Deviations from plan
- **No `ConfigManager::Load`-side seeding** (plan §Files-to-modify item 3 said
  "seed `vsync::SetEnabled(cfg.VsyncEnabled)` at end of `Load`"): `Load()` runs
  on every sync and after any `config.set` cache invalidation, so a Load-side
  seed would stomp a live-but-unsaved Preferences toggle back to the on-disk
  value. Both standalone boot paths seed explicitly once instead
  (`SeedFromBootSources` in `BootApplication` + the bootstrap `Initialize`).
- **`config.set vsync` Bucket-A test dropped** — command-path coverage needs the
  full CommandRegistry harness, the same boundary `SingleClickEditConfig.test.cpp`
  documents as out of bounds for the pure-logic rig. Covered by the CLI smoke
  (`--spawn` round-trip) instead; on/off normalisation + live-apply hook verified there.
- **`config.get` exposure added** (plan omission): the get-side projection is a
  separate table from the set-side `CfgKey` list; without a `vsync` row,
  `config.get --key=vsync` returned not-found. One line added.
- **Boot-path note**: the visible-window boot creates its GLFW context before
  CLI parse + config Load, so `BootInitGlfwAndWindow` keeps an initial
  `glfwSwapInterval(1)` and `BootApplication` re-applies the resolved value
  immediately after `Load(cli)`.

## Verification (actual)
- **Bucket A**: `tests/Core/VsyncControl.test.cpp` (4 cases / 15 asserts — hub
  round-trip + all three precedence sources) + `ConfigManager` round-trip
  extended with `VsyncEnabled` (110 asserts green). Full `ctest` suite: 2/2 pass.
- **Build gate**: dual-target `SmatchetStandalone` + `SmatchetCore_DX12` green
  (warnings-as-errors).
- **Lint gate**: `test-lint-rules.sh --diff origin/develop` all PASS (strict
  Config/ + Commands/ zones included).
- **CLI smoke**: `cmd config.set --key=vsync --value=off|on --spawn` →
  `written:true, hint:"applies immediately"`; `--value=banana` →
  validation-error; `config.get --key=vsync` round-trips; `vsync_enabled`
  persisted in `smatchet_config.json`.
- **Live smoke** (FPS measure, 144 Hz display): env `SMATCHET_FPS_VSYNC=0` →
  262 fps avg (uncapped, source logged `env SMATCHET_FPS_VSYNC`); `--no-vsync`
  → 197 fps (source `CLI flag`); default → 129 fps ≈ refresh-capped (source
  `persisted config`). Boot LOG line confirmed for all three sources.
- **Manual residue**: Preferences-checkbox live flip is visual-validation
  (Pillar-4 exception) — verified interactively at ship time.
