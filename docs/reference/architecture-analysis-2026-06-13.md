<!-- index-summary: dated architecture snapshot — 6-layer model, subsystem map, ADR index, adversarially-verified risk register (8 load-bearing risks, 2 hold CRITICAL) -->
# Smatchet architecture analysis — 2026-06-13 (snapshot)

**Status:** point-in-time snapshot (a *reference*, not a living index — superseded by code drift; re-run to refresh).
**Method:** multi-agent Workflow fan-out (per-dimension reader agents → synthesis) **plus** a two-batch adversarial-verify pass over the load-bearing risk claims (independent skeptics, REFUTE-prompted, every verdict cited to live `file:line`).
**Scope:** whole tree as of `origin/develop` (HEAD `f14e5766`).

> **Why the verify pass matters.** The reader-agent synthesis produced a risk register that was **half wrong**: 4 of 8 load-bearing risks were refuted or overstated, almost all because a reader trusted a stale **doc-comment / README / ADR prose** over the live code it described. Treat the § Risk register (verified) as the authoritative output and the § Architecture overview as synthesis — see § Provenance & confidence for the per-section trust level.

---

## Provenance & confidence

| Section | Confidence | Basis |
|---|---|---|
| § Risk register (verified) | **HIGH** | Each verdict read the cited live `file:line` this/last session; checkpoints in `build/arch-analysis/verify*.md`. |
| § Architecture overview — structural facts with `file:line` | **HIGH** | Line-verified during the adversarial passes (e.g. AppController fan-in, `UiDrawSession`, role interfaces, ADR-0020 conduit). |
| § Architecture overview — layer graph, counts, edge list | **MEDIUM** | Reader-agent fan-out synthesis; structurally sound, not exhaustively line-verified. LOC/file counts are approximate. |

Where a number is synthesis-only it is marked *(approx.)*.

---

## Architecture overview

### Scale & targets

- ~142k LOC C++14, ~571 source files *(approx.)*; C++14 is a **hard** floor (Unreal compat) — banned: `string_view`, `optional`, `variant`, structured bindings, `if constexpr`.
- **Four build targets** from one `Source/Core`:
  - `SmatchetStandalone` — GLFW/OpenGL desktop exe.
  - `SmatchetCore_DX12` — Unreal static lib (`EXCLUDE_FROM_ALL`), consumed by `Source/UnrealPlugins/SmatchetImGuiPlugin` over a hand-written C ABI.
  - `SmatchetMobile` — Android EGL/GLES3 `.so`.
  - `SmatchetCore_PosixCheck` — compile-only portability gate.
