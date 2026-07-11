#pragma once

#include "SmatchetImageTextureCache.h"

#include <string>

class AppController;
struct CachedTicket;
struct TicketGridColumn;
struct TrackerField;

namespace SmatchetFieldIconRender {

/**
 * DR28 backoff decision (pure, dependency-free so it is unit-testable without the icon/HTTP rig).
 * Given whether a prior fetch/resolve failure was recorded for a candidate, when that failure
 * happened, the current time and the backoff window (all as steady-clock milliseconds), returns
 * true when a fresh attempt is warranted and false while the candidate is still within backoff.
 * Callers use this to keep an unresolvable icon from re-issuing a multi-second HTTP GET every frame.
 */
inline bool ShouldAttemptIconResolve(bool haveNegativeEntry, long long lastFailureMs, long long nowMs,
                                     long long backoffMs) {
    if (!haveNegativeEntry || backoffMs <= 0) {
        return true;
    }
    return (nowMs - lastFailureMs) >= backoffMs;
}

/**
 * Draw icon-only for built-in Jira priority and Lua `register_field_icon_map` (read-only cells only).
 * Returns false to fall back to text / normal editors.
 * @param allowCellEdits when true: Lua icon maps are skipped (preserve editors); editable `priority` uses
 *   `DrawInlineFieldIconIfAny` in the combo row instead of full-cell draw here.
 */
bool TryDrawFieldValueIcon(const AppController& app, const std::string& fieldId, const TrackerField* field,
                           const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool allowCellEdits);

/** Load inline icon texture for SingleSelect (Lua map or priority); does not submit ImGui items. */
bool TryGetInlineFieldIconTexture(const AppController& app, const TrackerField& field, const std::string& rawValue,
                                  SmatchetLoadedIconTexture& outIcon, std::string& outError);

/** Small icon left of combo: Lua `register_field_icon_map` (any field), else built-in `priority` (Jira/bundled/URL). */
bool DrawInlineFieldIconIfAny(const AppController& app, const TrackerField& field, const std::string& rawValue);

/** Lua / scripting: draw image from URL or resolved local path (see `ResolveFieldIconAssetPath`). */
bool DrawImagePathOrUrl(AppController& app, const std::string& pathOrUrl, float width, float height);

} // namespace SmatchetFieldIconRender






