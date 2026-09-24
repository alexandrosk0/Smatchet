#pragma once

// DataFreshnessCue — the one visible cue for data that is cached, refreshing or unavailable (Quality
// Pillar 6). Every network-backed view that can show cached data draws this instead of a hand-written
// "Loading..." line. Localized; draws nothing for DataFreshness::Fresh. UI thread only.

#include "OfflineFirstPure.h"

namespace DataFreshnessCue {

/// Localized short text for the state ("" for Fresh).
const char* CueText(smatchet::offline::DataFreshness f);
/// One disabled-text line; `detail` (e.g. the last error) shows as a hover tooltip when non-null and non-empty.
void Draw(smatchet::offline::DataFreshness f, const char* detail = nullptr);
/// Compact "(cached)" / "(refreshing)" badge for combo rows and grid cells; nothing for Fresh.
void DrawInlineBadge(smatchet::offline::DataFreshness f, const char* detail = nullptr);

} // namespace DataFreshnessCue
