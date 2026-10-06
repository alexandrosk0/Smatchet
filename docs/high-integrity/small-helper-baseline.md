# Small-helper clones — grandfathered baseline

_Auto-generated. Do not hand-edit; run `bash agent-layer/agents/scripts/core/test-small-helper-audit.sh --baseline` and commit._
_The gate (`small_helper_audit.py --check`) is ADVISORY: it WARNs on groups absent from this file and never blocks. Graduation to blocking is a separate decision (mirrors ADR-0015)._

_Band: 25 <= body tokens < 70 (dup_audit.py's MIN_CLONE_TOKENS, imported). A group is a body shared by >= 3 distinct TUs._

## Totals
- groups grandfathered: 2
- call sites across all groups: 6

## group `42c378ac8bf86a00` — 3 TUs, 27 tokens, name(s): `OnFrame`
- `Source/Core/src/Commands/Scenarios/AttachmentPreviewOpenScenario.cpp:35` — `OnFrame`
- `Source/Core/src/Commands/Scenarios/SideBySide2GridScenario.cpp:97` — `OnFrame`
- `Source/Core/src/Commands/Scenarios/SideBySideNGridScenario.cpp:107` — `OnFrame`

## group `3c49cb47d700cdb6` — 3 TUs, 26 tokens, name(s): `ThCol`, `Rgba`, `ColFromRgba`
- `Source/Core/src/Ui/AnnotateAnalysisUi_Modals.cpp:281` — `ThCol`
- `Source/Core/src/Ui/CppSyntaxHighlight.cpp:13` — `Rgba`
- `Source/Core/src/Ui/P4ClPreview.cpp:35` — `ColFromRgba`
