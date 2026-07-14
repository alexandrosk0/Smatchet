# Golden screenshots — Phase 7 bucket-C verification

Per-channel L∞ ≤ 4 reference PNGs consumed by
[`scripts/dev/test-screenshot-diff.sh`](../../scripts/dev/test-screenshot-diff.sh).

**Format migrated PPM → PNG on 2026-05-17** to cut repo bloat (~40× smaller
post-compression). Bootstrap regenerates as `.png`; any leftover `.ppm` from a
prior clone should be deleted and rebootstrapped.

## File map

| File | Scenario | Captures |
|---|---|---|
| `dock-gap-sentinel.png` | `dock-gap-sentinel` | Standalone main window after dock-builder settle. Regression on any dock-gap leak, shell-chrome shift, theme retune. |
| `command-palette-fuzzy.png` | `command-palette-fuzzy` | Command palette modal opened with the `scenario.` substring filter pre-applied. Regression on palette placement, fuzzy-match highlight, modal chrome. |
| `code-syntax-coloring.png` | `code-syntax-coloring` | Multi-language code blocks (C++ / Python / Lua / Bash / JSON / Plain) rendered via `CodeColorView::DrawColoredCodeBlock`. Regression on per-language keyword/string/comment/number/identifier colouring, the theme syntax palette, or the language-badge layout. |
| `user-info-desktop-unified.png` | `user-info-desktop-unified` | Dockable User Info window at a desktop (1920px) framebuffer with `cfg.VcsFeedLayout="unified"`. Deterministic empty-feed steady state (empty email → instant p4 fail; GitHub config cleared → instant git fail; activity/groups compiled out under the headless spawn). Regression on the wide single-line VCS-row layout, the identity block, or the unified-feed header + radios. |
| `user-info-desktop-separate.png` | `user-info-desktop-separate` | As above at desktop width but `cfg.VcsFeedLayout="separate"` — the Perforce / Git feeds render as two independent collapsible sub-headers. Regression on the separate-layout sub-header split. |
| `user-info-narrow-unified.png` | `user-info-narrow-unified` | User Info window at a narrow (480px) framebuffer so the docked bottom-panel content region drops below `kNarrowLayoutWidthPx` (600) → the stacked two-line narrow-row path. `unified` layout. Regression on the responsive narrow-layout branch. |
| `user-info-narrow-separate.png` | `user-info-narrow-separate` | Narrow framebuffer + `separate` layout — narrow stacked rows under the split Perforce / Git sub-headers. Regression on the narrow × separate combination. |

## Bootstrap protocol

Goldens are GPU-specific (font hinting + driver-noise). Each developer
captures their own goldens on first run; the L∞ ≤ 4 tolerance absorbs the
remaining per-vendor variance.

```bash
# First-time setup OR after intentional UI change:
bash scripts/dev/test-screenshot-diff.sh --bootstrap

# Inspect the captured PNGs (any image viewer works):
explorer tests/golden/        # Windows
xdg-open tests/golden/        # Linux
open tests/golden/            # macOS

# Commit the new goldens so future runs gate against them:
git add tests/golden/*.png
git commit -m "test(phase-7): refresh bucket-C goldens after <reason>"
```

Subsequent runs (no `--bootstrap`) gate against the committed goldens:

```bash
bash scripts/dev/test-screenshot-diff.sh   # Passes when L∞ <= 4 on every scenario.
```

## Tolerance

Per-channel L∞ ≤ 4 (max absolute R/G/B byte delta). Tunable via
`SCREENSHOT_TOLERANCE=<N>` env var. Tighter tolerance catches subtler
regressions but raises false-positive risk on driver-version bumps.

## CI status

Advisory step in `.github/workflows/build-and-test.yml`
(`continue-on-error: true`) until 2026-05-30. Soak-period reasoning:
GitHub Actions `windows-2022` runners may lack a usable GL context for
`--spawn` UI sessions; we collect two consecutive weeks of clean runs
before flipping to blocking. Backlog entry filed for
`build-doctor` to wire mesa / ANGLE-D3D11 if the gap proves persistent.

## Regenerating after an intentional UI change

1. Make the UI change (theme retune, dock reorganisation, palette redesign).
2. `bash scripts/dev/test-screenshot-diff.sh --bootstrap` — overwrites the
   goldens with the post-change captures.
3. Eyeball the new PNGs to confirm they match the intent.
4. `git add tests/golden/*.png && git commit ...`.

## Adding a new scenario

1. New `Source/Core/{include,src}/Commands/Scenarios/<Name>Scenario.{h,cpp}`
   following the `DockGapSentinelScenario` template.
2. `extern std::unique_ptr<smatchet::cmd::IScenario> Make<Name>Scenario();`
   factory registration in `AppController.cpp::Initialize`.
3. Append the scenario name to the `SCENARIOS` array in
   `scripts/dev/test-screenshot-diff.sh`.
4. Run `--bootstrap` once, commit the new golden.
