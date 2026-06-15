#ifndef SMATCHET_STANDALONE_DX12_BOOTSTRAP_H
#define SMATCHET_STANDALONE_DX12_BOOTSTRAP_H

// Standalone DirectX 12 renderer bootstrap for the GLFW window.
// Phase 1 of docs/plans/active/dx12-standalone-win-arm64.md: gives the
// standalone build a native D3D12 swapchain so it runs without OpenGL on
// Windows-on-ARM64 (where the GL path is software-emulated and slow). The
// OpenGL path stays the default everywhere else.
// This header is platform-agnostic on purpose: it declares only `void*`/POD
// surfaces (no <d3d12.h>, no <windows.h>) so both render loops can hold a
// `std::unique_ptr<Dx12Bootstrap>` and branch on a runtime enum without any
// preprocessor guards in the loop bodies. The full D3D12 implementation lives
// in Dx12Bootstrap.cpp under SMATCHET_STANDALONE_HAS_DX12; on every other
// toolchain the same methods compile as linkable no-ops.

#include "imgui.h" // ImVec4, ImDrawData

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// 1 only where a real D3D12 backend is available: Windows + an MSVC-compatible
// compiler (MSVC proper or clang-cl, both of which define _MSC_VER). MinGW does
// NOT define _MSC_VER, so it stays OpenGL-only and `--renderer=dx12` is rejected
// at argument-parse time.
#if defined(_WIN32) && defined(_MSC_VER)
#define SMATCHET_STANDALONE_HAS_DX12 1
#else
#define SMATCHET_STANDALONE_HAS_DX12 0
#endif

namespace smatchet {
namespace standalone {

// Which renderer a standalone window drives. Resolved once at startup from the
// `--renderer` flag (or its platform default) and carried through both render
// loops so the per-frame branch is a runtime check, never a preprocessor one.
enum class StandaloneRenderer {
    OpenGL,
    Dx12
};

// Resolve which renderer to drive from argv (`--renderer=dx12|gl`, or the
// `--renderer dx12` two-token form). When the flag is absent or its value is
// unknown, returns the platform default: Dx12 on Windows+MSVC (the GL path is
// software-emulated on Windows-on-ARM64), OpenGL everywhere else. A request for
// a backend this build lacks logs a warning and falls back to OpenGL. Defined
// commonly for every toolchain (outside the DX12 compile guard).
StandaloneRenderer ResolveStandaloneRenderer(int argc, char** argv);

// Owns the D3D12 device/swapchain/RTV-heap/fence/command objects backing one
// GLFW window, plus the ImGui DX12 renderer backend. pImpl so this header drags
// in no D3D12 types. All methods are no-ops returning failure/empty on
// platforms where SMATCHET_STANDALONE_HAS_DX12 == 0.
class Dx12Bootstrap {
  public:
    Dx12Bootstrap();
    ~Dx12Bootstrap();

    Dx12Bootstrap(const Dx12Bootstrap&) = delete;
    Dx12Bootstrap& operator=(const Dx12Bootstrap&) = delete;

    // Create device + swapchain for `hwnd` (a Win32 HWND passed opaquely) at the
    // given client size, then initialise the ImGui DX12 backend. On failure
    // returns false and fills `err`; falls back to the WARP software adapter when
    // no hardware adapter creates a device.
    bool Init(void* hwnd, int width, int height, std::string& err);

    // Begin an ImGui renderer-backend frame (ImGui_ImplDX12_NewFrame). Call after
    // the platform backend's NewFrame and before ImGui::NewFrame.
    void NewFrame();

    // Wait for the next in-flight frame, reset its allocator, transition the back
    // buffer to RENDER_TARGET and clear it to `clearColor` (premultiplied, to
    // match the GL clear), then bind it as the render target. Call after
    // ImGui::Render() and before RenderDrawData.
    void BeginFrame(const ImVec4& clearColor);

    // Record the ImGui draw data into this frame's command list.
    void RenderDrawData(ImDrawData* drawData);

    // Transition back to PRESENT, execute the command list, signal the fence and
    // Present. `syncInterval` is 0 (tearing/no-vsync) or 1 (vsync).
    void Present(int syncInterval);

    // Resize the swapchain to a new client size. No-op when unchanged or when
    // either dimension is 0 (minimised); caller should skip rendering at 0x0.
    void Resize(int width, int height);

    // Read the most-recently-presented back buffer into `rgba` as tightly-packed
    // top-left-origin RGBA8 (no vertical flip — DX12 textures are already
    // top-left, unlike GL's bottom-left readback). Returns false on failure.
    bool CaptureBackbuffer(std::vector<uint8_t>& rgba, int& width, int& height);

    // Release the ImGui backend and all D3D12 objects. Idempotent.
    void Shutdown();

    // Human-readable adapter description (e.g. "NVIDIA GeForce ..." or
    // "Microsoft Basic Render Driver" for WARP). Empty before Init / on no-DX12.
    std::string AdapterDescription() const;

    // True when the device was created on the WARP software adapter (hardware
    // adapter unavailable). False before Init / on no-DX12.
    bool IsWarp() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace standalone
} // namespace smatchet

#endif // SMATCHET_STANDALONE_DX12_BOOTSTRAP_H
