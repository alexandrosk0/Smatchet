#pragma once

#include <string>

/** Shared clipped text renderer for grid and detail panels. */
void RenderClippedFieldText(const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool disabled,
                            const std::string* rawForTooltip = nullptr);