- **Three INTERFACE shim libs** (`SmatchetCoreInterface`, `…McpShim`, `…AiShim`) pin compile defines so macro-gated struct layouts (notably `UiDrawSession`) stay parity-consistent across targets ([ADR-0002](../adr/0002-plugin-shim-link-discipline.md)).
- Dependencies via CMake `FetchContent` at pinned commit SHAs; Android OpenSSL is tarball-SHA-256-pinned (see risk #8).

### The 6-layer model

Top → bottom. **Not a clean DAG** — it is hub-and-spoke around `AppController` with cyclic back-edges.

```
┌────────────────────────────────────────────────────────────────────┐
│ UI             ui-grid-panels (TicketGrid, Smatchet*Ui)             │  top
│                ui-host (ImGui host, theme, dockspace, fonts)        │
├────────────────────────────────────────────────────────────────────┤
│ ORCHESTRATION  AppController  (partial-class god-object, pImpl)     │
│                command-system (one CommandRegistry → 5 frontends)   │
├────────────────────────────────────────────────────────────────────┤
│ BACKEND-CLIENT tracker-backend (ITrackerBackend + 6 role ifaces)   │
│                ai-assistant (OpenAI/Anthropic/Ollama/DeepSeek)      │
├────────────────────────────────────────────────────────────────────┤
│ DOMAIN-SERVICE sync · persistence (SQLite cache) · offline-queue   │
├────────────────────────────────────────────────────────────────────┤
│ INFRA          threading (UiThreadAffinity) · config · lua (sol2)  │
│                plugins (IPlugin, MCP)                               │
├────────────────────────────────────────────────────────────────────┤
│ BUILD          3 INTERFACE shims · 4 targets · FetchContent (SHA)  │  bottom
└────────────────────────────────────────────────────────────────────┘

Back-edges that break the DAG:
  • UI ←→ ORCHESTRATION   (UI draws from AppController; AppController owns UI host callbacks)
  • DOMAIN-SERVICE → ORCHESTRATION   service headers re-export AppController types
        (OfflineQueueService.h, TicketSyncService.h #include "AppController.h")
  • DOMAIN-SERVICE → INFRA via AppController.h → LocalCacheManager.h → SQLiteCpp
        (the ADR-0020 accepted residual — see risk #3)
```

### Subsystem map (key nodes)

- **AppController** — the hub. Partial-class "god-object" split across ~9–10 `.cpp` files (`AppController_*.cpp`), ~1110-line header, ~150 methods, owns 10+ `unique_ptr` subsystems + ~10 mutexes; a `pImpl` isolates sol2 from its 113 includers. Header `AppController.h` is `#include`d by **113 files** (108 `.cpp` + 5 `.h`). Decomposition is underway and principled (`OfflineQueueService` / `TicketSyncService` / `LuaAutomationHost` already extracted behind ISP `*Deps` interfaces with test fakes). Tracked as debt (`debt.md`, deep-audit 2026-05-28).
- **Tracker backend** — `ITrackerBackend` split into **6 role interfaces** (`ITrackerIssueReader`, `ITrackerConnectivity`, `ITrackerFieldCatalog`, `ITrackerIssueMutations`, `ITrackerCollaboration`, `ITrackerActivity` — the 6th added by [ADR-0021](../adr/0021-itracker-activity-sixth-role.md)). Concrete clients `JiraClient` / `PlaneClient` / `GitHubClient` behind the agnostic interfaces; backend-specific code must not leak into the shared interfaces. Field flow is strictly **catalog → parser → payload**.
- **Command system** — one `CommandRegistry` feeds five frontends: CLI, Command Palette, MCP, Lua, Scenarios ([ADR-0010](../adr/0010-light-profile-feature-gated-command-registry.md) unified-command + feature-gate).
- **UI** — `UiDrawSession` is a **687-line mutable struct** (`SmatchetUiSession.h:156–843`; 860-line header) instantiated as a single file-scope global `UiDrawSession g_ui;` (`SmatchetUI.cpp:64`, `extern` at `:857`) — AI/Whisper at 260–343, panes 525, offline-queue 651, JQL 398–441. Highest-churn shared mutable state in the UI layer.
- **Sync / persistence / offline** — `LocalCacheManager` (SQLite), `OfflineQueueService` (pending-create / pending-field-edit replay, dead-letter), `TicketSyncService`, `BackendAuditTrail`. The `ISyncCache` seam ([ADR-0020](../adr/0020-sync-cache-seam-include-not-link-purity.md)) is an include-not-link purity boundary. Offline conflict resolution per [ADR-0016](../adr/0016-offline-scalar-edit-conflict-detection.md).
- **Per-pane backend isolation** — each grid pane carries a `GridLiveContext` + `backend_key` ([ADR-0018](../adr/0018-multi-grid-pane-contexts.md)); backend swaps use a `shared_ptr` atomic-graveyard latch ([ADR-0012](../adr/0012-shared-ownership-active-tracker-backend.md)).
- **Threading** — `UiThreadAffinity` invariant ([ADR-0019](../adr/0019-runtime-thread-affinity-assert-for-wrapper-sync-io.md)); background work goes through `LaunchBackgroundTask` (joined, never detached — see risk #4).

### Cross-cutting concerns

- **Logging** — `LOG_{DEBUG,INFO,WARN,ERROR,TRACE}` only (no `printf`/`std::cerr`); two named exceptions (pre-logger-init in `Source/Standalone/`, CLI stdout product output).
- **HTTP** — tracker traffic only through `TrackerHttpClient` / `TrackerHttpUtils` (retry, error classification, `NetworkUsageTracker`); TLS peer/host verify stays ON project-wide (`TrackerHttpUtils.cpp:122-131`).
- **RAII** — no raw `new`/`delete`; `unique_ptr` + `make_unique`.
- **Writes** — issue creates / field edits enqueue through `OfflineQueueService` + emit a `BackendAuditTrail` begin/result pair.

---

## Risk register (verified)

Eight load-bearing risks were flagged by the reader-agent synthesis and then adversarially verified in two batches. Verdict legend: **CONFIRMED** (holds as claimed) · **OVERSTATED** (real but milder/mitigated) · **REFUTED** (claim wrong, usually a stale doc-comment).

| # | Risk | Original | Verdict | Final grade |
|---|---|---|---|---|
| 1 | `AppController` god-object fan-in (113 includers) | CRITICAL | **CONFIRMED** | CRITICAL |
| 2 | `UiDrawSession g_ui` — 687-line global mutable struct | CRITICAL | **CONFIRMED** | CRITICAL |
| 3 | SQLite leaks into the header include graph | HIGH (ADR-violation) | **OVERSTATED** | accepted residual (ADR-0020) |
| 4 | `AiPrefsTestConnection` "detached thread" | HIGH | **REFUTED** | none (joined; doc-comment stale) |
| 5 | DeepSeek "silent provider clamp" | HIGH | **REFUTED** | LOW (display-only divergence) |
| 6 | C-ABI manual versioning | HIGH | **OVERSTATED** | MEDIUM (backlogged) |
| 7 | Lua HTTP / TOFU fetch | HIGH | **REFUTED** | LOW (console-only SSRF, backlogged) |
| 8 | Android OpenSSL unpinned | HIGH | **REFUTED** | none (more-pinned than baseline) |

**Score: 2 of 8 hold (both CRITICAL); 2 downgraded; 4 refuted.**

### Held — CRITICAL

**#1 — AppController god-object fan-in.** `Source/Core/include/AppController.h` is `#include`d by **113 files** (108 `.cpp` + 5 `.h`); it re-exports types so service headers depend back on it (`MainThreadDispatch.h:30`, `OfflineQueueService.h:32`, `SmatchetUiSession.h:3`). Any header change recompiles a third of the tree and is the dominant merge-conflict / reasoning bottleneck. Mitigation in flight (ISP `*Deps` extraction); full retirement is multi-PR. Tracked in `debt.md` (deep-audit 2026-05-28).

**#2 — `UiDrawSession g_ui` global.** A single 687-line mutable struct (`SmatchetUiSession.h:156–843`) instantiated file-scope (`SmatchetUI.cpp:64`) holds AI, panes, offline-queue, and JQL UI state together. Shared mutable global → aliasing / lifetime / test-isolation hazard and the highest-churn UI state. No ADR sanctions it; decomposition is open work.

### Downgraded

**#3 — SQLite in the include graph → accepted residual.** Real: `OfflineQueueService.h:32` and `TicketSyncService.h:20` `#include "AppController.h"`, and `AppController.h:41` → `LocalCacheManager.h:2` → `<SQLiteCpp/SQLiteCpp.h>`, so a sync TU transitively pulls SQLite. But this is **not** an undocumented ADR violation — [ADR-0020](../adr/0020-sync-cache-seam-include-not-link-purity.md):22 explicitly accepts it as the deferred residual of the `ISyncCache` include-not-link seam (the structural fix is relocating the queue/dead-letter result structs off `AppController.h` into `Sync/OfflineQueueTypes.h`, already a `debt.md` sub-task). Reclassified from "HIGH ADR-violation" to **accepted residual debt**.

**#6 — C-ABI manual versioning → MEDIUM (backlogged).** The DX12/Unreal boundary (`SmatchetImGuiHostC.h`) is **opaque-handle `void*` + flat C primitives only** — no struct layout crosses, so silent-layout-UB is structurally impossible there. Two live staleness signals exist but neither hard-fails: `WarnIfPackagedLibsAreStale()` (`SmatchetImGuiPlugin.Build.cs:234-306`) emits a UBT *warning*, and `SmatchetHost_GetBuildTag()` (`SmatchetImGuiHostC_Diagnostics.cpp:19-28`) is *logged* every `StartupModule()` (`SmatchetImGuiPluginModule.cpp:99-100`) but never compared to an expected constant. Residual = "warning vs hard-fail" only. Backlogged (`debt.md` 2026-06-13, `c-abi-staleness-soft-fail`).

### Refuted / near-refuted

**#4 — `AiPrefsTestConnection` detached thread → REFUTED.** The code uses `app.LaunchBackgroundTask(...)` (`AiPrefsTestConnection.cpp:229-231`), which is **joined, never detached** (documented `AppController.h:1282-1287`). The risk originated from a stale doc-comment `AiPrefsTestConnection.h:27` ("Spawns a detached worker thread") that already carries a DRIFT WARNING at `:32`. No threading risk.

**#5 — DeepSeek silent provider clamp → REFUTED (LOW residual).** `AiResolveProvider` (`SmatchetAiAssistantUi_detail.h:55-67`) has no `case 4:`, so a `DeepSeek` (`=4`, `AiTypes.h:10-15`) display lookup falls through to `OpenAi` — but this is a **display-string divergence only**; the functional path `ProviderFromConfig` (`AiAssistantController.cpp:37-54`) handles `case 4: return AiProvider::DeepSeek` correctly, and the clamp is a documented intentional default (`detail.h:52-54`). Not a correctness risk; minor UI polish (a real `case 4:` would remove the divergence — flagged as a background task).

**#7 — Lua HTTP / TOFU → REFUTED (LOW console-only SSRF, backlogged).** There is **no** general HTTP binding in the Lua API (`AppController_LuaBindingsCore.cpp:307-403` `InitLuaCore` exposes no `http`/`fetch`/`curl`/`net`), and the only outbound path uses standard system-CA TLS, not TOFU (`SmatchetFieldIconRender.cpp:181-200` `HttpGetBinary` = `cpr::Get` with no `SslOptions`). Residual LOW: the console sandbox doesn't block `imgui`, so `imgui.image('https://…')` from a console snippet reaches `HttpGetBinary` — an SSRF shape, but local-console-only and the body is decoded to a texture (512 KB cap) and **never returned to Lua** (no exfil channel); background/MCP Lua states can't reach `imgui` at all. Backlogged (`debt.md` 2026-06-13, `lua-imgui-image-url-allowlist`). Origin: a `SmatchetHooks.lua:39-43` doc-comment mentioning "https URL (fetched…)" misread as a general HTTP binding.

**#8 — Android OpenSSL unpinned → REFUTED (more-pinned than baseline).** OpenSSL for Android is pinned to 3.5.6 with a hardcoded SHA-256 in **both** the build script (`scripts/mobile/openssl/build-android-openssl.sh:22-32,102-104`) and CI (`build-and-test.yml:974-975,1031`), with fail-closed `sha256sum` verification. System/NDK-sysroot OpenSSL fallback is **banned** at the CMake level with `message(FATAL_ERROR)` (`cmake/SmatchetThirdParty.cmake:120-137`, regression-guard for Issue #1068), and a dedicated merge-blocking security gate (`mobile-security.yml` + `test-mobile-security.sh:62-146`) PCRE-asserts the marker is bound to `FATAL_ERROR` not `WARNING`. Strictly *more* controlled than the GIT_TAG-pinned FetchContent deps. No risk.

---

## Meta-lesson

Across both verify batches, **4 of 6 wrong/overstated calls traced to a stale or misread doc-comment / README / ADR prose** (`AiPrefsTestConnection.h:27`, `detail.h` clamp note, `SmatchetHooks.lua:39-43`, the ADR-0002 prose for #6). The merge-gate rule *"never trust an annotation blindly — read the cited code"* generalizes from CodeRabbit annotations to **all source comments and docs** during analysis. A risk register without an adversarial line-verification pass would have shipped 6 bogus/inflated CRITICAL/HIGH items. **Any future architecture or audit pass must verify load-bearing claims against live code before grading them.**

---

## Gaps / not covered

- Performance (Pillar 1, ≤6.94 ms steady-state) was not profiled here — see `docs/guides/perf-workflow.md`.
- Accessibility (Pillar 4) is backlogged project-wide, not assessed.
- The synthesis edge list (~60 edges) and LOC/file counts are reader-agent figures, not exhaustively line-verified (see § Provenance).
- ADR coverage references the decisions touched by the risk analysis; it is not a full ADR audit.

## Checkpoints

Per-dimension reader output and per-risk verdicts live under `build/arch-analysis/` (gitignored scratch): `_synthesis.md`, per-dimension `*.md`, batch-1 `verify-*.md`, batch-2 `verify2-*.md`.
