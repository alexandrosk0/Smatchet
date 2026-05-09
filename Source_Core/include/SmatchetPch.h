#pragma once

// =====================================================================================
// Precompiled header for SmatchetStandalone.
//
// Includes only the headers that are large *and* used widely. The point of a
// PCH is to amortize a one-time parse cost across many TUs — adding a
// rarely-used header bloats the PCH for no win, and adding a churning header
// (anything that re-includes itself often or pulls internal layout) means
// every header bump invalidates the PCH and rebuilds the world.
//
// What goes in:
//   - imgui.h           — included by 30+ UI translation units.
//   - windows.h (with LEAN_AND_MEAN, NOMINMAX) — included by a dozen Win32 TUs.
//   - the common STL containers/strings — cheap on their own but tedious to
//     re-parse 70 times.
//
// What deliberately stays OUT:
//   - imgui_internal.h  — only 7 callers; layout churns with ImGui bumps.
//   - sol/sol.hpp       — heavy (~1 MB header), only 2-3 callers; not worth it.
//   - nlohmann/json.hpp — was in PCH for a while; dropped because (a) the GCH
//     ballooned to 121 MB (mmap'd, so cosmetic, but slow first-time PCH compile)
//     and (b) the TUs that don't use json paid the parse cost anyway. Net
//     wash on full-rebuild time, faster first PCH compile, smaller GCH.
//     Re-add if profiling says otherwise.
//   - md4c.h            — pulled via extern "C" by 3 TUs; trivial to parse.
//   - third-party that wraps its own ifdefs around platform macros.
//
// Disable with `-DSMATCHET_USE_PCH=OFF` at configure time when bisecting an
// inscrutable compile error: `target_precompile_headers` skips the include,
// each TU sees the #include lines it actually wrote, and any "PCH masked the
// real bug" gremlins disappear.
// =====================================================================================

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#endif

// ImGui must come before SmatchetLocalizedImGui.h (which #defines ImGui to
// SmatchetLocalizedImGui via macro). Including imgui.h here is safe — TUs
// that pull in SmatchetLocalizedImGui.h afterwards still get the macro
// expansion, because a macro definition is unaffected by a prior #include.
#include <imgui.h>

// Common STL — included individually rather than <bits/stdc++.h> so we work
// across libstdc++ / libc++ / MSVC STL.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
