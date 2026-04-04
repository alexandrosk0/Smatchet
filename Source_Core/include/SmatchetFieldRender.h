#pragma once

#include <string>

/** Implemented in SmatchetUI.cpp; shared by Jira grid field renderers. */
void RenderClippedFieldText(const std::string& rawValue,
                            float availWidth,
                            bool tooltipsEnabled,
                            bool disabled,
                            const std::string* rawForTooltip = nullptr);
