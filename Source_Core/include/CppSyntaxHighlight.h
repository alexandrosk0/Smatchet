#ifndef CPP_SYNTAX_HIGHLIGHT_H
#define CPP_SYNTAX_HIGHLIGHT_H

/** Draw one line of C/C++-like source with basic token coloring (Dear ImGui).
 *  Colors come from the active theme via SmatchetTheme::GetSyntaxColors(). */
void DrawColoredCppLine(const char* utf8Line);

/** Draw a multi-line C/C++ blob — splits on '\n' and emits one ImGui row per line. */
void DrawColoredCppText(const char* utf8Multiline);

#endif
