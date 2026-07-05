# Mobile accessibility — research findings (Phase-1 slice P1.6)

> **Status**: research findings, not shipped code. This is the deliverable of slice **P1.6** of
> [`docs/plans/shipped/mobile-app-fuller-integration.md`](../plans/shipped/mobile-app-fuller-integration.md)
> (Pillar-4 is backlogged — no auto-fail yet). Code grounded against `develop` HEAD on **2026-06-20**.
> On-device confirmation (real TalkBack pass) is itself a follow-up — see § Validation gaps.

## Why this is research-only

Quality Pillar 4 (Accessibility — keyboard nav, font scaling, WCAG AA contrast) is the one UX pillar
that is **aspirational / backlogged** (`AGENTS.md` § Quality Pillars: "flagged in backlog (no auto-fail
yet)"). The plan scopes P1.6 as *research*: produce a findings doc + a Pillar-4 backlog entry, **not**
shipped code, because the hardest sub-problem (a screen-reader-navigable surface) is an open design
question for an immediate-mode GUI, not a mechanical port. This doc records what was found so a future
execution slice starts from facts, not a blank page.

## Scope & method

Audited the Android (GLES3 / `NativeActivity`) build's accessibility posture across the four axes Pillar 4
names, by reading the shipped Phase-0/Phase-1 code (no device — see § Validation gaps):

1. Screen-reader exposure (Android **TalkBack**) of the ImGui surface.
2. Font / UI scaling vs the OS accessibility text-size setting.
3. WCAG **AA** contrast of the mobile colour palette.
4. Touch-target sizing.

Plus focus indication and colour-only signifiers (WCAG 1.4.1 / 2.4.7).

---

## Finding 1 — Screen-reader (TalkBack) exposure: the hard problem

**State: not supported; no path exists today.** Android's accessibility services (TalkBack, Switch
Access, Voice Access) consume an **`AccessibilityNodeInfo` tree** that the platform derives from the
**`View` hierarchy**. Smatchet's Android host is a single GLES surface under `NativeActivity` —
Dear ImGui is an *immediate-mode* GUI that rasterises every widget into one textured draw list each
frame (`Source/Mobile/Android/SmatchetAndroidEgl.*`, `android_main.cpp`). There are **no child `View`s**,
so to TalkBack the entire app is one opaque, unlabelled element: nothing to focus, announce, or activate.
This is a known, structural Dear ImGui limitation, not a Smatchet bug.

**Three options, in increasing cost:**

- **(A) Virtual view hierarchy** — back the surface `View` with an `AccessibilityNodeProvider` (the
  `androidx.customview` `ExploreByTouchHelper` is the canonical helper) that maps each frame's visible
  ImGui widgets → *virtual* nodes carrying bounds (screen px), a label, a role, and actions
  (click/long-press). This is the mechanism canvas/game apps use. **Cost is real**: it needs an
  ImGui-widget → node bridge fed from a per-frame inventory of widgets (id, rect, label, state). ImGui
  does not expose that inventory natively; Smatchet would synthesise it from the same draw pass — likely
  a small "a11y emit" side-channel populated where widgets are drawn (the grid cells, chrome buttons,
  field editors). Hit-testing virtual nodes must reuse ImGui's own rects to stay consistent.
- **(B) Documented non-support (short term)** — ship with TalkBack unsupported, state it plainly in the
  store listing + an in-app note, and prioritise the visual-accessibility axes (Findings 2–4) that *are*
  tractable now. Honest and cheap; excludes blind users.
- **(C) Native overlay for critical controls** — render the few most critical controls (e.g. the
  view-switcher, Save/Cancel of the P1.3 touch editor) as real Android `View`s overlaid on the GLES
  surface, so at least the critical path is screen-reader-navigable while the bulk grid stays ImGui.
  Hybrid complexity (two input/focus models) but bounds the work.

**Recommendation:** **(B) now, scoped (A) later.** Treat full TalkBack support as its own multi-week
execution slice gated on the (A) bridge; do not block the rest of Phase-1 on it. The grid (the densest
surface) is where (A) is hardest and where (C) helps least.

## Finding 2 — Font / UI scaling vs the OS text-size setting

