# Golden screenshots — Phase 7 bucket-C verification

Per-channel L∞ ≤ 4 reference PPMs consumed by
[`scripts/dev/test-screenshot-diff.sh`](../../scripts/dev/test-screenshot-diff.sh).

## File map

| File | Scenario | Captures |
|---|---|---|
| `dock-gap-sentinel.ppm` | `dock-gap-sentinel` | Standalone main window after dock-builder settle. Regression on any dock-gap leak, shell-chrome shift, theme retune. |
| `command-palette-fuzzy.ppm` | `command-palette-fuzzy` | Command palette modal opened with the `scenario.` substring filter pre-applied. Regression on palette placement, fuzzy-match highlight, modal chrome. |

## Bootstrap protocol

Goldens are GPU-specific (font hinting + driver-noise). Each developer
captures their own goldens on first run; the L∞ ≤ 4 tolerance absorbs the
remaining per-vendor variance.

```bash
# First-time setup OR after intentional UI change:
bash scripts/dev/test-screenshot-diff.sh --bootstrap

# Inspect the captured PPMs (any image viewer that reads P6 PPM works):
explorer tests/golden/        # Windows
xdg-open tests/golden/        # Linux
open tests/golden/            # macOS

# Commit the new goldens so future runs gate against them:
git add tests/golden/*.ppm
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
3. Eyeball the new PPMs to confirm they match the intent.
4. `git add tests/golden/*.ppm && git commit ...`.

## Adding a new scenario

1. New `Source_Core/{include,src}/Commands/Scenarios/<Name>Scenario.{h,cpp}`
   following the `DockGapSentinelScenario` template.
2. `extern std::unique_ptr<smatchet::cmd::IScenario> Make<Name>Scenario();`
   factory registration in `AppController.cpp::Initialize`.
3. Append the scenario name to the `SCENARIOS` array in
   `scripts/dev/test-screenshot-diff.sh`.
4. Run `--bootstrap` once, commit the new golden.
