#include "SmatchetImGuiHost.h"
#include "SmatchetImGuiHostC.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <mutex>

#include "imgui.h"
#include "Logger.h"
#include "imgui_impl_dx12.h"
#include "SmatchetImGuiFonts.h"
#endif

// Smatchet core components (needed for the pImpl state).
#include "AppController.h"
#include "ConfigManager.h"
#include "PluginHost.h"
#include "SmatchetUI.h"
#include "McpPlugin.h"
#include "LuaConsolePlugin.h"

// Private implementation that keeps SmatchetImGuiHost.h lightweight for Unreal.
// Unreal should not need to include AppController/PluginHost/SmatchetUI headers.
struct SmatchetImGuiHost::Impl {
    std::atomic<bool> Initialized{false};
    std::atomic<bool> Initializing{false};
    std::atomic<bool> BuildingFrame{false};
    std::atomic<bool> RenderingDraw{false};
    std::atomic<bool> FrameActive{false};
    std::atomic<bool> OptionsSet{false};
    std::atomic<bool> UiVisible{false};

    SmatchetImGuiHost::InitOptions CachedOptions;

    // ImGui uses a thread-local "current context" (TLS). Unreal calls into this host
    // from different threads (Slate/input vs render). We store the created context and
    // re-attach it on each thread before touching ImGui.
    ImGuiContext* ImGuiCtx = nullptr;

    AppController App;
    PluginHost Plugins;
    SmatchetUI Ui;

    // Serialize all ImGui API calls across threads (IO updates, NewFrame, Render, etc).
    std::mutex ImGuiMutex;

    // Backend resource used by renderer implementation.
    // ImGui_ImplDX12_RenderDrawData() does not bind descriptor heaps, so we store it
    // and bind it right before rendering.
    void* RendererResource0 = nullptr;

    // Throttle failed init retries when UI is visible but DX resources aren't ready yet.
    std::atomic<std::uint64_t> LastInitAttemptMs{0};

    std::string LastInitError;
};

#if defined(_WIN32)
// Modern imgui_impl_dx12 requires ImGui_ImplDX12_InitInfo with a real ID3D12CommandQueue.
// The legacy 6-argument ImGui_ImplDX12_Init is known to crash against current ImGui / D3D12. (#8429)
static bool Smatchet_ImplDX12_InitBackend(const SmatchetRendererInitInfo& renderer, std::string& outError) {
    outError.clear();
    if (!renderer.NativeDevice || !renderer.NativeCommandQueue || !renderer.RendererResource0 ||
        !renderer.RendererResource1 || !renderer.RendererResource2) {
        outError = "Missing required DX12 initialization resources (device, command queue, font SRV heap, handles).";
        return false;
    }

    auto* device = reinterpret_cast<ID3D12Device*>(renderer.NativeDevice);
    auto* commandQueue = reinterpret_cast<ID3D12CommandQueue*>(renderer.NativeCommandQueue);
    auto* fontSrvDescriptorHeap = reinterpret_cast<ID3D12DescriptorHeap*>(renderer.RendererResource0);
    D3D12_CPU_DESCRIPTOR_HANDLE fontSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE fontSrvGpuHandle{};
    fontSrvCpuHandle.ptr = static_cast<SIZE_T>(reinterpret_cast<std::uintptr_t>(renderer.RendererResource1));
    fontSrvGpuHandle.ptr = static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(renderer.RendererResource2));

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = device;
    initInfo.CommandQueue = commandQueue;
    initInfo.NumFramesInFlight = renderer.NumFramesInFlight;
    initInfo.RTVFormat = static_cast<DXGI_FORMAT>(renderer.ColorFormat);
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = fontSrvDescriptorHeap;
    initInfo.LegacySingleSrvCpuDescriptor = fontSrvCpuHandle;
    initInfo.LegacySingleSrvGpuDescriptor = fontSrvGpuHandle;

    {
        char dbg[640];
        std::snprintf(
            dbg,
            sizeof(dbg),
            "[Smatchet] ImGui_ImplDX12_Init(ptr): dev=%p cq=%p heap=%p cpu=%llu gpu=%llu dxgi_fmt=%d nframes=%d\n",
            static_cast<void*>(device),
            static_cast<void*>(commandQueue),
            static_cast<void*>(fontSrvDescriptorHeap),
            static_cast<unsigned long long>(fontSrvCpuHandle.ptr),
            static_cast<unsigned long long>(fontSrvGpuHandle.ptr),
            static_cast<int>(renderer.ColorFormat),
            renderer.NumFramesInFlight);
        OutputDebugStringA(dbg);
    }

    if (!ImGui_ImplDX12_Init(&initInfo)) {
        outError = "ImGui_ImplDX12_Init failed.";
        return false;
    }
    return true;
}
#endif

