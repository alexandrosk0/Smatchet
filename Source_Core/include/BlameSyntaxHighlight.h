#ifndef BLAME_SYNTAX_HIGHLIGHT_H
#define BLAME_SYNTAX_HIGHLIGHT_H

#include "ConfigManager.h"

/** Draw one line of C/C++-like source with basic token coloring (Dear ImGui). */
void BlameDrawColoredCppLine(const char* utf8Line, const BlameUiThemeColors& theme);

#endif






