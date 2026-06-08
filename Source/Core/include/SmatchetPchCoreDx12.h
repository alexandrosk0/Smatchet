#pragma once

// Precompiled header for SmatchetCore_DX12 (Unreal-embedded DX12 lib). Carries
// nlohmann/json.hpp ONLY: ~64% of the ~225 Core_DX12 TUs pull this heavy header
// (67 direct includers under Source/Core plus transitive reach), so amortizing
// its parse is the whole point. json is platform/backend-agnostic + header-only,
// safe in the DX12 path. Deliberately separate from SmatchetPch.h (the
// Standalone PCH): no imgui.h (DX12 uses ImGuiLib_DX12, a different backend), no
// GLFW/OpenGL (no-glfw-in-core-headers gate), no <windows.h> (NOMINMAX /
// WIN32_LEAN_AND_MEAN already arrive as command-line defines on this target).
// ghc/filesystem measured ~6-7% reach — left out so json-free TUs don't pay.
// Disable via -DSMATCHET_USE_PCH=OFF when bisecting a compile error.
#include <nlohmann/json.hpp>