SmatchetImGuiHost::SmatchetImGuiHost() : ImplData(new Impl()) {}

SmatchetImGuiHost::~SmatchetImGuiHost() {
    Shutdown();
}

bool SmatchetImGuiHost::IsUiVisible() const {
    return ImplData && ImplData->UiVisible.load(std::memory_order_relaxed);
}

bool SmatchetImGuiHost::IsInitialized() const {
    return ImplData && ImplData->Initialized.load(std::memory_order_acquire);
}

bool SmatchetImGuiHost::IsFrameActive() const {
    return ImplData && ImplData->FrameActive.load(std::memory_order_acquire);
}

void SmatchetImGuiHost::SetUiVisible(bool visible) {
    if (!ImplData) {
        return;
    }

    if (!visible) {
        ImplData->UiVisible.store(false, std::memory_order_relaxed);
        ImplData->FrameActive.store(false, std::memory_order_relaxed);
        return;
    }

    ImplData->UiVisible.store(true, std::memory_order_relaxed);
}

void SmatchetImGuiHost::ToggleUiVisible() {
    SetUiVisible(!IsUiVisible());
}

void SmatchetImGuiHost::SetInitOptions(const InitOptions& options) {
    if (!ImplData) {
        return;
    }
    ImplData->CachedOptions = options;
    ImplData->OptionsSet.store(true, std::memory_order_release);
}

bool SmatchetImGuiHost::UpdateRendererColorFormat(int colorFormat, std::string& outError) {
    outError.clear();
    if (!ImplData) {
        outError = "ImplData is null.";
        return false;
    }
    if (colorFormat <= 0) {
        outError = "Invalid renderer color format.";
        return false;
    }

    if (ImplData->CachedOptions.Renderer.ColorFormat == colorFormat) {
        return true;
    }

    ImplData->CachedOptions.Renderer.ColorFormat = colorFormat;
    ImplData->OptionsSet.store(true, std::memory_order_release);

#if !defined(_WIN32)
    outError = "Renderer RTV update is Win32/DX12-only.";
    return false;
#else
    if (!ImplData->Initialized.load(std::memory_order_acquire)) {
        // Not initialized yet; new format will apply on first Initialize().
        return true;
    }

    const SmatchetRendererInitInfo& renderer = ImplData->CachedOptions.Renderer;
    if (renderer.Backend != SmatchetRendererBackend::Dx12) {
        outError = "Renderer color format update supports only DX12 backend.";
        return false;
    }

    if (!renderer.NativeDevice || !renderer.NativeCommandQueue || !renderer.RendererResource0 ||
        !renderer.RendererResource1 || !renderer.RendererResource2) {
        outError = "Missing required DX12 resources for RTV format update.";
        return false;
    }

    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }

    ImGui_ImplDX12_Shutdown();
    if (!Smatchet_ImplDX12_InitBackend(renderer, outError)) {
        return false;
    }

    ImplData->RendererResource0 = renderer.RendererResource0;
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    return true;
#endif
}

