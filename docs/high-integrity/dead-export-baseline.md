# Dead exports — grandfathered baseline

_Auto-generated. Do not hand-edit; run `bash agents/scripts/core/test-dead-export-audit.sh --baseline` and commit._
_The gate (`dead_export_audit.py --check`) is ADVISORY: it WARNs on findings absent from this file and never blocks. Graduation to blocking is a separate decision (mirrors ADR-0015)._

## dead-export (2 symbols)
- `Source/Core/include/Ui/SmatchetImGuiHostC.h:37` — `SmatchetHost_UpdateRendererColorFormat`
- `Source/Core/include/Ui/SmatchetImGuiHostC.h:101` — `SmatchetHost_SetKeyDown`

## Totals
- dead exports grandfathered: 2
