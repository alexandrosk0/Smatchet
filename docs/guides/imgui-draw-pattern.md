# ImGui draw-function section-helper pattern

> Canonical decomposition pattern for Smatchet's immediate-mode UI. Any `Draw*` /
> `draw*` function that approaches **200 lines** uses this shape. Enforced by the
> `function-too-long` / `function-too-branchy` gate (`function_size_audit.py`, wired
> into `test-lint-rules.sh` — see `AGENTS.md` § Tiered enforcement). Plan:
> [`docs/plans/shipped/decompose-top-20-monoliths.md`](../plans/shipped/decompose-top-20-monoliths.md)
> § Approach A.
>
> **Tiered cap (2026-06-01):** non-UI functions block at **120 lines**; ImGui-draw
> functions get a **200-line** escape hatch (path under `Ui/` OR name starting
> `Draw`/`Render`/`draw`/`render`) because declarative UI is inherently noisier. The
> 200-line headroom is for cases where **section-splitting would add more noise than
> clarity** — not a licence to skip the pattern. A new/changed function over **100
> lines** (or **20 branches**) also draws a non-blocking `[func-size] WARN` advisory;
> prefer the 40-80-line ideal even when under the 200 hard cap.

## Why

Immediate-mode UI naturally accretes: one `void DrawX(...)` ends up owning window
setup, every per-section layout block, every state mutation, and every action
dispatch. Past ~200 lines that body defeats `clang-tidy` cognitive-complexity
analysis, forces a full-file reread on every minor edit, couples unrelated concerns
(fetch + layout + side-effect in one frame body), and multiplies merge-conflict
surface across feature branches. The section-helper pattern extracts at the natural
seams so each piece is independently readable and the orchestrator body stays
layout-only.

## Canonical shape

```cpp
// Header (.h) — on the owning UI object
class FooUi {
public:
    void Draw(AppController& app, UiDrawSession& d);

private:
    struct DrawCtx {
        AppController& app;
        UiDrawSession& d;
        // captured-once-per-frame snapshots (ticketsSnap, columns, …) live here,
        // NOT re-fetched per helper.
    };

    bool BeginWindow(UiDrawSession& d);   // ImGui::Begin + early-return guard
    void DrawHeader(DrawCtx& ctx);        // toolbar, filters, banner
    void DrawBody(DrawCtx& ctx);          // main table / grid / canvas
    void DrawFooter(DrawCtx& ctx);        // status row, hints
    void DrawModals(DrawCtx& ctx);        // popups owned by this window
    void HandleHotkeys(DrawCtx& ctx);     // keyboard-shortcut dispatch
    // per-section helpers as needed; each <= ~80 lines

    struct FooWindowState {               // persistent per-window state — see Rule 4
        char filterBuf[128] = {};
        // … was `static` locals
    };
    FooWindowState fooState_;
};

// .cpp
void FooUi::Draw(AppController& app, UiDrawSession& d) {
    if (!BeginWindow(d)) { ImGui::End(); return; }
    DrawCtx ctx{ app, d /* + snapshots */ };
    { SMATCHET_UI_PERF_SCOPE("foo:header"); DrawHeader(ctx); }
    { SMATCHET_UI_PERF_SCOPE("foo:body");   DrawBody(ctx);   }
    { SMATCHET_UI_PERF_SCOPE("foo:footer"); DrawFooter(ctx); }
    DrawModals(ctx);
    HandleHotkeys(ctx);
    ImGui::End();
}
```

## Rules

1. **`DrawCtx` struct** holds the per-frame snapshots + references. No more 30-line
   argument lists; no more `static` locals leaking across windows-of-same-type.
2. **One responsibility per helper.** Header / body / footer / modals / hotkeys are
   non-overlapping. A helper that grows past ~80 lines splits again.
3. **Perf scopes stay at the section boundary.** Where `SMATCHET_UI_PERF_SCOPE`
   blocks already bracket sections, **reuse them verbatim** — the brace-block becomes
   the helper-call site, scope name unchanged → **zero baseline shift, no
   `MARKER_INVENTORY.md` regen**. Where a function has *no* scopes, decompose on
   logical sections and treat any *new* scope as an intentional, inventory-tracked
   addition (regen `docs/perf/MARKER_INVENTORY.md` in that PR). Helper-internal
   scopes only when `perf-detective` asks for finer resolution.
