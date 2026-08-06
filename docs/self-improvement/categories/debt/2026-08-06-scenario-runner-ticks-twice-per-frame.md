- 2026-08-06 · claude-code · [debt] · P2 — `ScenarioRunner::Tick` runs **twice per rendered frame**, so `--warmupFrames=N` silently means `N/2` rendered frames and any scenario that draws in `OnFrame` submits its content twice into one ImGui frame

  Two call sites, both live in the standalone ephemeral loop:
  - [`SmatchetUI.cpp:642`](../../../../Source/Core/src/Ui/SmatchetUI.cpp) — end of
    `drawPerFrameTicksAndHandlers`, itself the tail of `SmatchetUI::Draw`.
  - [`StandaloneAppBootstrap.cpp:685`](../../../../Source/Standalone/StandaloneAppBootstrap.cpp)
    — in `RunRenderLoop`, after `SmatchetDrawFrameWithSeh`.

  `git log -S` dates both sites and the bootstrap `SmatchetDrawFrameWithSeh` call to
  `27063e22` (2026-05-29), so the duplication is not recent.

  Two observable consequences:

  1. **Frame budgets are halved.** `IsDone(frameIndex)` is compared against a
     frame counter incremented twice per rendered frame, so `--warmupFrames=16`
     gives 8 actual rendered warm-up frames. Every scenario's warm-up constant is
     off by 2× from what its author intended, and any timing-sensitive scenario is
     tuned against the wrong number.
  2. **Content is drawn twice.** Scenarios that issue ImGui calls from `OnFrame`
     — e.g. [`CodeSyntaxColoringScenario`](../../../../Source/Core/src/Commands/Scenarios/CodeSyntaxColoringScenario.cpp)
     — submit their whole window twice into a single ImGui frame. The
     `code-syntax-coloring` capture visibly renders every code sample twice (and the
     Syncing toast twice, the second dimmed). Scenarios whose `OnFrame` only sets
     idempotent session flags (the `user-info-*` family) show no duplication, which
     is why this stayed invisible.

  Not fixed inline with PR #1962 deliberately: removing either call site changes
  frame semantics for **every** scenario (warm-up counts double overnight) and
  invalidates every bucket-C golden, so it needs its own slice with golden
  regeneration — which is approval-gated by
  [`golden-image-approval.md`](../../../agent-rules/golden-image-approval.md).

  Suggested shape: keep the in-`Draw` tick (it matches production ordering and
  runs inside the `NewFrame`/`Render` bracket), delete the bootstrap one, then
  halve every scenario's default warm-up constant in the same commit so the
  *rendered*-frame count is unchanged and only the goldens that were genuinely
  double-drawn move.
