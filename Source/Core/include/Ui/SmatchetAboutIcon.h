#pragma once

#include "Persistence/SmatchetImageTextureCache.h"

namespace smatchet {
namespace ui {

/**
 * Resolve the baked-in app icon as an uploaded texture, ready for ImGui::Image.
 *
 * The icon bytes are the Win32 resource .ico, baked into the binary at configure time
 * (cmake/SmatchetIconBytes.h.in). This is the ONLY translation unit that includes the generated
 * header, keeping it out of anything packaged into the Unreal plugin.
 *
 * Decode + upload happen once; afterwards this is an icon-cache lookup. A failure (no baked icon,
 * an ICO variant the decoder does not read, or a renderer with no texture support) latches, so a
 * deterministic failure is not retried every frame.
 *
 * @return true with `out` filled; false when the caller must draw a glyph fallback instead.
 */
bool TryGetAboutIconTexture(SmatchetLoadedIconTexture& out);

} // namespace ui
} // namespace smatchet