bool SmatchetImGuiHost::Initialize(const InitOptions& options, std::string& outError) {
    outError.clear();
    if (ImplData) {
        ImplData->LastInitError.clear();
    }
    if (ImplData->Initialized.load(std::memory_order_acquire)) {
        return true;
    }

#if !defined(_WIN32)
    outError = "SmatchetImGuiHost renderer backend is unavailable on this platform.";
    if (ImplData) {
        ImplData->LastInitError = outError;
    }
    LOG_ERROR("SmatchetImGuiHost::Initialize failed: %s", outError.c_str());
    return false;
#else
    if (options.Renderer.Backend != SmatchetRendererBackend::Dx12) {
        outError = "Only DX12 backend is implemented in this runtime build.";
        ImplData->LastInitError = outError;
        LOG_ERROR("SmatchetImGuiHost::Initialize failed: %s", outError.c_str());
        return false;
    }

    if (!options.Renderer.NativeDevice || !options.Renderer.NativeCommandQueue || !options.Renderer.RendererResource0 ||
        !options.Renderer.RendererResource1 || !options.Renderer.RendererResource2) {
        outError = "Missing required DX12 initialization resources (device, command queue, SRV heap, handles).";
        ImplData->LastInitError = outError;
        LOG_ERROR("SmatchetImGuiHost::Initialize failed: %s", outError.c_str());
        return false;
    }

    auto* device = reinterpret_cast<ID3D12Device*>(options.Renderer.NativeDevice);
    auto* fontSrvDescriptorHeap = reinterpret_cast<ID3D12DescriptorHeap*>(options.Renderer.RendererResource0);
    D3D12_CPU_DESCRIPTOR_HANDLE fontSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE fontSrvGpuHandle{};
    fontSrvCpuHandle.ptr = static_cast<SIZE_T>(reinterpret_cast<std::uintptr_t>(options.Renderer.RendererResource1));
    fontSrvGpuHandle.ptr = static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(options.Renderer.RendererResource2));

    IMGUI_CHECKVERSION();
    // Diagnostics: if these are invalid/dangling, ImGui's font atlas upload can crash in NewFrame.
    std::fprintf(
        stderr,
        "SmatchetImGuiHost::Initialize: device=%p queue=%p heap=%p cpu.ptr=%llu gpu.ptr=%llu\n",
        static_cast<void*>(device),
        options.Renderer.NativeCommandQueue,
        static_cast<void*>(fontSrvDescriptorHeap),
        static_cast<unsigned long long>(fontSrvCpuHandle.ptr),
        static_cast<unsigned long long>(fontSrvGpuHandle.ptr));
    {
        std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
        ImGui::CreateContext();
        ImplData->ImGuiCtx = ImGui::GetCurrentContext();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io);

    // Ensure this thread has the correct ImGui context during DX12 init.
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    if (!Smatchet_ImplDX12_InitBackend(options.Renderer, outError)) {
        LOG_ERROR("SmatchetImGuiHost::Initialize backend init failed: %s", outError.c_str());
        ImGui::DestroyContext();
        return false;
    }

    // Cache the SRV heap for later draw calls.
    ImplData->RendererResource0 = options.Renderer.RendererResource0;

    // Warm up font atlas while backend still matches our single-descriptor setup.
    {
        std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
        if (ImplData->ImGuiCtx) {
            ImGui::SetCurrentContext(ImplData->ImGuiCtx);
        }
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height); // Force atlas build / builder init.
    }

    ImplData->App.SetOpenUrlHandler(options.OpenUrlHandler);
    ImplData->App.SetAttachmentViewerHandler(options.AttachmentViewerHandler);

    JiraConfig cfg = ConfigManager::Load();
    const int mcpPort = (cfg.McpPort >= 1 && cfg.McpPort <= 65535) ? cfg.McpPort : options.McpPort;
    if (cfg.McpEnabled) {
        ImplData->Plugins.Register(std::unique_ptr<IPlugin>(new McpPlugin(mcpPort)));
        LOG_INFO("SmatchetImGuiHost: MCP plugin enabled on port %d", mcpPort);
    } else {
        LOG_INFO("SmatchetImGuiHost: MCP plugin disabled by config.");
    }
    ImplData->Plugins.Register(std::unique_ptr<IPlugin>(new LuaConsolePlugin()));
    ImplData->Plugins.OnEarlyInit(ImplData->App);
    ImplData->App.Initialize(options.DbPath, options.BackendType);
    ImplData->Plugins.OnStart(ImplData->App);

    ImplData->Initialized.store(true, std::memory_order_release);
    ImplData->LastInitError.clear();
    return true;
#endif
}

void SmatchetImGuiHost::Shutdown() {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }

#if defined(_WIN32)
    ImplData->Plugins.OnStop();
    {
        std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
        if (ImplData->ImGuiCtx) {
            ImGui::SetCurrentContext(ImplData->ImGuiCtx);
        }
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
        ImplData->ImGuiCtx = nullptr;
    }
#endif

    ImplData->FrameActive.store(false, std::memory_order_relaxed);
    ImplData->Initialized.store(false, std::memory_order_release);
}

