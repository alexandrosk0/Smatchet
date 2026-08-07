- 2026-08-07 · claude-code · [process] · P2 — a backlog entry proposing a gate must name the concrete symbol the gate enumerates, and be checked against the bug that motivated it

  I proposed a gate to catch dock-node-id constants that name no real slot, and specified it as
  "enumerate `SmatchetDockNodeIds::kEntries`". That table lives in an anonymous namespace in the
  `.cpp` (so the qualified name is not even addressable) and maps layout keys to only three
  slots; `kSecondarySideBar` — the exact constant the gate exists to catch — never appears in
  it. The gate as written would have stayed green through its own motivating bug.

  The proposal read as concrete because it named a real symbol. Naming a symbol is necessary but
  not sufficient; the symbol has to be the one that actually enumerates the population.

  Two checks, both mechanical, both cheap enough to be unconditional:

  1. **Name the enumerator explicitly** — the file and the declaration the gate iterates, not a
     prose description of the population ("every dock id"). A prose population cannot be wrong,
     which is precisely why it hides this failure.
  2. **Replay the motivating bug against it** — walk the proposed enumerator by hand and confirm
     the known-bad case appears in it. If the entry cannot point at the row the gate would have
     tripped on, the gate is not specified yet.

  Generalises past gates: the same check applies to any proposed automation described by the
  population it covers. "Assert every X" is only meaningful once X resolves to an enumerable
  declaration, and only correct once the known counterexample is shown to be inside it.

  Concrete instance and the corrected proposal:
  [`../debt/2026-08-07-dock-node-id-slot-liveness-followups.md`](../debt/2026-08-07-dock-node-id-slot-liveness-followups.md).
