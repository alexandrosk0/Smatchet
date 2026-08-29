#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "SmatchetDefaults.h"

class AppController;
class PluginHost;
class SmatchetUI;
struct TrackerConfig;

enum class SmatchetRendererBackend : uint32_t { Unknown = 0, Dx12 = 1, Ps5 = 2, Xbox = 3 };

struct SmatchetRendererInitInfo {
    SmatchetRendererBackend Backend = SmatchetRendererBackend::Unknown;
    int NumFramesInFlight = 3;
    int ColorFormat = 0;
    void* NativeDevice = nullptr;
    /** D3D12: ID3D12CommandQueue* (e.g. Unreal ID3D12DynamicRHI::RHIGetCommandQueue()). Required for modern
     * imgui_impl_dx12. */
    void* NativeCommandQueue = nullptr;
    void* RendererResource0 = nullptr;
    void* RendererResource1 = nullptr;
    void* RendererResource2 = nullptr;
    /** D3D12: total number of SRV descriptors in the heap pointed to by RendererResource0. Slot 0 is
     *  used for the font atlas (legacy single-SRV path); slots 1..N-1 are available for dynamic
     *  ImTextureData uploads (attachment thumbnails, user textures, etc.). Set at least 2 to enable
     *  attachment thumbnails. Defaults to 1 to preserve legacy behaviour. */
    int NumSrvDescriptors = 1;
};

class SmatchetImGuiHost {
  public:
    struct InitOptions {
        std::string DbPath = SmatchetDefaults::kDefaultDbPath;
        std::string BackendType = SmatchetDefaults::kDefaultBackendType;
        int McpPort = SmatchetDefaults::Mcp::kDefaultPort;
        SmatchetRendererInitInfo Renderer;

        std::function<void(const std::string&)> OpenUrlHandler;
        std::function<void(const std::string& localPath, const std::string& mimeType, const std::string& filename)>
            AttachmentViewerHandler;
    };

    SmatchetImGuiHost();
    ~SmatchetImGuiHost();

    bool Initialize(const InitOptions& options, std::string& outError);
    void Shutdown();

    void BeginFrame(float deltaTimeSeconds, float viewportWidth, float viewportHeight);
    void DrawUI();
    void RenderDrawData(SmatchetRendererBackend backend, void* nativeCommandList);

    /** Progress offline queues + streamed issue apply without building an ImGui frame. Call when the embedded
     *  host skips BeginFrame/DrawUI (e.g. Unreal overlay hidden) so tracker sync sessions still drain. */
    void TickApplicationWork();

    std::uint64_t EnqueueCommand(const std::string& commandName, const std::string& argsJson, bool confirmedDestructive,
                                 bool dryRun);
    bool IsCommandResultReady(std::uint64_t requestId) const;
    bool TakeCommandResultJson(std::uint64_t requestId, std::string& outJson);

    void SetMousePosition(float x, float y);
    void SetMouseButton(int button, bool isDown);
    void AddMouseWheel(float wheelX, float wheelY);
    void SetKeyModifiers(bool ctrl, bool shift, bool alt, bool superKey);
    /** Atomically apply modifier keys + key-down in one ImGui mutex hold (avoids render-thread NewFrame clearing
     * modifiers). */
    void ApplyKeyChordDown(int imguiKey, bool ctrl, bool shift, bool alt, bool superKey);
    /** Atomically apply modifier keys + key-up in one ImGui mutex hold. */
    void ApplyKeyChordUp(int imguiKey, bool ctrl, bool shift, bool alt, bool superKey);
    void AddInputCharacter(unsigned int character);

    bool IsUiVisible() const;
    bool IsInitialized() const;
    bool IsFrameActive() const;
    void SetUiVisible(bool visible);
    void ToggleUiVisible();

    /** When true, ImGui will not draw its software mouse sprite inside Unreal game viewports (useful if the OS cursor
     * is already visible). Default false. */
    void SetSuppressSoftwareCursor(bool suppress);
    bool GetSuppressSoftwareCursor() const;

    // Cache Unreal-side init options without starting networking/loading yet.
    // The host will call Initialize(options) lazily when the UI is shown.
    void SetInitOptions(const InitOptions& options);
    /** D3D12: patches the cached InitOptions with the SRV descriptor heap size so that the lazy
     *  Initialize() call picks it up when wiring up the dynamic-texture allocator. Must be set before
     *  the first BeginFrame after options are cached. */
    void SetRendererNumSrvDescriptors(int numSrvDescriptors);

    /** Last Initialize() failure (empty if never failed or last run succeeded). For UE_LOG / diagnostics. */
    const char* PeekLastInitErrorUtf8() const;
    /** snprintf-style summary of cached renderer init fields (ASCII). */
    void FormatCachedRendererDebugSummary(char* buf, std::size_t bufSize) const;

  private:
    void DrainCommandQueue(std::size_t maxCount);

#if defined(_WIN32)
    // Initialize() sub-steps (Windows-only; split out for function-size compliance). Each runs in
    // the same order and under the same preconditions as the pre-decomposition inline body.
    void initResolveStorageDirs(const InitOptions& options);
    bool initValidateRenderer(const InitOptions& options, std::string& outError);
    bool initImGuiContextAndBackend(const InitOptions& options, std::string& outError);
    void initRegisterPlugins(const InitOptions& options, const TrackerConfig& cfg);
#endif

    struct Impl;
    std::unique_ptr<Impl> ImplData;
};
