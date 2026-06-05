# Debug techniques (load on-demand)

Trigger: **debugging** a visual / behavioural issue. These project-wide techniques are mandatory whenever they apply. The full behavioural-bug investigation loop is `agents/core/debug-detective.md` + its `debug-instrument` skill; this doc holds the two standalone techniques AGENTS.md used to inline.

## Pink-clear UI gap detection

For "is the background ever visible behind panels?" / "are dock gaps still leaking?" questions, set the clear color to magenta (`glClearColor(1.0f, 0.0f, 1.0f, 1.0f)` on Standalone, equivalent `ClearRenderTargetView` color on DX12). Any visible pink is a guaranteed dock gap or transparent region. Pair with a screenshot + per-pixel pink scan for objective regression tests.

## Exe staleness check

After every rebuild, `ls -la` both the patched output and the most-likely-stale exe paths side-by-side, compare mtimes, and name the **exact** path the user should run. Multiple build outputs (`build/ninja-iter-msvc/`, `build/ninja-debug-msvc/`, `build/ninja-publish-msvc/`, worktree builds) make wrong-exe testing a common time-sink — orchestrator + perf / build agents all enforce this.