void SmatchetImGuiHost::BeginFrame(float deltaTimeSeconds, float viewportWidth, float viewportHeight) {
    if (!ImplData) return;

    // Lazy init while UI is visible (retry if DX resources weren't ready at first show).
    if (!ImplData->Initialized.load(std::memory_order_acquire)) {
        if (IsUiVisible() && ImplData->OptionsSet.load(std::memory_order_acquire)) {
#if defined(_WIN32)
            // Prevent multiple concurrent Initialize() calls (can corrupt ImGui global state).
            if (!ImplData->Initializing.exchange(true, std::memory_order_acq_rel)) {
                const std::uint64_t nowMs = static_cast<std::uint64_t>(GetTickCount64());
                const std::uint64_t lastMs = ImplData->LastInitAttemptMs.load(std::memory_order_relaxed);
                if (nowMs - lastMs > 1000) {
                    ImplData->LastInitAttemptMs.store(nowMs, std::memory_order_relaxed);
                    std::string err;
                    const bool ok = Initialize(ImplData->CachedOptions, err);
                    if (!ok) {
                        // Keep UI visible; we'll retry until resources are ready and Initialize succeeds.
                        std::fprintf(stderr, "SmatchetImGuiHost: Initialize() retry failed: %s\n", err.c_str());
                        LOG_ERROR("SmatchetImGuiHost: Initialize() retry failed: %s", err.c_str());
#if defined(_WIN32)
                        {
                            std::string msg = std::string("[Smatchet] Initialize failed: ") + err + "\n";
                            OutputDebugStringA(msg.c_str());
                        }
#endif
                    }
                }
                ImplData->Initializing.store(false, std::memory_order_release);
            } else {
                // Another thread is initializing; skip this frame until it finishes.
                return;
            }
#else
            // Non-Windows builds are unsupported; don't retry continuously.
            std::string err;
            Initialize(ImplData->CachedOptions, err);
#endif
        }
        // If init hasn't succeeded yet, don't build a frame.
        if (!ImplData->Initialized.load(std::memory_order_acquire)) {
            ImplData->FrameActive.store(false, std::memory_order_relaxed);
            return;
        }
    }

#if defined(_WIN32)
    if (!IsUiVisible()) {
        // When hidden we still receive input, but skip building a frame/draw data.
        ImplData->FrameActive.store(false, std::memory_order_relaxed);
        return;
    }

    // ImGui is not thread-safe: BeginFrame/NewFrame must not run concurrently.
    if (ImplData->BuildingFrame.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }

    // Diagnostics: confirm context attachment and that we'll reach ImGui::NewFrame().
    std::fprintf(
        stderr,
        "SmatchetImGuiHost::BeginFrame: thread=%lu ctx=%p ui=%d\n",
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<void*>(ImplData->ImGuiCtx),
        IsUiVisible() ? 1 : 0);

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = (deltaTimeSeconds > 0.0f) ? deltaTimeSeconds : (1.0f / 60.0f);
    io.DisplaySize = ImVec2(viewportWidth, viewportHeight);

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    ImplData->FrameActive.store(true, std::memory_order_relaxed);
    ImplData->BuildingFrame.store(false, std::memory_order_release);
#else
    (void)deltaTimeSeconds;
    (void)viewportWidth;
    (void)viewportHeight;
#endif
}

void SmatchetImGuiHost::DrawUI() {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire) ||
        !ImplData->FrameActive.load(std::memory_order_relaxed) || !IsUiVisible()) {
        return;
    }

    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }

    // Temporary diagnostic window to confirm the render path is producing output.
    // If you see this, rendering is working and the issue is inside SmatchetUI window visibility/state.
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
        ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, 140), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Smatchet Debug", nullptr, flags)) {
            ImGui::Text("UiVisible=%d Initialized=%d", IsUiVisible() ? 1 : 0,
                        ImplData->Initialized.load(std::memory_order_relaxed) ? 1 : 0);
            ImGui::Text("FrameActive=%d", ImplData->FrameActive.load(std::memory_order_relaxed) ? 1 : 0);
            ImGui::Text("If this is visible, ImGui rendering is OK.");
        }
        ImGui::End();
    }

    ImplData->Ui.Draw(ImplData->App);
    ImplData->Plugins.OnDraw(ImplData->App);
}

void SmatchetImGuiHost::RenderDrawData(SmatchetRendererBackend backend, void* nativeCommandList) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire) ||
        !ImplData->FrameActive.load(std::memory_order_relaxed) || !IsUiVisible()) {
        return;
    }