**State: density scaling shipped; OS *font-scale* (large-text a11y setting) is NOT yet honoured.**
A host density seam already exists and is wired on Android:

- `SmatchetTheme::ApplyUiDensityScale(scale)` stores a host scale; `ReapplyHostDensityScale` /
  `ReassertHostDensityScale` re-apply it via `ImGuiStyle::ScaleAllSizes` on style rebuilds and on
  `APP_CMD_CONFIG_CHANGED` DPI changes. The pure predicates `ShouldApplyDensityScale` /
  `ShouldRescaleHostDensity` (`Source/Core/include/Ui/SmatchetThemeDensity.h`) gate the maths and are
  doctest-unit-tested. The host feeds **display density** (`AConfiguration_getDensity / 160`).

**The gap:** that seam tracks **display DPI**, not the user's **accessibility font-size** preference.
Android exposes the large-text setting as `Configuration.fontScale` (and `densityDpi` separately).
Native ImGui apps must read `fontScale` *explicitly* — unlike Views/Compose, nothing applies it for you.
A user who sets "Largest" font in Android Accessibility settings currently sees **no change** in Smatchet.

**Recommendation (tractable, mostly already-built):** fold `Configuration.fontScale` into the value
handed to `ApplyUiDensityScale` (e.g. `effectiveScale = displayDensity * fontScale`, clamped), and
re-assert it on `APP_CMD_CONFIG_CHANGED` (the re-assert path already exists). Font *atlas* rebuild at
large scales is the one caveat (ImGui bakes glyphs at a fixed px size; very large scales want an atlas
rebuild, not just `ScaleAllSizes`). This is the **highest-value / lowest-cost** a11y win available.

## Finding 3 — WCAG AA contrast audit of the mobile palette

The mobile build uses the shared dark palette (`Source/Core/src/Ui/SmatchetTheme.cpp`). Computed WCAG 2.1
contrast ratios (relative-luminance method) for the load-bearing foreground/background pairs:

| Foreground | Background | Ratio | AA normal (≥4.5) | AA large/UI (≥3.0) |
|---|---|---|---|---|
| Body text `0.95` | WindowBg `0.12,0.12,0.14` | **14.8:1** | ✅ | ✅ |
| Body text `0.95` | FrameBg (inputs) `0.18,0.18,0.22` | **12.0:1** | ✅ | ✅ |
| Body text `0.95` | PopupBg `0.14,0.14,0.16` | **13.9:1** | ✅ | ✅ |
| Amber warning `1.0,0.80,0.30` | WindowBg | **11.0:1** | ✅ | ✅ |
| Accent-as-text `0.35,0.55,0.95` | WindowBg | **5.1:1** | ✅ | ✅ |
| **Disabled text `0.50`** | WindowBg | **4.15:1** | ❌ | ✅ |
| **White text `0.95` on accent fill `0.35,0.55,0.95`** | (active button / selected header / active tab) | **2.90:1** | ❌ | ❌ |

**Findings:**
- **Body text is excellent** (12–15:1) — no action.
- **Disabled text (`0.50` grey) = 4.15:1** → fails AA for *normal* text (needs 4.5), passes the
  large-text / non-text-UI 3:1 floor. Minor; disabled controls are lower-stakes. Nudging the disabled
  grey to ≈`0.55` clears 4.5:1.
- **White label on the accent-blue fill = 2.90:1** → fails **both** AA-normal **and** the 3:1 UI/large
  floor. This is the **most actionable** contrast defect: the accent `(0.35,0.55,0.95)` is used as the
  *fill* for `ButtonActive`, `HeaderActive`, `TabActive`, `CheckMark`, sliders, separators-active —
  i.e. every *selected/active* element — with body text drawn on top. Selected rows / active buttons
  therefore have insufficient label contrast. **Fix options:** (i) darken the accent fill to ≈
  `(0.20,0.34,0.62)` (clears 4.5:1 with white text), or (ii) keep the bright accent only for *borders /
  glyphs* and use a darker fill behind text, or (iii) draw the on-accent label in a dark colour. Option
  (i) is the least invasive and keeps the brand hue.

