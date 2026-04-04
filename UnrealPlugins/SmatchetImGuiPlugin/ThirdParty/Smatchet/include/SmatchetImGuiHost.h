#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class AppController;
class PluginHost;
class SmatchetUI;

enum class SmatchetRendererBackend : uint32_t {
    Unknown = 0,
    Dx12 = 1,
    Ps5 = 2,
    Xbox = 3
};

struct SmatchetRendererInitInfo {
    SmatchetRendererBackend Backend = SmatchetRendererBackend::Unknown;
    int NumFramesInFlight = 3;
    int ColorFormat = 0;
    void* NativeDevice = nullptr;
    /** D3D12: ID3D12CommandQueue* (e.g. Unreal ID3D12DynamicRHI::RHIGetCommandQueue()). Required for modern imgui_impl_dx12. */
    void* NativeCommandQueue = nullptr;
    void* RendererResource0 = nullptr;
    void* RendererResource1 = nullptr;
    void* RendererResource2 = nullptr;
};

class SmatchetImGuiHost {
public:
    struct InitOptions {
        std::string DbPath = "Smatchet_LocalCache.sqlite";
        std::string BackendType = "Jira";
        int McpPort = 8080;
        SmatchetRendererInitInfo Renderer;

        std::function<void(const std::string&)> OpenUrlHandler;
        std::function<void(const std::string& localPath,
                           const std::string& mimeType,
                           const std::string& filename)>
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

    AppController& GetAppController();
    PluginHost& GetPluginHost();

    // Cache Unreal-side init options without starting networking/loading yet.
    // The host will call Initialize(options) lazily when the UI is shown.
    void SetInitOptions(const InitOptions& options);
    bool UpdateRendererColorFormat(int colorFormat, std::string& outError);

    /** Last Initialize() failure (empty if never failed or last run succeeded). For UE_LOG / diagnostics. */
    const char* PeekLastInitErrorUtf8() const;
    /** snprintf-style summary of cached renderer init fields (ASCII). */
    void FormatCachedRendererDebugSummary(char* buf, std::size_t bufSize) const;

private:
    struct Impl;
    std::unique_ptr<Impl> ImplData;
};