#if defined(_WIN32)
    if (backend != SmatchetRendererBackend::Dx12) {
        return;
    }

    if (!nativeCommandList) {
        return;
    }
    auto* commandList = reinterpret_cast<ID3D12GraphicsCommandList*>(nativeCommandList);

    // ImGui rendering must also not run concurrently.
    if (ImplData->RenderingDraw.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }

    ImGui::Render();

    ImDrawData* drawData = ImGui::GetDrawData();
    static double LastDrawStatsLogSeconds = 0.0;
    {
        // Avoid spamming logs: at most once per 1s.
        // (Use std::chrono instead of UE time here; it's fine for diagnostics.)
        using clock = std::chrono::steady_clock;
        const double nowSeconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(clock::now().time_since_epoch()).count();
        if (nowSeconds - LastDrawStatsLogSeconds > 1.0) {
            LastDrawStatsLogSeconds = nowSeconds;
            if (drawData) {
                std::fprintf(
                    stderr,
                    "SmatchetImGuiHost::RenderDrawData: display=(%f,%f) totalVtx=%d cmds=%d ui=%d init=%d\n",
                    drawData->DisplaySize.x,
                    drawData->DisplaySize.y,
                    drawData->TotalVtxCount,
                    drawData->CmdListsCount,
                    IsUiVisible() ? 1 : 0,
                    ImplData->Initialized.load(std::memory_order_relaxed) ? 1 : 0);
            } else {
                std::fprintf(stderr, "SmatchetImGuiHost::RenderDrawData: drawData=null\n");
            }
        }
    }

    if (drawData) {
        if (ImplData->RendererResource0) {
            ID3D12DescriptorHeap* heaps[] = { reinterpret_cast<ID3D12DescriptorHeap*>(ImplData->RendererResource0) };
            commandList->SetDescriptorHeaps(1, heaps);
        }
        ImGui_ImplDX12_RenderDrawData(drawData, commandList);
    }
    ImplData->RenderingDraw.store(false, std::memory_order_release);
#else
    (void)backend;
    (void)nativeCommandList;
#endif

    ImplData->FrameActive.store(false, std::memory_order_relaxed);
}

void SmatchetImGuiHost::SetMousePosition(float x, float y) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(x, y);
}

void SmatchetImGuiHost::SetMouseButton(int button, bool isDown) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    if (button < 0 || button >= 5) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDown[button] = isDown;
}

void SmatchetImGuiHost::AddMouseWheel(float wheelX, float wheelY) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.MouseWheelH += wheelX;
    io.MouseWheel += wheelY;
}

void SmatchetImGuiHost::SetKeyDown(int imguiKey, bool isDown) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(static_cast<ImGuiKey>(imguiKey), isDown);
}

void SmatchetImGuiHost::SetKeyModifiers(bool ctrl, bool shift, bool alt, bool superKey) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_LeftCtrl, ctrl);
    io.AddKeyEvent(ImGuiKey_LeftShift, shift);
    io.AddKeyEvent(ImGuiKey_LeftAlt, alt);
    io.AddKeyEvent(ImGuiKey_LeftSuper, superKey);
}

void SmatchetImGuiHost::AddInputCharacter(unsigned int character) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.AddInputCharacter(character);
}

const char* SmatchetImGuiHost::PeekLastInitErrorUtf8() const {
    if (!ImplData || ImplData->LastInitError.empty()) {
        return "";
    }
    return ImplData->LastInitError.c_str();
}

void SmatchetImGuiHost::FormatCachedRendererDebugSummary(char* buf, std::size_t bufSize) const {
    if (!buf || bufSize == 0) {
        return;
    }
    buf[0] = '\0';
    if (!ImplData) {
        return;
    }
    const auto& r = ImplData->CachedOptions.Renderer;
    std::snprintf(
        buf,
        bufSize,
        "backend=%u dev=%p cq=%p heap=%p h1=%p h2=%p fmt=%d frames=%d opt=%d ui=%d",
        static_cast<unsigned>(r.Backend),
        r.NativeDevice,
        r.NativeCommandQueue,
        r.RendererResource0,
        r.RendererResource1,
        r.RendererResource2,
        r.ColorFormat,
        r.NumFramesInFlight,
        ImplData->OptionsSet.load() ? 1 : 0,
        ImplData->UiVisible.load() ? 1 : 0);
}

