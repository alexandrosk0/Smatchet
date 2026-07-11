# User-visible error-surface inventory

> Baseline for error-message quality passes. Scope: text that reaches the user's
> eyes in the UI (banners, toasts, chat strips, cell feedback, modals) — NOT
> `LOG_*`, CLI stdout, or MCP protocol responses. Quality bar per surface:
> does the message say **what failed**, **why**, and **what to do next**,
> without leaking internals (tokens, full paths, raw exception text)?
> First pass: `docs/plans/error-message-quality-pass.md` (2026-07). Update this
> table when a surface's shape changes.

## Redaction infrastructure

- `smatchet::ai::pure::RedactProviderErrorBody` (`AiErrorRedact.cpp`) — strips
  Bearer/Basic/api-key/`sk-`… tokens, length-caps. Applied on the AI-provider
  error path and (since the first pass) the connectivity-banner technical
  suffix. NOT yet applied to tracker `.Error` strings generally — safe today
  because tracker auth lives in headers that never echo into those strings, but
  any future fold of a response *body* into `.Error` must go through a scrub.
- `RedactUrlForLog` / `RedactHttpBodyForLog` (`TrackerHttpUtils.cpp`) — log-only.

## Surfaces

| Surface | Where built → shown | Message origin | State after first pass |
|---|---|---|---|
| Grid field-edit banner/toast (`gridEditError`) | `SmatchetGridFieldEditPipeline.cpp` → "Active Project" toast | tracker `.Error` (Jira/Linear/Plane mutation layers) | Jira paths now route `ExtractJiraErrorMessage` (errorMessages[]/errors{} parsed; **raw body splice removed**); fallback localized |
| Connectivity banner (`TrackerConnectivityBannerForUi`) | `ConnectivityMonitorService.cpp` → dashboards/grid/mobile + toast | headline literals + raw `fetchError` suffix | headline unchanged (good); technical suffix now scrubbed via `RedactProviderErrorBody`; 401/403 page-fetch errors carry the check-credentials hint (was log-only) |
| Sync warning (`lastTicketSyncWarning_`) | `TicketSyncService.cpp` | "Showing cached issues — …: " + fetchError | parse-failure fetchError now fixed text ("unreadable response"), detail logged |
| Offline queue errors | `OfflineQueueService.cpp` → grid banner | fixed literals + `ex.what()` | "Cache is not initialized." → actionable; `ex.what()` off the UI (logged) |
| Config load failure | `ConfigManager` (was **silent** — log-only, defaults loaded) | — | one-shot startup toast via `TakeStartupConfigWarning()` (file name only) |
| App-update check | `SmatchetUI.cpp` + `AttachmentAppUpdateService.cpp` → "Updates" toast | `ex.what()` / literals | localized what+next message; exception detail logged |
| AI test-connection (Prefs → Assistant) | `SmatchetPreferencesUi_Assistant.cpp` | redacted provider error; raw `ex.what()` on internal errors | internal-error branch now localized + logged |
| Whisper test-connection (Prefs → Whisper) | `SmatchetPreferencesUi_Whisper.cpp` | transport/HTTP literals; raw `ex.what()` | same fix |
| AI chat error strip | `AiAssistantController.cpp` → chat panel | `RedactProviderErrorBody` output | already best-in-class; unchanged |
| Bug-report submit banner | `BugReportService.cpp` → `SmatchetBugReportUi.cpp` | relay `error` field; `ex.what()`/parseErr on bad responses | parse/exception branches → fixed retry message, detail logged |
| Bulk import/export | `SmatchetBulkTicketsUi.cpp` | `r.Error`, bare `"failed"`, full file paths | `"failed"` → actionable localized text; file errors show the file **name** only |
| Lua console/script errors | `LuaConsolePlugin.cpp` → console + grid toast | raw Lua interpreter text (incl. script path) | unchanged — raw detail is correct for a script console; flagged: the same string also reaches the global toast |
| Ticket-change / membership toasts | `TicketChangeDiffPure.cpp` | pure English sentence builders | unchanged (functional, not error text) — localization is part of the missing-French backlog |

## Known remaining gaps (next pass candidates)

1. **No tracker-side redaction layer** — `.Error` → toast has no scrub choke
   point; add one before any code folds response bodies into `.Error`.
2. **Plane backend has no error-body extractor** (Jira and Linear now do).
3. **Lua error strings reach the global toast** with full script paths — fine in
   the console, worth trimming at the toast boundary.
4. **Tracker-layer message localization** — `.Error` strings are English by the
   README contract ("backend error details shown as-is"); revisit if a
   translation pass ever wants tracker-built prefixes localized.
5. **No in-Preferences tracker test-connection** — auth failures surface only
   via the startup banner / first sync; a dedicated "Test connection" button
   with the 401-hint would shorten the feedback loop.
