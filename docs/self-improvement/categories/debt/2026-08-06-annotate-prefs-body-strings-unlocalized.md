- 2026-08-06 · orchestrator · [debt] · P3 — `AnnotateAnalysisUi_Preferences.cpp` preference **body strings are still unlocalized**: the Preferences IA re-segmentation localized the new category/section titles it added but deliberately left every pre-existing widget label in that TU as an English literal
  Details: The TU is the one prefs page that never adopted `SmatchetLocalization` — it neither
    `#define ImGui SmatchetLocalizedImGui`s (the implicit English-literal lookup path the other prefs
    TUs use) nor calls `SmatchetLocalization::T()` explicitly, so its labels are hard English while
    every sibling category translates. This was scoped out up-front in
    `docs/plans/shipped/preferences-ia-resegmentation-and-search.md` § Out of scope ("Localizing the
    existing Annotate preference body strings (deferred, backlogged)") and recorded again in that
    plan's § Deviations from plan, which owes this entry.
    Second-order effect worth noting: `PreferencesFilter` indexes each setting's *translated* label,
    so under a non-English `cfg.UiLanguage` the Annotate settings are only findable by their English
    text (or by the descriptor `Keywords`, which are English by design). The filter is correct; the
    labels are the gap.
  Concrete next action: route the TU through `SmatchetLocalizedImGui` the same way
    `SmatchetPreferencesUi_Local.cpp` does (a `#define ImGui SmatchetLocalizedImGui` at the top plus
    the `SmatchetLocalization.cpp` rows for each surviving label), rather than sprinkling explicit
    `T()` calls — the define is the established shape in this family and keeps the diff to the string
    table. Watch the format-string hazard while doing it: a translated / non-literal string passed as
    a `Text`-family *fmt* argument routes through `TranslateSourceAsFormat` with an empty va_list, so
    those call sites need `"%s"`. Est ~2h including the string rows.
  Cross-ref: `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp`;
    `Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp` (the reference shape);
    `Source/Core/include/SmatchetLocalizedImGui.h`; `Source/Core/src/SmatchetLocalization.cpp`;
    `docs/plans/shipped/preferences-ia-resegmentation-and-search.md` § Out of scope + § Deviations.
  Status: open
  Last-reviewed: 2026-08-06
