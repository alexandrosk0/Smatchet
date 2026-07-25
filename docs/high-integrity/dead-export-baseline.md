# Dead exports — grandfathered baseline

_Auto-generated. Do not hand-edit; run `bash agents/scripts/core/test-dead-export-audit.sh --baseline` and commit._
_The gate (`dead_export_audit.py --check`) is ADVISORY: it WARNs on findings absent from this file and never blocks. Graduation to blocking is a separate decision (mirrors ADR-0015)._

## dead-export (14 symbols)
- `Source/Core/include/Persistence/SmatchetImageTextureCache.h:40` — `GetOrLoadFromFile`
- `Source/Core/include/Persistence/SmatchetImageTextureCache.h:42` — `EvictCacheKey`
- `Source/Core/include/SmatchetDefaults.h:13` — `GetEnvString`
- `Source/Core/include/SmatchetLocalization.h:16` — `GetLanguage`
- `Source/Core/include/StringUtil.h:65` — `EqualsCaseInsensitive`
- `Source/Core/include/TicketGridModel.h:63` — `ResolveTicketGridRenderPlan`
- `Source/Core/include/Tracker/ProjectResolver.h:56` — `ResolveProjectForDraftFromParent`
- `Source/Core/include/Tracker/TrackerFieldValueUtils.h:23` — `SaveDurationSuggestions`
- `Source/Core/include/Tracker/TrackerFieldValueUtils.h:26` — `SaveCommentTemplates`
- `Source/Core/include/Ui/SelectableTextRun.h:97` — `TextRun`
- `Source/Core/include/Ui/SmatchetFieldIconRender.h:43` — `DrawInlineFieldIconIfAny`
- `Source/Core/include/Ui/SmatchetImGuiHostC.h:37` — `SmatchetHost_UpdateRendererColorFormat`
- `Source/Core/include/Ui/SmatchetImGuiHostC.h:101` — `SmatchetHost_SetKeyDown`
- `Source/Core/include/Ui/SmatchetThemedTextEditorPalette.h:41` — `GetThemedAiChatPalette`

## Totals
- dead exports grandfathered: 14
