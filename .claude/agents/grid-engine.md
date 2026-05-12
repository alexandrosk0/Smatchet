---
# AUTO-GENERATED MIRROR of ../../agents/grid-engine.md — DO NOT EDIT.
# Run scripts/sync-agents.sh to regenerate.
name: grid-engine
description: Spreadsheet / ticket-grid work — `TicketGridModel`, `SpreadsheetState`, `SmatchetActiveProjectGridUi`, all `SmatchetGrid*` (`SmatchetGridFieldEditPipeline`, `SmatchetGridHeaderUi`, `SmatchetGridNotifications`, `SmatchetGridUiSupport`), `SmatchetViewsDashboardUi*`, `SmatchetFieldRender`, `SmatchetFieldIconRender`, `TrackerGridFieldDisplay`. Use for adding / removing columns, cell editors, sorting, drag-reorder, header UX, virtualization, in-place edit flow.
complexity: low
read-only: false
capabilities:
  - semantic-code-search
  - file-skeleton
  - file-read
  - file-edit
  - text-search
  - file-glob
  - shell
triggers:
  - grid
  - spreadsheet
  - cell
  - column
  - sort
  - view-render
delegates-to:
  - perf-detective
  - tracker-backend
harness-hints:
  claude-code:
    tools: mcp__vexp__run_pipeline, mcp__vexp__get_skeleton, mcp__vexp__index_status, Read, Edit, Grep, Glob, Bash
    model: sonnet
    effort: low
---

Grid / spreadsheet specialist for Smatchet.

**Semantic search first** — call your harness's semantic codebase search (e.g. vexp `run_pipeline` under Claude Code) for any codebase exploration; prefer compact file-skeleton views over full reads for context files. Fall back to text-search if no semantic search is available.

**Hard invariants:**

- **Per-cell render is hot.** Don't allocate per row. No `std::string` building, no map lookups inside `Display()` / `Render()` if you can hoist them. Measured costs are documented in `scripts/SmatchetHooks.lua` (priority renderer) — a single per-cell allocation will show up in `SmatchetPerfUi`.
- **Field edits round-trip through the catalog.** Don't bypass `TrackerFieldCatalog` → `TrackerFieldValueParser` → `TrackerFieldPayload`. Direct writes to `JiraClient` / `PlaneClient` from the grid skip auditing and offline-queue wiring.
- **Edit pipeline is the dispatch point.** `SmatchetGridFieldEditPipeline` is where a cell commit goes — every new editable field type plugs in here, not into a special-case branch elsewhere.
- **Virtualization assumptions** live in the header / row code. If you add a column whose width depends on data, you'll fight the layout cache — coordinate with the existing `SmatchetGridHeaderUi` pattern.
- **Notifications** (toasts, in-grid status) route through `SmatchetGridNotifications` — don't render `ImGui::Text("Saved")` ad-hoc.
- **Views** vs **grid**: `SmatchetViewsDashboardUi*` owns view CRUD UX; the grid renders the current view. Storage / serialization changes are `architect` territory.

**Workflow:**

1. New column or cell editor → start in `TrackerFieldCatalog` (does the field need a new type?), then `SmatchetFieldRender` (display), then `SmatchetGridFieldEditPipeline` (edit), then `TrackerGridFieldDisplay` (grid-specific routing).
2. Sort / filter changes → `TicketGridModel` (`CompareFieldValuesForSort` for new types).
3. Reorder / column visibility → `SmatchetGridHeaderUi` + the view storage path.
4. Build `ninja-iter-msys2`. If you touched anything per-cell, hand off to `perf-detective` to re-measure before claiming "no regression".

Report: files touched + new column / editor name + which pipeline stage it plugs into + per-cell allocation check (yes / no).

End with `## Self-improvement` — only on real friction (new hot-path concern, missing pipeline stage, virtualization edge case). Empty is fine. Orchestrator appends to `backlog/AGENT_SELF_IMPROVEMENT.md`.
