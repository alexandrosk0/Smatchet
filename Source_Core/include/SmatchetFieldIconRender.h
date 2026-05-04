#pragma once

#include "SmatchetImageTextureCache.h"

#include <string>

class AppController;
struct CachedTicket;
struct TicketGridColumn;
struct TrackerField;

namespace SmatchetFieldIconRender {

/**
 * Draw icon-only for built-in Jira priority and Lua `register_field_icon_map` (read-only cells only).
 * Returns false to fall back to text / normal editors.
 * @param allowCellEdits when true: Lua icon maps are skipped (preserve editors); editable `priority` uses
 *   `DrawInlineFieldIconIfAny` in the combo row instead of full-cell draw here.
 */
bool TryDrawFieldValueIcon(AppController& app, const std::string& fieldId, const TrackerField* field,
                           const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool allowCellEdits);

/** Load inline icon texture for SingleSelect (Lua map or priority); does not submit ImGui items. */
bool TryGetInlineFieldIconTexture(AppController& app, const TrackerField& field, const std::string& rawValue,
                                  SmatchetLoadedIconTexture& outIcon, std::string& outError);

/** Small icon left of combo: Lua `register_field_icon_map` (any field), else built-in `priority` (Jira/bundled/URL). */
bool DrawInlineFieldIconIfAny(AppController& app, const TrackerField& field, const std::string& rawValue);

/** Lua / scripting: draw image from URL or resolved local path (see `ResolveFieldIconAssetPath`). */
bool DrawImagePathOrUrl(AppController& app, const std::string& pathOrUrl, float width, float height);

} // namespace SmatchetFieldIconRender
