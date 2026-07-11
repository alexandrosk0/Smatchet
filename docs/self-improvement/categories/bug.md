# Agent self-improvement — bug (DEPRECATED)

> **DEPRECATED (ADR-0014, 2026-06-03).** Product bugs now live as **GitHub Issues**
> — see [`../../agent-rules/issue-triage.md`](../../agent-rules/issue-triage.md). This
> file is **frozen**: no new entries. The one-time migration (G) is **done**:
> 9 genuine product bugs became GitHub Issues (#734, #818, #820–#826), 4 tech-debt
> items moved to [`debt.md`](debt.md). The entries **below are the ambiguous residue** —
> they need a human bug-vs-Issue call (per the protocol, ambiguous is never silently
> filed). Resolve each: file an Issue, move to `debt.md`, or close as won't-do.
>
> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug (deprecated) · debt · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Ambiguous residue — pending a human bug-vs-Issue decision (ADR-0014 migration). -->

- 2026-05-20 · orchestrator · [bug] · P3 — Three UI-thread sync-I/O sites not yet moved to workers (Pillar 2 follow-up from Slice 2 migration)
  Details: Slice 2 of `docs/plans/shipped/pillar-1-2-perf-review-system.md` ran `bash scripts/dev/pillar2-scan.sh` against the full first-party tree + migrated the worker-bound false positives by annotating with `/* PILLAR2_WORKER_ONLY */ // est-latency:` markers. Three hits remain that are NOT worker-bound — UI-thread sync reads with bounded sizes today but flagged for migration. Tracked via `// TODO(pillar2): bug-2026-05-20-ui-sync-reads` comments at each site so the scanner reports them as WARN (not CRITICAL — doesn't block the lint gate). Sites: (1) `Source/Core/src/SmatchetAttachmentPreviewUi.cpp:61` `ParseImageDimensions` — reads the entire attachment file into memory on the UI thread to parse the first 24 bytes for PNG/JPEG dimensions. Up to the 50 MB attachment limit. Easy fix: `seekg` + read 64 bytes. (2) `Source/Core/src/SmatchetPlanDocViewerUi.cpp:95` `ReadCapped` — UI-thread read of `docs/plans/active/*.md` / `docs/adr/*.md` on combo-change. 1 MiB cap, local disk, typically sub-ms but legitimately sync on UI. Could move to worker with `MainThreadDispatcher::PostToMainThread` callback. (3) `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:92` `ReadFileAll` — Lua script load on editor-open (UI thread). Small scripts (typically < 100 KB), sub-ms typical. Could move to worker but the load-on-edit flow is a one-time cost.
  Concrete next action: fix in priority order: (1) ParseImageDimensions — high-impact (50 MB hot path), low effort (~30 min — switch to seekg + 64-byte read). (2) ReadCapped — low-impact (1 MiB cap), low effort (~30 min — worker + dispatcher post-back). (3) ReadFileAll — lowest impact (small files, one-time), defer until a real user reports a hitch. After fix, remove the TODO marker so the scanner stops emitting WARN.
  Status: partially applied (2026-06-20 trap-sweep — shipped: site 1 SmatchetAttachmentPreviewUi 64KB-bounded; remaining: sites 2+3 (SmatchetPlanDocViewerUi, LuaConsolePlugin) UI-thread sync reads, site 3 deferred)
  Last-reviewed: 2026-06-20
