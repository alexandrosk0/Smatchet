- 2026-05-12 · tracker-backend · [debt] · P3 · DEFERRED — `RemoteProject` POD uses lowerCamelCase (`id`, `key`, `displayName`) while most other DTOs use PascalCase
  Details: Style drift introduced in PR 1. Worth normalizing before more call sites accumulate. Architect call.
  Concrete next action: C++ rename touching every `RemoteProject` call site (tracker-backend + grid-engine + bulk-import). Architect should scope the rename inside the next PR that legitimately touches `RemoteProject`. Don't open a standalone rename PR — bundle with adjacent work to minimise diff noise.
  Re-filed 2026-10-04 (backlog-sweep-2026-10): moved from process.md — this is product tech-debt, not workflow friction.
  Status: deferred
  Last-reviewed: 2026-10-04