4. **Window-state extraction.** Persistent `static` locals (filter buffers,
   expanded-row sets, last-selection ids) move into a `<Foo>WindowState` member
   struct on the owning UI object. For Smatchet's singleton windows this is
   behaviour-identical; for any future multi-instance window it's a strict
   improvement (per-instance, not shared). Audit:
   `grep -nE "static .*(Buf|bool s_|\[)" Source/Core/src/Ui/Smatchet*Ui*.cpp`.
5. **Action handlers** (button click → mutation) move into `OnX()` methods returning
   `void`/`bool`. Keeps the draw body layout-only.
6. **Section-file split when a `.cpp` exceeds ~1500 lines.** Precedent:
   `SmatchetViewsDashboardUi.cpp` + `SmatchetViewsDashboardUi_widgets.cpp`,
   `SmatchetPreferencesUi_Whisper.cpp`, `SmatchetPreferencesUi_Assistant.cpp`.
   Naming: `<Owner>Ui_<Section>.cpp`.
7. **Do NOT edit `docs/high-integrity/function-size-baseline.md` in a decomposition PR.**
   That file is an **informational snapshot, not the gate input** — the live gate is
   the `function_size_audit.py --diff origin/develop` merge-base delta. A decomposed
   function simply leaves the over-cap set, so the delta gate passes with **zero**
   baseline edit. Editing it per-PR creates a cross-PR merge cascade: every merge
   re-conflicts every sibling decomposition PR on `baseline.md`, forcing repeated
   regen + resolve. Regenerate the snapshot **once per campaign**, not per slice.

## Positional-ImGui hazards (the part that bites)

`ImGui::Begin/End`, `PushID/PopID`, `BeginTable/EndTable`, `BeginChild/EndChild`,
`Indent/Unindent`, `PushStyleVar/PopStyleVar` are **positional and paired**. When you
cut a section into a helper:

- Keep each `Begin*`/`End*` pair **inside the same helper** — never split a pair
  across the orchestrator/helper boundary.
- Preserve `PushID`/`PopID` nesting depth exactly; an off-by-one ID change silently
  breaks widget state persistence (and selection).
- Preserve call **order** — immediate-mode output is order-dependent.
- Layout / behaviour / state semantics are **byte-for-byte preserved**. This is a
  pure mechanical decomposition, not a rewrite. Verify with the window's bucket-C
  screenshot diff + bucket-E test against the pre-refactor golden — any visual delta
  is a regression, not an improvement.

## Worked reference

`SmatchetUI::drawActiveProjectWindow` (`SmatchetActiveProjectGridUi.cpp`) is the
canonical canary — 8 existing `activeProject:*` perf scopes that become the section
seams (zero baseline shift). It is the reference application of this pattern
(decompose-top-20-monoliths Slice 1).

## When to apply

- **New** draw functions: write them in this shape from the start.
- **Existing** monoliths: **ride-along only** — decompose a draw function when a
  feature already opens that file (per the plan's Phase B). Do *not* open a dedicated
  mechanical-decomposition PR per draw function; the churn/conflict/regression cost of
  a sweep outweighs the benefit, and the size gate already prevents regrowth.

## Verification

**Before committing a decomposition, run the per-file size scan on the touched TU** — do
not trust the `--diff` merge-base gate alone. The delta gate **grandfathers** a function
that was already over cap (it lives in both the HEAD and base sets), so a *partial*
decomposition that is still over cap passes `--diff` silently:

```sh
python agents/scripts/core/function_size_audit.py --scan-file <touched-file.cpp>
# every helper must be under the cap (no `function-too-long` / `function-too-branchy` row);
# the soft-tier `[func-size] WARN` lines (>100 lines / >20 branches) are advisory — aim 40-80.
```

For the absolute end-state (a campaign step asserting the whole subtree is drained, where the
delta gate's grandfathering would hide a still-over-cap survivor), use the repo-wide assertion:

```sh
python agents/scripts/core/function_size_audit.py --assert-clean --in Source/Core/src/Ui/
# exit 0 iff NOTHING under that subtree is over any hard cap (grandfather-blind, zero-tolerance).
```

To prove one named function actually dropped under cap: `--assert-absent <Cls::Draw> --in <file>`.
