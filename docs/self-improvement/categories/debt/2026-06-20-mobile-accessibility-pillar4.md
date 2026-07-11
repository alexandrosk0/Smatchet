- 2026-06-20 · claude-code · [debt] · P2 — Mobile (Android) accessibility — Pillar-4 gaps from the P1.6 research slice

  Details: Slice P1.6 of `docs/plans/shipped/mobile-app-fuller-integration.md` (accessibility research)
  produced findings in `docs/mobile/PHASE1_ACCESSIBILITY_RESEARCH.md`. Four concrete, partly-tractable
  Pillar-4 gaps on the Android (GLES3 immediate-mode ImGui) build:
  (1) **Screen reader**: TalkBack sees the GLES surface as one opaque element — Dear ImGui has no
      `AccessibilityNodeInfo` tree. Full support needs an `AccessibilityNodeProvider` virtual-view bridge
      (its own multi-week slice); short term = documented non-support.
  (2) **Font scaling**: the host density seam (`SmatchetTheme::ApplyUiDensityScale` /
      `ReassertHostDensityScale`, `SmatchetThemeDensity.h`) honours display DPI but NOT the OS
      `Configuration.fontScale` (Android "large text" a11y setting), so large-text users see no change.
  (3) **WCAG AA contrast**: white text on the accent fill `(0.35,0.55,0.95)` — used for every
      active/selected element (`ButtonActive`/`HeaderActive`/`TabActive`/`CheckMark`) — is **2.90:1**,
      failing even the 3:1 UI floor; disabled text `0.50` grey is **4.15:1**, failing AA-normal (4.5).
      (Body text is fine, 12–15:1.) Palette is shared with desktop → golden-image approval applies.
  (4) **Touch targets**: grid rows / chrome authored for mouse precision likely fall below the 48 dp
      Material minimum; couple the fix into P1.3's touch redesign acceptance criteria.

  Concrete next action: ship the two low-cost wins as a small Pillar-4 slice once an Android build +
  on-device visual pass is available — (a) fold `Configuration.fontScale` into the density scale
  (the re-assert seam already exists), and (b) darken the accent fill so white-on-accent clears 4.5:1
  (and nudge disabled grey 0.50→0.55). Fold the 48 dp touch-target minimum into P1.3. Scope the TalkBack
  `AccessibilityNodeProvider` virtual-tree bridge as a separate slice; ship documented non-support first.
  Blocked on: an Android emulator/device for the on-device TalkBack + touch-target measurement pass
  (unavailable in the cloud/CI authoring environment — code-read inference only so far).

  Status: open
  Last-reviewed: 2026-06-20
