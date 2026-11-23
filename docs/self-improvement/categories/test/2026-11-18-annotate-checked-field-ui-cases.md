# The annotate checked settings fields have no bucket-E case

- **Category**: test
- **Priority**: P3
- **Date**: 2026-11-18
- **Observed on**: review of the tolerant-parsing branch (`docs/plans/input-ux-w2-tolerant-parsing.md`, § Deviations) after it merged the shared text field.
- **Status**: open

## What happened

`DrawCheckedSettingField` in `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp` saves the p4 / p4vc executable and the AI chat URL only when the edit ends on text the normaliser accepts, writes back the cleaned form ("Copy as path" quotes removed), and shows the reason under the field otherwise. The normalisers and `TrackerInputNormalise::AcceptedOr` are unit-tested, but nothing drives the field itself: a regression in the commit path (saving refused text, or not writing back the cleaned form) would pass every test.

## Concrete next action

Add `TolerantParse` cases in `tests/ui/tolerant_parse_inline_error.test.cpp` that open Preferences > Connections > Perforce, type `"C:\Perforce\p4.exe"` with quotes and press Enter (field shows the unquoted path, config saved once), then type a folder path and click away (inline error shown, config keeps the previous path).
