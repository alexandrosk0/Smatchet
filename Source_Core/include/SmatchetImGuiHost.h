#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class AppController;
class PluginHost;
class SmatchetUI;

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
        std::string DbPath = "Smatchet_LocalCache.sqlite";
        std::string BackendType = "Jira";
        int McpPort = 8080;
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

    void SetMousePosition(float x, float y);
    void SetMouseButton(int button, bool isDown);
    void AddMouseWheel(float wheelX, float wheelY);
    void SetKeyDown(int imguiKey, bool isDown);
    void SetKeyModifiers(bool ctrl, bool shift, bool alt, bool superKey);
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

    AppController& GetAppController();
    PluginHost& GetPluginHost();

    // Cache Unreal-side init options without starting networking/loading yet.
    // The host will call Initialize(options) lazily when the UI is shown.
    void SetInitOptions(const InitOptions& options);
    bool UpdateRendererColorFormat(int colorFormat, std::string& outError);
    /** D3D12: patches the cached InitOptions with the SRV descriptor heap size so that the lazy
     *  Initialize() call picks it up when wiring up the dynamic-texture allocator. Must be set before
     *  the first BeginFrame after options are cached. */
    void SetRendererNumSrvDescriptors(int numSrvDescriptors);

    /** Last Initialize() failure (empty if never failed or last run succeeded). For UE_LOG / diagnostics. */
    const char* PeekLastInitErrorUtf8() const;
    /** snprintf-style summary of cached renderer init fields (ASCII). */
    void FormatCachedRendererDebugSummary(char* buf, std::size_t bufSize) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> ImplData;
};