These are palette-level (shared desktop/mobile) findings; any change must be checked on desktop too
(golden-image approval) since the palette is shared — see `docs/agent-rules/golden-image-approval.md`.

## Finding 4 — Touch-target sizing

**State: unverified; structurally at risk in the grid.** WCAG 2.5.5 (AAA) wants ≥44×44 CSS px; Android
Material guidance is **48×48 dp**. The grid (`SmatchetActiveProjectGridUi.cpp`) was authored for desktop
mouse precision (hover-reveal affordances, dense rows). Without density/font scaling honouring the OS
a11y setting (Finding 2), small rows and chrome buttons likely fall below 48 dp on a phone. This is
**coupled to P1.3** (touch cell editors / interaction model): the long-press + explicit Save/Cancel
redesign should adopt ≥48 dp hit-rects for cells, the view-switcher, and editor Save/Cancel. No new axis
of work — fold the 48 dp minimum into P1.3's touch affordance acceptance criteria.

## Finding 5 — Focus indication & colour-only signifiers

- **Focus ring:** ImGui's keyboard-nav focus highlight is subtle on dark; an external keyboard user
  (Android supports them; also a Switch Access proxy) needs a clearly visible focus outline (WCAG 2.4.7).
  Cheap to strengthen via style (`NavHighlight`).
- **Colour-only state:** verify no state is conveyed by colour *alone* (WCAG 1.4.1) — e.g. a row whose
  only "selected" cue is the accent fill also has the (failing) contrast problem from Finding 3; add a
  non-colour cue (checkmark / left bar) which helps both 1.4.1 and the contrast issue.

---

## Recommendations — prioritised

| Pri | Item | Cost | Axis |
|---|---|---|---|
| **P1** | Honour `Configuration.fontScale` in the density seam (Finding 2) | Low (seam exists) | Font scaling |
| **P1** | Darken accent fill so white-on-accent ≥4.5:1 (Finding 3) | Low (palette) | Contrast |
| **P2** | 48 dp minimum touch targets — fold into P1.3 acceptance (Finding 4) | Low (rider on P1.3) | Touch |
| **P2** | Disabled-text grey `0.50`→`0.55` (Finding 3) | Trivial (palette) | Contrast |
| **P2** | Strengthen keyboard focus outline; add non-colour selected cue (Finding 5) | Low | Focus / 1.4.1 |
| **P3** | TalkBack: ship documented non-support now; scope the `AccessibilityNodeProvider` virtual-tree bridge as its own slice (Finding 1) | High | Screen reader |

P1/P2 items are mostly palette/seam tweaks that *could* ship as a small Pillar-4 slice once a build +
on-device visual pass is available. P3 (TalkBack) is a multi-week slice of its own.

## Validation gaps (what this research could NOT confirm without a device)

- No Android device/emulator (and no NDK/SDK) was available in the authoring environment, so **none of
  the above was confirmed on a running TalkBack session.** The contrast ratios are computed from source
  palette values (deterministic, device-independent — trustworthy); everything touch/screen-reader is
  **code-read inference** pending an on-device pass.
- A real TalkBack walkthrough + a touch-target measurement pass on a physical phone is the first task of
  any execution slice that picks this up.

## Backlog linkage

Tracked as a Pillar-4 product-debt entry:
[`docs/self-improvement/categories/debt/2026-06-20-mobile-accessibility-pillar4.md`](../self-improvement/categories/debt/2026-06-20-mobile-accessibility-pillar4.md).

## References

- WCAG 2.1 — 1.4.1 (Use of Colour), 1.4.3 (Contrast Minimum, AA), 1.4.11 (Non-text Contrast),
  2.4.7 (Focus Visible), 2.5.5 (Target Size).
- Android — `AccessibilityNodeProvider` / `androidx.customview.widget.ExploreByTouchHelper` (virtual
  view hierarchy); `Configuration.fontScale` vs `densityDpi`; Material 48 dp touch-target guidance.
- Dear ImGui — immediate-mode rendering has no native accessibility tree (upstream-acknowledged).
- Code seams: `Source/Core/include/Ui/SmatchetThemeDensity.h`, `Source/Core/src/Ui/SmatchetTheme.cpp`,
  `Source/Mobile/Android/android_main.cpp`, `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp`.