// -------------------------------------------------------------------------------------------------
// C ABI wrappers (avoid C++ ABI mismatch between MinGW-built native libs and MSVC-built Unreal module)
// -------------------------------------------------------------------------------------------------
#if defined(_WIN32)
extern "C" {

SmatchetImGuiHostHandle SmatchetHost_Create() {
    return reinterpret_cast<SmatchetImGuiHostHandle>(new SmatchetImGuiHost());
}

void SmatchetHost_Destroy(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    delete h;
}

void SmatchetHost_SetInitOptions(
    SmatchetImGuiHostHandle host,
    const char* dbPathUtf8,
    const char* backendTypeUtf8,
    int mcpPort,
    int rendererBackend,
    int numFramesInFlight,
    int colorFormat,
    void* nativeDevice,
    void* rendererResource0,
    void* rendererResource1,
    void* rendererResource2,
    void* nativeCommandQueue,
    SmatchetOpenUrlFn openUrlFn,
    void* openUrlUserData,
    SmatchetAttachmentViewerFn attachmentViewerFn,
    void* attachmentViewerUserData) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;

    SmatchetImGuiHost::InitOptions opts;
    opts.DbPath = dbPathUtf8 ? std::string(dbPathUtf8) : std::string();
    opts.BackendType = backendTypeUtf8 ? std::string(backendTypeUtf8) : std::string();
    opts.McpPort = mcpPort;
    opts.Renderer.Backend = static_cast<SmatchetRendererBackend>(rendererBackend);
    opts.Renderer.NumFramesInFlight = numFramesInFlight;
    opts.Renderer.ColorFormat = colorFormat;
    opts.Renderer.NativeDevice = nativeDevice;
    opts.Renderer.NativeCommandQueue = nativeCommandQueue;
    opts.Renderer.RendererResource0 = rendererResource0;
    opts.Renderer.RendererResource1 = rendererResource1;
    opts.Renderer.RendererResource2 = rendererResource2;

    if (openUrlFn) {
        opts.OpenUrlHandler = [openUrlFn, openUrlUserData](const std::string& urlUtf8) {
            openUrlFn(urlUtf8.c_str(), openUrlUserData);
        };
    }

    if (attachmentViewerFn) {
        opts.AttachmentViewerHandler = [attachmentViewerFn, attachmentViewerUserData](
            const std::string& localPathUtf8,
            const std::string& mimeTypeUtf8,
            const std::string& filenameUtf8) {
            attachmentViewerFn(localPathUtf8.c_str(),
                                mimeTypeUtf8.c_str(),
                                filenameUtf8.c_str(),
                                attachmentViewerUserData);
        };
    }

    h->SetInitOptions(opts);
}

bool SmatchetHost_UpdateRendererColorFormat(SmatchetImGuiHostHandle host, int colorFormat) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return false;
    std::string err;
    return h->UpdateRendererColorFormat(colorFormat, err);
}

void SmatchetHost_SetUiVisible(SmatchetImGuiHostHandle host, bool visible) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->SetUiVisible(visible);
}

void SmatchetHost_ToggleUiVisible(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->ToggleUiVisible();
}

bool SmatchetHost_IsUiVisible(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return false;
    return h->IsUiVisible();
}

bool SmatchetHost_IsInitialized(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return false;
    return h->IsInitialized();
}

bool SmatchetHost_IsFrameActive(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return false;
    return h->IsFrameActive();
}

void SmatchetHost_BeginFrame(
    SmatchetImGuiHostHandle host,
    float deltaTimeSeconds,
    float viewportWidth,
    float viewportHeight) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->BeginFrame(deltaTimeSeconds, viewportWidth, viewportHeight);
}

void SmatchetHost_DrawUI(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->DrawUI();
}

void SmatchetHost_RenderDrawData(SmatchetImGuiHostHandle host, int rendererBackend, void* nativeCommandList) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->RenderDrawData(static_cast<SmatchetRendererBackend>(rendererBackend), nativeCommandList);
}

void SmatchetHost_SetMousePosition(SmatchetImGuiHostHandle host, float x, float y) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->SetMousePosition(x, y);
}

void SmatchetHost_SetMouseButton(SmatchetImGuiHostHandle host, int button, bool isDown) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->SetMouseButton(button, isDown);
}

void SmatchetHost_AddMouseWheel(SmatchetImGuiHostHandle host, float wheelX, float wheelY) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->AddMouseWheel(wheelX, wheelY);
}

void SmatchetHost_SetKeyDown(SmatchetImGuiHostHandle host, int imguiKey, bool isDown) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->SetKeyDown(imguiKey, isDown);
}

void SmatchetHost_SetKeyModifiers(SmatchetImGuiHostHandle host,
                                    bool ctrl,
                                    bool shift,
                                    bool alt,
                                    bool superKey) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->SetKeyModifiers(ctrl, shift, alt, superKey);
}

void SmatchetHost_AddInputCharacter(SmatchetImGuiHostHandle host, unsigned int character) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) return;
    h->AddInputCharacter(character);
}

} // extern "C"
#endif
