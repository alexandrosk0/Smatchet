# Plan — input UX Wave 2 piece 4.7: one matcher

> **Slug**: `input-ux-w2-one-matcher` (matches this file's basename without `.md`).
>
> **Status**: `active` — driving the Wave 2 piece 4.7 PR against `develop`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

[`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 4.7 found that every picker and filter matches text its own way: exact substrings, some case-sensitive, most ASCII-only, none ranked, and none show why a row matched. Settings search is the one surface that already falls back to the palette's fuzzy score when a plain substring finds nothing. This plan is piece 4.7: one shared matcher that every picker, filter and the grid title search go through, plus one draw helper that highlights the matched letters.

After this lands: typing in the shared dropdown, the command palette, settings search, the toolbar command list, the icon picker, the audit search, query field-name suggestions or the grid search box splits the input into words, folds case for Latin, Greek, Cyrillic and Armenian letters (not only ASCII) and finds the words in any order. The pickers, the palette, settings search, the toolbar list and the grid title search also fall back to letters-in-order matching when no word matches as written; the audit search and field-name suggestions match words as written only (ids, JSON and field names). Lists are ranked where shown, and the dropdown and palette highlight the letters that matched.

Closes (audit § 7): #8, #20, #42 (partly, see § Out of scope), #62 (the word split already shipped in Wave 1; this adds the fold and the title fallback), #67, #118 (the toolbar list already ranks through `FuzzyScore` since Wave 1; this gives it the token matcher), #131.

## Approach

One header-only, ImGui-free matcher, `Source/Core/include/TextMatchPure.h` (namespace `smatchet::textmatch`). It decodes UTF-8 (an invalid byte decodes as itself, so nothing is read past the end), folds each code point with a simple case-fold table for the blocks whose mapping is regular (ASCII, Latin-1, Latin Extended-A, Latin Extended Additional, Greek and Coptic, Cyrillic and its supplement, Armenian, fullwidth Latin), splits the query on whitespace into at most 16 words and scores a candidate word by word. A word that occurs contiguously scores on the same scale as a fuzzy one but the result carries a `Substring` flag, and lists follow the settings-search rule: **when any row matches every word as written, only those rows are listed**; letters-in-order matches appear only when nothing matches as written. So exact hits never drown in fuzzy noise, and every existing substring behaviour (the dropdown's bucket-E suite) holds. The fuzzy alignment is a small dynamic programme (best alignment, not first left-most), with bonuses for word starts, the start of the text, consecutive letters and a word typed whole, and a linear gap penalty. It returns the matched byte ranges.

Header-only because `tests/CMakeLists.txt` is plan-locked by `pane-picker-jira-sites` (§ Risks): a new production `.cpp` would have to join every test target that links a caller. `Commands/FuzzyMatch.{h,cpp}` keeps its two functions as thin wrappers over the matcher, so the registry's did-you-mean, the toolbar command list and settings search's tier 2 pick up the token matcher without a call-site change. The highlight is one draw helper, `Ui/SmatchetMatchHighlight.h`, that paints `ImGuiCol_TextSelectedBg` (a colour every theme in `SmatchetTheme.cpp` sets) under each matched byte range, the same mark the dropdown already drew for its single substring.

## Interface contracts

New shared headers. No tracker interface, command, Lua / MCP binding, config, cache or SQLite format changes.

```cpp
// Source/Core/include/TextMatchPure.h — header-only, no ImGui, C++14
namespace smatchet { namespace textmatch {
constexpr std::size_t kMaxQueryWords = 16;        // words past this are dropped (the match only loosens)
constexpr std::size_t kMaxWordCodepoints = 64;    // a longer word is cut
constexpr std::size_t kMaxFuzzyCodepoints = 512;  // letters-in-order matching looks this far into a candidate
constexpr std::size_t kMaxTextBytes = 16384;      // a candidate or query is read this far when matched
constexpr char32_t kInvalidByteBase = 0x110000;   // an invalid UTF-8 byte b decodes as this + b
constexpr int kMaxScore = 1 << 22;                // Result::Score is clamped here
constexpr long long kExactRankTier = 1LL << 40;   // added to the RankKey of an all-words-as-written result

char32_t DecodeUtf8(const char* s, std::size_t n, std::size_t& len);   // invalid byte -> itself, len 1
void AppendUtf8(std::string& out, char32_t cp);
char32_t FoldCodepoint(char32_t cp);
std::string FoldCopy(const char* s, std::size_t n);                    // folded UTF-8, every byte read
std::string FoldCopy(const std::string& s);
std::size_t FindFolded(const char* hay, std::size_t hayLen, const std::string& needleFolded, std::size_t from = 0);
std::size_t FindFolded(const std::string& hay, const std::string& needleFolded, std::size_t from = 0);
bool EqualsFolded(const char* text, std::size_t len, const std::string& otherFolded);
bool EqualsFolded(const char* text, const std::string& otherFolded);

struct Query { std::vector<std::u32string> Words; std::vector<std::string> WordsUtf8; bool Empty() const; };
Query MakeQuery(const char* raw, std::size_t n);
Query MakeQuery(const char* raw);
Query MakeQuery(const std::string& raw);
bool ContainsAllWordsFolded(const std::string& haystackFolded, const Query&);  // exact words, pre-folded haystack

struct ByteRange { std::size_t Begin = 0; std::size_t End = 0; };      // [Begin, End) bytes of the candidate
enum class Mode { Substring, Fuzzy };                                 // Substring: every word as written
struct Result { int Score = 0; bool Substring = false; bool Matched() const; };
long long RankKey(const Result& r);                                   // exact results always outrank fuzzy ones
bool IsExactRank(long long rankKey);
bool RowListed(const Result& r, bool anyExactRow);                    // the exact-first list rule

struct Field { const char* Text = nullptr; std::size_t Len = 0; int Weight = 1; bool ExactOnly = false; };
struct Scratch;                                                       // reusable working memory
// Every word must match one field; a word takes its best field (as written in any field beats letters in
// order anywhere; then weight x score). `ranges`, when given, points at `count` vectors.
Result MatchFields(const Query&, const Field* fields, std::size_t count, Mode, Scratch&,
                   std::vector<ByteRange>* ranges = nullptr);
Result Match(const Query&, const char* text, std::size_t len, Mode, Scratch&, std::vector<ByteRange>* ranges = nullptr);
Result Match(const Query&, const std::string& text, Mode = Mode::Fuzzy, std::vector<ByteRange>* ranges = nullptr);
}}

// Source/Core/include/Ui/SmatchetMatchHighlight.h (include after imgui.h)
namespace SmatchetMatchHighlight {
// Text at `pos` in `col`, with ImGuiCol_TextSelectedBg under each matched byte range (exact glyph advances).
void DrawText(ImDrawList* dl, const ImVec2& pos, ImU32 col, const char* text, const char* textEnd,
              const smatchet::textmatch::ByteRange* ranges, std::size_t rangeCount);
}
```

`FilterDropdownPure.h`: `ItemMatches` / `NormalizeFilter` / `ShouldOfferCreate` keep their names and now mean "the shared matcher" (fold beyond ASCII). New `ScoreItem(item, query, scratch, ranges)` scores label (weight 3), secondary text (1) and id (1, `ExactOnly`). `Row` gains `Rank` (the option's `RankKey`, 0 for a row listed only because it is checked) and `ModelInput` gains optional `Query` / `Scratch` pointers. `BuildRows` lists filtered matches best first (ties in list order), then the kept checked options; `DefaultHighlight` and `RowAnswersFilter` read `Row::Rank`. `MatchRank`, `FindCaseInsensitive`, `EqualsCaseInsensitive` and `LowerAscii` are removed (no caller left). `CommandPaletteUi::rebuildFiltered` scores Title (weight 3), Name (2) and Summary (1) separately (`rankByQuery`) and adds a small boost for recent commands; the row draws its title and summary through the highlight helper. Test-observability accessors `FilteredCount` / `FilteredName` / `FilteredTitleRangeCount` join `SelectedIndex`.

## Files to modify

**New: the matcher and the highlight**

1. `Source/Core/include/TextMatchPure.h`: the matcher (above).
2. `Source/Core/include/Ui/SmatchetMatchHighlight.h`, `Source/Core/src/Ui/SmatchetMatchHighlight.cpp`: the draw helper; the `.cpp` is picked up by `file(GLOB_RECURSE CORE_SOURCES …)`.

**Callers**

3. `Source/Core/include/Commands/FuzzyMatch.h`, `Source/Core/src/Commands/FuzzyMatch.cpp`: `FuzzyScore` / `FuzzyHasSubsequence` wrap the matcher (did-you-mean, toolbar command list, settings tier 2).
4. `Source/Core/include/FilterDropdownPure.h`: token matching, ranking, match ranges for the highlight (#8, #42 typo side).
5. `Source/Core/src/FilterDropdown.cpp`: rows draw through `SmatchetMatchHighlight::DrawText`; the body keeps one `Scratch` per open dropdown. (`#2443` adds a Ctrl check to this file's PageDown / PageUp handling, another hunk.)
6. `Source/Core/src/Commands/CommandPaletteUi.cpp`, `Source/Core/include/Commands/CommandPaletteUi.h`: per-field weighted scoring, recent boost, trimmed query, highlighted title (#20).
7. `Source/Core/src/Ui/PreferencesFilter.cpp`: `ToLowerFold` becomes the shared fold; tier 1 requires every word, in any order (`ContainsAllWordsFolded`). (`#2443` touches another hunk.)
8. `Source/Core/src/TicketGridFilterPure.cpp`, `Source/Core/include/TicketGridFilterPure.h`: words fold with the shared fold and match through `FindFolded` (Greek, Cyrillic, accented Latin case-insensitive); new `TicketTitleMatchesGridFilterFuzzy` (#62).
9. `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp` (`ApplyGridFilterProjection` only): when no row matches every word as written, keep the rows whose title matches the words letters-in-order. (`#2452` touches other hunks of this file.)
10. `Source/Core/src/Ui/SmatchetProjectPicker_detail.cpp`: `ContainsCi` folds through the matcher.
11. `Source/Core/src/Ui/SmatchetIconPickerUi.cpp`: token match, ranked (cached per query).
12. `Source/Core/src/Ui/SmatchetAuditUi.cpp`, `Source/Core/src/Ui/SmatchetAudit_detail.cpp`, `Source/Core/include/Ui/SmatchetAudit_detail.h`: the cached haystack is folded with the shared fold; the query matches every word in any order (plain substrings: the audit log is ids and JSON, no fuzzy).
13. `Source/Core/src/Tracker/TrackerQuerySuggestCommon.cpp`: field-name suggestions keep prefix hits first, then add names whose words contain the typed text (#67).
14. `Source/Core/src/CallstackParser.cpp`, `Source/Core/include/CallstackParser.h`: remap rules are trimmed, `/` and `\` compare equal, a drive-letter path compares case-insensitively, and the join never doubles or drops a separator (#131).

**Tests**

15. `tests/Core/TicketGridFilterPure.test.cpp`: matcher doctests (fold table, decode safety, words, ranking, ranges, caps) plus the grid fold and title fallback. It is a registered TU that runs in both the Linux subset and `SmatchetTests`; a new `tests/Core/TextMatchPure.test.cpp` would need the locked `tests/CMakeLists.txt` (debt entry, item 21).
16. `tests/Core/FilterDropdownPure.test.cpp`: ranking, fuzzy tier, Unicode fold, ranges.
17. `tests/Core/CallstackParser.test.cpp`, `tests/Core/ToolbarEditorPure.test.cpp`, `tests/Core/PreferencesFilter.test.cpp`, `tests/Core/JqlSuggestEnginePure.test.cpp`: cases for #131, #118, settings word order, #67.
18. `tests/ui/filter_dropdown.test.cpp`: bucket-E: a fuzzy query picks the ranked best row, a Greek upper-case query finds a lower-case option, the highlight ranges the row draws are the matched letters, exact matches hide fuzzy ones. The "i o" case is re-pinned (words now match in any order, see § Risks).
19. `tests/ui/grid_search_apply.test.cpp`: bucket-E (fixture lane `test-ui-grid-search-apply`): words in any order, case-folded, and the letters-in-order title fallback narrow the live grid.
20. `tests/ui/command_palette_inline_typing.test.cpp`: bucket-E: initials rank the expected command first.

**Docs**

21. `docs/self-improvement/categories/debt/2026-11-19-one-matcher-deferred.md`: deferred surfaces and the test relocation.
22. `Source/Core/src/Ui/AGENTS.md`: one Before-you-edit bullet: pickers and filters match through `TextMatchPure.h` and highlight through `SmatchetMatchHighlight`.
23. `docs/plans/active/input-ux-w2-one-matcher.md`: this plan.

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Commands/FuzzyMatch.cpp` | `FuzzyScore` | `CORE_SOURCES` |
| `Source/Core/src/FilterDropdown.cpp` | `DrawRowText` | `CORE_SOURCES` |
| `Source/Core/src/Commands/CommandPaletteUi.cpp` | `rebuildFiltered` | `CORE_SOURCES` |
| `Source/Core/src/Ui/PreferencesFilter.cpp` | `ToLowerFold` | `CORE_SOURCES` |
| `Source/Core/src/TicketGridFilterPure.cpp` | `ContainsFolded` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp` | `ApplyGridFilterProjection` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetProjectPicker_detail.cpp` | `ContainsCi` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetAudit_detail.cpp` | `ContainsNoCasePreLower` | `CORE_SOURCES` |
| `Source/Core/src/Tracker/TrackerQuerySuggestCommon.cpp` | `AppendFieldCatalog` | `CORE_SOURCES` |
| `Source/Core/src/CallstackParser.cpp` | `ApplyPathRemaps` | `CORE_SOURCES` |

## Existing utilities reused

- `smatchet::cmd::FuzzyScore`, `Source/Core/src/Commands/FuzzyMatch.cpp:31`: promoted; its bonuses (start, after separator, consecutive) carry over into the alignment score.
- `PreferencesFilter::RecomputeMatches`, `Source/Core/src/Ui/PreferencesFilter.cpp:170`: the two-tier rule (exact first, fuzzy only when exact finds nothing) every list now follows.
- `PreferencesFilter::ToLowerFold`, `Source/Core/src/Ui/PreferencesFilter.cpp:43`: the Latin-1 fold, now a subset of the shared table.
- `DrawRowText`'s exact-advance highlight, `Source/Core/src/FilterDropdown.cpp:368-381`: hoisted into the helper, generalised from one range to many.
- `SplitGridFilterTokens` / `TicketMatchesGridFilterTokens`, `Source/Core/src/TicketGridFilterPure.cpp`: the Wave 1 word split (#62) stays; only the compare changes.
- `TrimCopyAsciiWhitespace`, `Source/Core/include/StringUtil.h:9`: remap rule trim.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is split to clear a cap; the highlight hoist is ~20 lines.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: an open dropdown with a filter scores its options only when the filter or the options change: the body keys its ranked rows on an FNV-1a hash of the options' text and flags (callers rebuild their option arrays each frame, so pointers prove nothing), which costs about what the old per-frame substring scan did. A rebuild first rejects, straight off the bytes, every option whose fields cannot hold a word's letters in order, and decodes only fields some word can use. Measured at -O2 in the container on label + e-mail + id rows (one rebuild): 2k options 0.15-0.8 ms, 10k options 0.8 ms for a filter nothing matches, 1.1-2.1 ms for a selective one, 4.3-5.0 ms for a one-letter filter every option holds (once per keystroke, not per frame). The matcher reuses one `Scratch` per open dropdown, so a warm rebuild allocates nothing per option. The grid keeps its per-keystroke compare allocation-free (`FindFolded` decodes on the fly, ASCII fast path); the title fallback runs only when the strict pass found nothing. The palette and icon picker score only when the query changes.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no I/O, locks or waits. The audit haystack fold stays on its worker.
- **Pillar 3 (never crash)**: every decode is bounded by the byte count and never reads past it; an invalid UTF-8 byte decodes as itself; words, word length, candidate length and the alignment table are capped; ranges are clamped to the text. No recursion.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: what this plan exists for: fewer keystrokes to reach a row, and the matched letters are marked. The mark is the theme's existing selected-text colour, already used by the dropdown, so no contrast change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires on the fixed `perf-pr-fast-set.json` set. No scenario types into a picker; the grid scenarios run with an empty search box, so the changed filter compare is the empty-word fast path.
2. **Pillar 2 static scanner** — N/A: no sync I/O added to a render path.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` added.

**Pre-push local check**: not runnable in the linux-container tier (no Windows exe). The PR-fast lane runs on CI.

**Override**: none planned.

## Risks / non-goals

- **Plan-lock on `tests/CMakeLists.txt`** (`pane-picker-jira-sites`, live). Mitigation: no edit there. The matcher is header-only, so no test target needs a new source, and its doctests go in a registered TU (item 15). Debt entry to move them once the lock clears.
- **Words in any order change what some queries match.** "i o" now matches "iota" (both words occur), where the old phrase test found nothing. Accepted: that is the audit's ask (#62, #20). The bucket-E "Space is filter text" case is re-pinned on the filter text and the single pick, not on an empty result.
- **Fuzzy noise.** Letters-in-order matching is noisy on long text. Mitigation: the exact tier hides fuzzy rows whenever an exact row exists; the grid falls back to fuzzy on the title only; the audit search stays exact.
- **Partial Unicode fold.** Only blocks with a regular mapping fold (no Latin Extended-B, Georgian, Cherokee; no ß → ss expansion, no accent stripping). Accepted: the rest stay byte-exact, which is today's behaviour, never a wrong match.
- **Remap matching changes (#131).** A rule that used to miss on case or slash direction now applies. Case folds only when the rule or path starts with a drive letter, so POSIX and depot paths stay case-sensitive. Existing `CallstackParser` cases stay green.
- **Grid fuzzy fallback has no "close matches" cue.** The cue belongs in the grid search box (`SmatchetGridSearchUi.cpp`, changed by `#2441`) and `GridPane.h` (locked). Deferred to the debt entry.
- **Dual-target compile.** The new headers include only `<string>`, `<vector>`, `<cstddef>`, `<cstdint>` and (the highlight) `imgui.h`.
- Save formats, MCP wire format, tracker interfaces, localization keys: unchanged.
- **Non-goals:** edit-distance typo tolerance (transpositions, a wrong letter); accent-insensitive matching; label suggestions from a tracker lookup (#42's data side).

## Implementation handoff

N/A — single slice, orchestrator. Order: matcher + doctests, then `FuzzyMatch` wrappers, then the dropdown, palette, grid, the small pickers, remaps, then bucket-E.

## Verification

- **Bucket A (pure-logic doctest)**: items 15-17, run in `ninja-test-linux` (the grid / callstack / query-suggest TUs) and the full `SmatchetTests` on CI.
- **Bucket E (ImGui Test Engine, `ninja-ui-test-msvc`)**: items 18-20 on CI (`--all` lane for the dropdown and palette, the fixture lane for the grid).
- **Bash-driver scenario / screenshot / sanitizer**: none new; the sanitizer lanes run the doctests.
- **Build gate**: Windows dual-target on CI; locally `ninja-test-linux` plus a `posix-core-check` compile of the touched Core TUs.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against this plan before finalising; outcome recorded in § Verification (actual).
- **Manual residue**: none planned.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Highlight in the grid cells, the toolbar command list (`SmatchetToolbarUi.cpp`, changed by `#2440`) and the views list (`SmatchetViewsDashboardUi.cpp`, locked): follow-ups in the debt entry.
- A "showing close matches" cue for the grid fallback: debt entry.
- #42's tracker label lookup and per-user recent labels: a data-source change, not the matcher.
- #131's remap Preview field and trim-on-commit in the Annotate preferences (`AnnotateAnalysisUi_Preferences.cpp`, changed by `#2440` / `#2441`): the matcher side trims instead; the preview is in the debt entry.

## Implementation log
- `ac11770` · wip(plan): this plan.
- `82b2c33` · the shared matcher (`TextMatchPure.h`), the highlight helper and every caller listed above, with the bucket-A and bucket-E cases.
- `3b7997a` · merge of `develop` (the mobile-lane pin).
- `a82bb10` · field-name suggestions match word starts only (Windows unit suite); the grid fallback skips an empty query; ASCII remap fold (Cursor Bugbot).
- `93c8a5d` · clang-format of one include comment in the palette test.
- `024b002` · CodeRabbit's 13 findings: UTF-8 character boundaries in the matcher, a blank wide-space dropdown filter, an unambiguous options cache key, cached row highlight ranges, the pressed-row guard, the toolbar exact-first rule, word matching in the project picker, the empty-ToPrefix rule documented.
- `8d229c7` · the empty-ToPrefix wording in `CallstackParser.h`; this log.
- (this commit) · CodeRabbit's head-`8d229c7` finding: field-name word starts split at the shared matcher's non-word characters (`textmatch::IsWordChar` on decoded code points), so "points" finds "Story—Points" and a no-break-space name; JQL case added.

## Deviations from plan
- **Matcher doctests** live in `tests/Core/TextMatchPure.cases.h`, a header included once by `tests/Core/TicketGridFilterPure.test.cpp`, rather than inline in that TU: same rig (Linux subset and `SmatchetTests`), one file to move when `tests/CMakeLists.txt` unlocks (debt entry).
- **`FoldCopy` reads every byte.** The plan capped it at `kMaxTextBytes`; an audit event's JSON can be longer, and a capped fold would hide matches past 16 KiB. Matching (`Match` / `MatchFields`) keeps the cap.
- **Added to the API while building:** `EqualsFolded` (the create row's "no label equals the filter" test without an allocation), `Field::ExactOnly` (ids never match letters-in-order, as #8 asks), `kExactRankTier` / `IsExactRank` / `RowListed` (one spelling of the exact-first rule for every list), `kMaxScore` (so `FuzzyScore` can add its tier boost inside `int`).
- **Dropdown order under a filter:** matches first, best first; options listed only because they are ticked (`KeepCheckedVisible`) come after them, in list order (they used to sit in list order among the matches). `MatchRank` and the ASCII helpers were removed with their last caller.
- **Path remaps (#131):** besides trim, slash folding and drive-letter case, a `FromPrefix` without a trailing separator now matches whole path segments only ("D:/Work/Game" no longer rewrites "D:/Work/Games/..."), and the join puts back a separator the rule's `ToPrefix` dropped.
- **Files touched beyond the list:** `Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp` (review fix above), `Source/Core/include/Ui/SmatchetIconPickerUi.h` (the cached match list), `Source/Core/include/Ui/SmatchetProjectPicker_detail.h` (doc line), `tests/Core/AuditWindowFilter.test.cpp` and `tests/Core/ProjectPickerFilter.test.cpp` (cases for the audit and project-picker folds).
- **Audit search** keeps `ContainsNoCasePreLower`'s name; it now means "every word, in any order" over the shared fold.
- **Code review (2026-11-19) fixes:** (High) a Create row now outranks a letters-in-order match for the default highlight, so Enter in the labels picker adds "qa" instead of ticking "quarantine" (the arrow keys still reach it; pure and bucket-E cases). (Medium) the per-frame rescoring became a per-change one (row cache keyed on an options hash, byte-level pre-reject, lazy decode), measured above. (Low) the grid falls back to letters-in-order only when no row at all, shown or hidden as a parent, holds the words as written. (Low) the missing simple folds in Greek and Coptic (U+0370/0372/0376/037F, U+03CF-03FF symbols) and U+1E9B were added, with an idempotence check over U+0000-U+1FFFF. (Low) the keybinding table's filters (`SmatchetPreferencesUi_Keybindings.cpp`) match every word in any order through the shared fold, like the settings search above them.
- **Golden:** `tests/golden/command-palette-fuzzy.png` predates the palette's match marks. Bucket-C reports the drift (advisory lane); the re-capture needs the CI bootstrap job and a human verdict, so it is in the debt entry, not this PR.

## Verification (actual)
- **Bucket A, Linux subset** (`ninja-test-linux`, `SmatchetTsanTests`): 1204 / 1204 cases pass after the field-name word-start fix (1203 at `024b002`), including the matcher cases (fold table and idempotence over U+0000-U+1FFFF, decode safety, character boundaries, words, ranking, ranges, caps), the grid fold and fallback, remaps and JQL field names.
- **Bucket A, `SmatchetTests` TUs built standalone under ASan/UBSan**: FilterDropdownPure, ToolbarEditorPure, PreferencesFilter, AuditWindowFilter, ProjectPickerFilter, CallstackParser, TrackerQuerySuggestCommon: all pass. A 300k-input fuzz run of the matcher and brute-force checks of the alignment and the boundary helpers (review agent): clean.
- **Windows CI, first head**: the MSVC build and the dual-target build compiled; `SmatchetTests` caught one real regression (`TrackerQuerySuggestCommon.test.cpp:218`, a one-letter prefix suggesting "Assignee"), fixed in `a82bb10`. The final head's CI results are on the PR's checks.
- **Bucket E** (`FilterDropdown` T-X, `CommandPalette` `OneMatcher_InitialsRankAndMarkTheTitle`, `GridSearch` `TitleSearch_WordsAnyOrderCaseFoldedLettersInOrder`): run by CI only (Windows + Mesa); the three TUs pass a syntax check against imgui_test_engine locally.
- **Perf**: a dropdown rebuild measured at -O2 (numbers in § UX Pillar callouts); rebuilds happen per change, not per frame.
- **Gates**: `pre-ship.sh` lint delta, duplication, function size and `test-docs.sh` pass; the code-review agent reviewed every commit (6 findings in its first pass, all fixed, then clean); Cursor Bugbot (2) and CodeRabbit (13, then 1 on `8d229c7`) findings addressed on their threads.
- **`grill-with-docs`**: run without an interviewer (autonomous session). No glossary in `Source/Core/src/Ui/` or the repo root defines a picker / filter / matcher term to conflict with, and the plan claims no storage shape. Nothing to change.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index from the repository root: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
