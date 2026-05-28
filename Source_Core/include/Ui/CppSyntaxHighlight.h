#ifndef CPP_SYNTAX_HIGHLIGHT_H
#define CPP_SYNTAX_HIGHLIGHT_H

/** Draw one line of C/C++-like source with basic token coloring (Dear ImGui).
 *  Colors come from the active theme via SmatchetTheme::GetSyntaxColors(). */
void DrawColoredCppLine(const char* utf8Line);

/** Draw a multi-line C/C++ blob — splits on '\n' and emits one ImGui row per line. */
void DrawColoredCppText(const char* utf8Multiline);

/** Slice 7 of docs/design/code-syntax-coloring-and-tooltips.md — semantic
 *  callstack tokenizer. Recognises the `Module!Class::Method(args) [Path\File.ext:Line]`
 *  pattern Smatchet sees from VS-debugger pastes + Unreal callstack dumps;
 *  emits per-element coloured spans:
 *    - Module               → default text
 *    - `!` / `::` / `(...)` → default text (punctuation)
 *    - Class identifiers    → Identifier (per-theme; slice 6)
 *    - Method (last segment before `(`) → Keyword (per-theme)
 *    - Path before filename → Comment (dimmed)
 *    - Filename + extension → Number (bright per-theme)
 *    - `:LineNumber`        → Number (bright per-theme)
 *  Lines that don't match the canonical shape fall through to the generic
 *  `DrawColoredCppLine` so non-callstack rows still render readably. */
void DrawColoredCallstackLine(const char* utf8Line);

/** Multi-line callstack version — splits on '\n' and emits one
 *  DrawColoredCallstackLine per row. */
void DrawColoredCallstackText(const char* utf8Multiline);

#endif
