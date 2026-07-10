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
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "imgui.h"
#include "Logger.h"
#include "imgui_impl_dx12.h"
#include "SmatchetImGuiFonts.h"
#endif

// Smatchet core components (needed for the pImpl state).
#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "GridContextDepsAdapter.h"
#include "Json/BoundedJsonParse.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "ConfigManager.h"
#include "ITrackerBackendFactory.h"
#include "LuaAutomationHost.h"
#include "OfflineQueueService.h"
#include "PluginHost.h"
#include "SmatchetUI.h"
#include "TicketSyncService.h"
#if defined(SMATCHET_WITH_MCP)
#include "McpPlugin.h"
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
#include "LuaConsolePlugin.h"
#endif
#include "SmatchetInputModifierBridge.h"
#include "SmatchetTheme.h"
#include "Ui/SmatchetTooltipWheelRouter.h"

namespace {

void PushImGuiModifierKeys(ImGuiIO& io, bool ctrl, bool shift, bool alt, bool superKey) {
    io.AddKeyEvent(ImGuiKey_LeftCtrl, ctrl);
    io.AddKeyEvent(ImGuiKey_RightCtrl, ctrl);
    io.AddKeyEvent(ImGuiKey_LeftShift, shift);
    io.AddKeyEvent(ImGuiKey_RightShift, shift);
    io.AddKeyEvent(ImGuiKey_LeftAlt, alt);
    io.AddKeyEvent(ImGuiKey_RightAlt, alt);
    io.AddKeyEvent(ImGuiKey_LeftSuper, superKey);
    io.AddKeyEvent(ImGuiKey_RightSuper, superKey);
}

// Unreal feeds keys on the Slate thread after render-thread NewFrame; io.KeyCtrl lags KeysData by a
// frame. InputText paste uses Shortcut(ImGuiMod_Ctrl|V) which reads io.KeyMods, not IsKeyDown(Ctrl).
void SyncImGuiIoModifierFlagsFromKeys(ImGuiIO& io) {
#if defined(_WIN32)
    const bool asyncCtrl =
        ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
    const bool asyncShift =
        ((::GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
    const bool asyncAlt =
        ((::GetAsyncKeyState(VK_LMENU) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RMENU) & 0x8000) != 0);
#else
    const bool asyncCtrl = false;
    const bool asyncShift = false;
    const bool asyncAlt = false;
#endif
    const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl) ||
                      SmatchetInput_GetPluginModifiersPushedCtrl() || asyncCtrl;
    const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift) ||
                       SmatchetInput_GetPluginModifiersPushedShift() || asyncShift;
    const bool alt = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt) || asyncAlt;
    const bool superKey = ImGui::IsKeyDown(ImGuiKey_LeftSuper) || ImGui::IsKeyDown(ImGuiKey_RightSuper);
    io.KeyCtrl = ctrl;
    io.KeyShift = shift;
    io.KeyAlt = alt;
    io.KeySuper = superKey;
    io.KeyMods = (ctrl ? ImGuiMod_Ctrl : 0) | (shift ? ImGuiMod_Shift : 0) | (alt ? ImGuiMod_Alt : 0) |
                 (superKey ? ImGuiMod_Super : 0);
}

constexpr std::size_t kMaxPendingHostCommands = 128;
constexpr std::size_t kMaxCompletedHostCommandResults = 128;
constexpr std::size_t kMaxCommandsPerHostTick = 32;

struct PendingHostCommand {
    std::uint64_t RequestId = 0;
    std::string CommandName;
    std::string ArgsJson;
    bool ConfirmedDestructive = false;
    bool DryRun = false;
};

std::string MakeCommandFailureResultJson(const std::string& commandName, smatchet::cmd::ErrorCode code,
                                         const std::string& message, const std::string& hint, bool dryRun) {
    smatchet::cmd::CommandResult r = smatchet::cmd::CommandResult::Failure(code, message, hint);
    return r.ToWireJson(commandName, dryRun).dump();
}

void StoreCompletedHostCommandResult(std::unordered_map<std::uint64_t, std::string>& completed,
                                     std::deque<std::uint64_t>& order, std::uint64_t requestId,
                                     std::string resultJson) {
    if (completed.find(requestId) == completed.end()) {
        order.push_back(requestId);
    }
    completed[requestId] = std::move(resultJson);
    while (completed.size() > kMaxCompletedHostCommandResults && !order.empty()) {
        const std::uint64_t oldId = order.front();
        order.pop_front();
        completed.erase(oldId);
    }
}

} // namespace

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
    std::atomic<bool> SuppressSoftwareCursor{false};

    SmatchetImGuiHost::InitOptions CachedOptions;

    // ImGui uses a thread-local "current context" (TLS). Unreal calls into this host
    // from different threads (Slate/input vs render). We store the created context and
    // re-attach it on each thread before touching ImGui.
    ImGuiContext* ImGuiCtx = nullptr;

    AppController App;
    PluginHost Plugins;
    SmatchetUI Ui;
    std::string ImGuiIniPath; // cppcheck-suppress unusedStructMember -- WIN32 init path assigns io.IniFilename below

    // Serialize all ImGui API calls across threads (IO updates, NewFrame, Render, etc).
    std::mutex ImGuiMutex;

    // Backend resource used by renderer implementation.
    // ImGui_ImplDX12_RenderDrawData() does not bind descriptor heaps, so we store it
    // and bind it right before rendering.
    void* RendererResource0 = nullptr;

    // Throttle failed init retries when UI is visible but DX resources aren't ready yet.
    std::atomic<std::uint64_t> LastInitAttemptMs{0};

    std::atomic<std::uint64_t> NextCommandRequestId{1};
    mutable std::mutex CommandMutex;
    std::deque<PendingHostCommand> PendingCommands;
    std::unordered_map<std::uint64_t, std::string> CompletedCommandResults;
    std::deque<std::uint64_t> CompletedCommandOrder;

    std::string LastInitError;
};

#if defined(_WIN32)
namespace {

std::string SmatchetDirectoryFromFilePath(const std::string& path) {
    const std::string::size_type last = path.find_last_of("\\/");
    if (last == std::string::npos) {
        return std::string();
    }
    return path.substr(0, last + 1);
}

} // namespace

// Shader-visible SRV heap allocator state used by the ImGui DX12 backend when creating dynamic
// ImTextureData textures (attachment thumbnails, etc.). Slot 0 stays reserved for the legacy font
// atlas path; additional slots are allocated/freed with a tiny free-list.
struct Smatchet_ImplDX12_SrvAllocator {
    D3D12_CPU_DESCRIPTOR_HANDLE CpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE GpuBase{};
    UINT IncrementSize = 0;
    int Capacity = 0;          // total descriptors in heap, including slot 0 (reserved for font)
    int OverflowSlot = -1;     // reserved non-font slot used only on exhaustion
    int NextSlot = 1;          // next fresh non-overflow slot; typically 1..OverflowSlot-1
    std::vector<int> FreeList; // returned slots available for reuse (LIFO)
    bool bLoggedExhausted = false;
    std::mutex Mutex;
};

static Smatchet_ImplDX12_SrvAllocator gSmatchetDx12SrvAllocator;

static void Smatchet_ImplDX12_SrvAllocCb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                         D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    Smatchet_ImplDX12_SrvAllocator& a = gSmatchetDx12SrvAllocator;
    std::lock_guard<std::mutex> lk(a.Mutex);
    int slot = -1;
    if (!a.FreeList.empty()) {
        slot = a.FreeList.back();
        a.FreeList.pop_back();
    } else if (a.OverflowSlot > 0 ? (a.NextSlot < a.OverflowSlot) : (a.NextSlot < a.Capacity)) {
        slot = a.NextSlot++;
    }
    if (slot < 0) {
        // Never reuse slot 0 (font atlas). Spill exhausted allocations into a dedicated non-font
        // overflow slot so extra thumbnails may alias each other but cannot corrupt text/icons.
        slot = (a.OverflowSlot > 0) ? a.OverflowSlot : 0;
        if (!a.bLoggedExhausted) {
            LOG_WARN("SmatchetImGuiHost: DX12 SRV heap exhausted (capacity=%d, overflowSlot=%d). Extra thumbnails may "
                     "alias.",
                     a.Capacity, a.OverflowSlot);
            a.bLoggedExhausted = true;
        }
    }
    if (outCpu) {
        outCpu->ptr = a.CpuBase.ptr + static_cast<SIZE_T>(slot) * a.IncrementSize;
    }
    if (outGpu) {
        outGpu->ptr = a.GpuBase.ptr + static_cast<UINT64>(slot) * a.IncrementSize;
    }
}

static void Smatchet_ImplDX12_SrvFreeCb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
    Smatchet_ImplDX12_SrvAllocator& a = gSmatchetDx12SrvAllocator;
    std::lock_guard<std::mutex> lk(a.Mutex);
    if (a.IncrementSize == 0 || a.Capacity <= 0 || cpu.ptr < a.CpuBase.ptr) {
        return;
    }
    const SIZE_T offset = cpu.ptr - a.CpuBase.ptr;
    const int slot = static_cast<int>(offset / a.IncrementSize);
    if (slot > 0 && slot < a.Capacity && slot != a.OverflowSlot) {
        a.FreeList.push_back(slot);
    }
}

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

    // Configure the dynamic-SRV allocator. When NumSrvDescriptors <= 1 we only expose the legacy
    // single-SRV path so attachment thumbnails will remain blank — this matches the pre-allocator
    // behaviour and logs a warning.
    {
        Smatchet_ImplDX12_SrvAllocator& a = gSmatchetDx12SrvAllocator;
        std::lock_guard<std::mutex> lk(a.Mutex);
        a.CpuBase = fontSrvCpuHandle;
        a.GpuBase = fontSrvGpuHandle;
        a.IncrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        a.Capacity = renderer.NumSrvDescriptors > 0 ? renderer.NumSrvDescriptors : 1;
        a.OverflowSlot = (a.Capacity > 1) ? (a.Capacity - 1) : -1;
        a.NextSlot = 1;
        a.FreeList.clear();
        a.bLoggedExhausted = false;
        if (a.Capacity <= 1) {
            LOG_WARN("SmatchetImGuiHost: DX12 SRV heap has %d slot(s); attachment thumbnails and other dynamic "
                     "textures will not render. Pass NumSrvDescriptors>=2 in SmatchetRendererInitInfo.",
                     a.Capacity);
        }
    }

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = device;
    initInfo.CommandQueue = commandQueue;
    initInfo.NumFramesInFlight = renderer.NumFramesInFlight;
    initInfo.RTVFormat = static_cast<DXGI_FORMAT>(renderer.ColorFormat);
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = fontSrvDescriptorHeap;
    initInfo.LegacySingleSrvCpuDescriptor = fontSrvCpuHandle;
    initInfo.LegacySingleSrvGpuDescriptor = fontSrvGpuHandle;
    if (gSmatchetDx12SrvAllocator.Capacity > 1) {
        initInfo.SrvDescriptorAllocFn = &Smatchet_ImplDX12_SrvAllocCb;
        initInfo.SrvDescriptorFreeFn = &Smatchet_ImplDX12_SrvFreeCb;
    }

    {
        char dbg[640];
        std::snprintf(
            dbg, sizeof(dbg),
            "[Smatchet] ImGui_ImplDX12_Init(ptr): dev=%p cq=%p heap=%p cpu=%llu gpu=%llu dxgi_fmt=%d nframes=%d\n",
            static_cast<void*>(device), static_cast<void*>(commandQueue), static_cast<void*>(fontSrvDescriptorHeap),
            static_cast<unsigned long long>(fontSrvCpuHandle.ptr),
            static_cast<unsigned long long>(fontSrvGpuHandle.ptr), static_cast<int>(renderer.ColorFormat),
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

SmatchetImGuiHost::SmatchetImGuiHost() : ImplData(std::make_unique<Impl>()) {}

SmatchetImGuiHost::~SmatchetImGuiHost() { Shutdown(); }

bool SmatchetImGuiHost::IsUiVisible() const { return ImplData && ImplData->UiVisible.load(std::memory_order_relaxed); }

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

void SmatchetImGuiHost::ToggleUiVisible() { SetUiVisible(!IsUiVisible()); }

void SmatchetImGuiHost::SetSuppressSoftwareCursor(bool suppress) {
    if (!ImplData) {
        return;
    }
    ImplData->SuppressSoftwareCursor.store(suppress, std::memory_order_relaxed);
}

bool SmatchetImGuiHost::GetSuppressSoftwareCursor() const {
    if (!ImplData) {
        return false;
    }
    return ImplData->SuppressSoftwareCursor.load(std::memory_order_relaxed);
}

void SmatchetImGuiHost::SetInitOptions(const InitOptions& options) {
    if (!ImplData) {
        return;
    }
    ImplData->CachedOptions = options;
    ImplData->OptionsSet.store(true, std::memory_order_release);
}

void SmatchetImGuiHost::SetRendererNumSrvDescriptors(int numSrvDescriptors) {
    if (!ImplData) {
        return;
    }
    ImplData->CachedOptions.Renderer.NumSrvDescriptors = numSrvDescriptors > 0 ? numSrvDescriptors : 1;
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
        // Backend is now torn down. Without clearing Initialized, every gated
        // path (DrawUI / RenderDrawData / NewFrame) would happily call into a
        // dead backend on subsequent frames. Flip the flag so BuildFrame's
        // lazy-retry path (~once per second while the UI is visible) will
        // re-attempt Initialize(), and surface the error via LastInitError
        // for the host's diagnostic accessors.
        ImplData->Initialized.store(false, std::memory_order_release);
        ImplData->LastInitError = outError;
        LOG_ERROR("SmatchetImGuiHost::UpdateRendererColorFormat backend re-init failed: %s", outError.c_str());
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

#if defined(_WIN32)
// Resolve the runtime asset + user-data directories from the DB path on first init. Plugin defaults
// to the Portable per-Unreal-project preference to preserve historical config-file behaviour. The
// user can opt into Shared mode via Preferences then Local data.
void SmatchetImGuiHost::initResolveStorageDirs(const InitOptions& options) {
    if (ConfigManager::GetFilesBaseDirectory().empty()) {
        const std::string baseFromDbPath = SmatchetDirectoryFromFilePath(options.DbPath);
        if (!baseFromDbPath.empty()) {
            ConfigManager::SetRuntimeAssetDirectory(baseFromDbPath);
            const ConfigManager::StoragePreference pref =
                ConfigManager::GetStoragePreference(baseFromDbPath, ConfigManager::StoragePreference::Portable);
            std::string userDataDir = baseFromDbPath;
            if (pref == ConfigManager::StoragePreference::Shared) {
                const std::string shared = ConfigManager::GetPlatformSharedUserDataDirectory();
                if (!shared.empty()) {
                    userDataDir = shared;
                }
            }
            ConfigManager::SetUserDataDirectory(userDataDir);
        }
    }
}

// Validate the renderer backend + required DX12 resources. Returns false (and sets outError +
// LastInitError) when the backend is not DX12 or any required handle is missing.
bool SmatchetImGuiHost::initValidateRenderer(const InitOptions& options, std::string& outError) {
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
    return true;
}

// Create the ImGui context, run the layout-schema ini migration, apply the initial theme + font,
// init the DX12 backend, and warm up the font atlas. Returns false (destroying the context) on
// backend init failure. Preconditions validated by initValidateRenderer().
bool SmatchetImGuiHost::initImGuiContextAndBackend(const InitOptions& options, std::string& outError) {
    auto* device = reinterpret_cast<ID3D12Device*>(options.Renderer.NativeDevice);
    auto* fontSrvDescriptorHeap = reinterpret_cast<ID3D12DescriptorHeap*>(options.Renderer.RendererResource0);
    D3D12_CPU_DESCRIPTOR_HANDLE fontSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE fontSrvGpuHandle{};
    fontSrvCpuHandle.ptr = static_cast<SIZE_T>(reinterpret_cast<std::uintptr_t>(options.Renderer.RendererResource1));
    fontSrvGpuHandle.ptr = static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(options.Renderer.RendererResource2));

    IMGUI_CHECKVERSION();
    // Diagnostics: if these are invalid/dangling, ImGui's font atlas upload can crash in NewFrame.
    LOG_DEBUG("SmatchetImGuiHost::Initialize: device=%p queue=%p heap=%p cpu.ptr=%llu gpu.ptr=%llu",
              static_cast<void*>(device), options.Renderer.NativeCommandQueue,
              static_cast<void*>(fontSrvDescriptorHeap), static_cast<unsigned long long>(fontSrvCpuHandle.ptr),
              static_cast<unsigned long long>(fontSrvGpuHandle.ptr));
    {
        std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
        ImGui::CreateContext();
        ImplData->ImGuiCtx = ImGui::GetCurrentContext();
    }
    ImGuiIO& io = ImGui::GetIO();
    ImplData->ImGuiIniPath = ConfigManager::GetImGuiSettingsPath();
    // Schema migration MUST run before ImGui auto-loads the ini, otherwise
    // already-created windows stay docked at the old positions and runtime
    // LoadIniSettingsFromDisk does not re-parent them. Load cfg, compare schema,
    // force-write the hardcoded default + persist new schema, then let ImGui
    // pick up the fresh file via io.IniFilename.
    {
        TrackerConfig bootCfg = ConfigManager::Load();
        if (bootCfg.LayoutSchemaVersion < ConfigManager::kCurrentLayoutSchemaVersion) {
            ConfigManager::WriteDefaultImGuiSettingsFile();
            bootCfg.LayoutSchemaVersion = ConfigManager::kCurrentLayoutSchemaVersion;
            ConfigManager::Save(bootCfg);
        } else {
            ConfigManager::EnsureDefaultImGuiSettingsFile();
        }
    }
    io.IniFilename = ImplData->ImGuiIniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    // Initial palette before cfg is loaded; SmatchetUI::Draw re-applies once cfg is available.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
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
        ImGuiIO& warmupIo = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        warmupIo.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height); // Force atlas build / builder init.
    }
    return true;
}

// Register optional plugins (MCP, Lua console) per build flags + config. Mirrors the
// pre-decomposition inline order verbatim.
void SmatchetImGuiHost::initRegisterPlugins(const InitOptions& options, const TrackerConfig& cfg) {
    (void)options;
    (void)cfg;
#if defined(SMATCHET_WITH_MCP)
    {
        const int mcpPort = (cfg.McpPort >= 1 && cfg.McpPort <= 65535) ? cfg.McpPort : options.McpPort;
        if (cfg.McpEnabled) {
            ImplData->Plugins.Register(std::make_unique<McpPlugin>(mcpPort));
            LOG_INFO("SmatchetImGuiHost: MCP plugin enabled on port %d", mcpPort);
        } else {
            LOG_INFO("SmatchetImGuiHost: MCP plugin disabled by config.");
        }
    }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    ImplData->Plugins.Register(std::make_unique<LuaConsolePlugin>());
    LOG_INFO("SmatchetImGuiHost: LuaConsole plugin registered.");
#if !defined(SMATCHET_WITH_MCP)
    LOG_INFO("SmatchetImGuiHost: light profile (Lua + command registry; MCP/AI/Whisper off).");
#endif
#else
    LOG_WARN("SmatchetImGuiHost: built without SMATCHET_WITH_LUA_AUTOMATION — LuaConsole not loaded.");
#endif
}
#endif

bool SmatchetImGuiHost::Initialize(const InitOptions& options, std::string& outError) {
    outError.clear();
    if (!ImplData) {
        outError = "Host state is unavailable.";
        return false;
    }
    ImplData->LastInitError.clear();
    if (ImplData->Initialized.load(std::memory_order_acquire)) {
        return true;
    }

#if !defined(_WIN32)
    outError = "SmatchetImGuiHost renderer backend is unavailable on this platform.";
    ImplData->LastInitError = outError;
    LOG_ERROR("SmatchetImGuiHost::Initialize failed: %s", outError.c_str());
    return false;
#else
    initResolveStorageDirs(options);

    if (!initValidateRenderer(options, outError)) {
        return false;
    }

    if (!initImGuiContextAndBackend(options, outError)) {
        return false;
    }

    TrackerConfig cfg = ConfigManager::Load();

    ImplData->App.SetOpenUrlHandler(options.OpenUrlHandler);
    ImplData->App.SetAttachmentViewerHandler(options.AttachmentViewerHandler);
    ImplData->App.SetCloseEmbeddedUiHandler([this]() { SetUiVisible(false); });

    initRegisterPlugins(options, cfg);
    ImplData->Plugins.OnEarlyInit(ImplData->App);

    std::string resolvedDbPath = options.DbPath;
    if (cfg.DbPath != SmatchetDefaults::kDefaultDbPath) {
        resolvedDbPath = cfg.DbPath;
    }
    std::string resolvedBackend = options.BackendType;
    if (cfg.TrackerType != SmatchetDefaults::kDefaultBackendType) {
        resolvedBackend = cfg.TrackerType;
    }

    ImplData->App.Initialize(resolvedDbPath, resolvedBackend);
    ImplData->Plugins.OnStart(ImplData->App);
    ImplData->App.SetRuntimePluginHost(&ImplData->Plugins);

    ImplData->Initialized.store(true, std::memory_order_release);
    ImplData->LastInitError.clear();
    return true;
#endif
}

void SmatchetImGuiHost::Shutdown() {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }

    ImplData->App.SetCloseEmbeddedUiHandler({});
    ImplData->App.SetRuntimePluginHost(nullptr);
    // Sinks may capture `this` of plugin instances that are about to be destroyed;
    // drop them before tearing down plugins so any late callers fall back to stdio.
    ImplData->App.ClearAutomationLogSinks();

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
    if (!ImplData)
        return;

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

    if (SmatchetCheckAndApplyFontReload()) {
        ImGui_ImplDX12_InvalidateDeviceObjects();
        ImGui_ImplDX12_CreateDeviceObjects();
    }

    ImGui_ImplDX12_NewFrame();
    smatchet::ui::RouteWheelToScrollableTooltipBeforeNewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = (deltaTimeSeconds > 0.0f) ? deltaTimeSeconds : (1.0f / 60.0f);
    io.DisplaySize = ImVec2(viewportWidth, viewportHeight);

    ImGui::NewFrame();

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
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

    DrainCommandQueue(kMaxCommandsPerHostTick);

    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }

    ImGuiIO& io = ImGui::GetIO();
    SyncImGuiIoModifierFlagsFromKeys(io);

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
    {
        // Avoid spamming logs: at most once per 1s.
        // (Use std::chrono instead of UE time here; it's fine for diagnostics.)
        static double LastDrawStatsLogSeconds = 0.0;
        using clock = std::chrono::steady_clock;
        const double nowSeconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(clock::now().time_since_epoch()).count();
        if (nowSeconds - LastDrawStatsLogSeconds > 1.0) {
            LastDrawStatsLogSeconds = nowSeconds;
            if (drawData) {
                LOG_DEBUG("SmatchetImGuiHost::RenderDrawData: display=(%f,%f) totalVtx=%d cmds=%d ui=%d init=%d",
                          drawData->DisplaySize.x, drawData->DisplaySize.y, drawData->TotalVtxCount,
                          drawData->CmdListsCount, IsUiVisible() ? 1 : 0,
                          ImplData->Initialized.load(std::memory_order_relaxed) ? 1 : 0);
            } else {
                LOG_DEBUG("SmatchetImGuiHost::RenderDrawData: drawData=null");
            }
        }
    }

    if (drawData) {
        if (ImplData->RendererResource0) {
            ID3D12DescriptorHeap* heaps[] = {reinterpret_cast<ID3D12DescriptorHeap*>(ImplData->RendererResource0)};
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
    PushImGuiModifierKeys(io, ctrl, shift, alt, superKey);
    SmatchetInput_SetPluginModifiersPushed(ctrl, shift);
}

void SmatchetImGuiHost::ApplyKeyChordDown(int imguiKey, bool ctrl, bool shift, bool alt, bool superKey) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    PushImGuiModifierKeys(io, ctrl, shift, alt, superKey);
    if (imguiKey >= 0) {
        io.AddKeyEvent(static_cast<ImGuiKey>(imguiKey), true);
    }
    SmatchetInput_SetPluginModifiersPushed(ctrl, shift);
}

void SmatchetImGuiHost::ApplyKeyChordUp(int imguiKey, bool ctrl, bool shift, bool alt, bool superKey) {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ImplData->ImGuiMutex);
    if (ImplData->ImGuiCtx) {
        ImGui::SetCurrentContext(ImplData->ImGuiCtx);
    }
    ImGuiIO& io = ImGui::GetIO();
    PushImGuiModifierKeys(io, ctrl, shift, alt, superKey);
    if (imguiKey >= 0) {
        io.AddKeyEvent(static_cast<ImGuiKey>(imguiKey), false);
    }
    SmatchetInput_SetPluginModifiersPushed(ctrl, shift);
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

void SmatchetImGuiHost::TickApplicationWork() {
    if (!ImplData || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }
    DrainCommandQueue(kMaxCommandsPerHostTick);
    ImplData->App.TickOfflineCreates();
    ImplData->App.TickOfflineFieldEdits();
    ImplData->App.TickAllContexts(); // all live pane contexts (multi-grid Slice 3)
}

std::uint64_t SmatchetImGuiHost::EnqueueCommand(const std::string& commandName, const std::string& argsJson,
                                                bool confirmedDestructive, bool dryRun) {
    if (!ImplData) {
        return 0;
    }

    std::uint64_t requestId = ImplData->NextCommandRequestId.fetch_add(1, std::memory_order_relaxed);
    if (requestId == 0) {
        requestId = ImplData->NextCommandRequestId.fetch_add(1, std::memory_order_relaxed);
    }

    PendingHostCommand req;
    req.RequestId = requestId;
    req.CommandName = commandName;
    req.ArgsJson = argsJson;
    req.ConfirmedDestructive = confirmedDestructive;
    req.DryRun = dryRun;

    std::lock_guard<std::mutex> lock(ImplData->CommandMutex);
    if (commandName.empty()) {
        StoreCompletedHostCommandResult(
            ImplData->CompletedCommandResults, ImplData->CompletedCommandOrder, requestId,
            MakeCommandFailureResultJson(commandName, smatchet::cmd::ErrorCode::ValidationError,
                                         "Command name is required.", std::string(), dryRun));
        return requestId;
    }
    if (ImplData->PendingCommands.size() >= kMaxPendingHostCommands) {
        StoreCompletedHostCommandResult(
            ImplData->CompletedCommandResults, ImplData->CompletedCommandOrder, requestId,
            MakeCommandFailureResultJson(commandName, smatchet::cmd::ErrorCode::Timeout,
                                         "Unreal command queue is full.",
                                         "Poll existing command results before enqueueing more requests.", dryRun));
        return requestId;
    }

    ImplData->PendingCommands.push_back(std::move(req));
    return requestId;
}

bool SmatchetImGuiHost::IsCommandResultReady(std::uint64_t requestId) const {
    if (!ImplData || requestId == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ImplData->CommandMutex);
    return ImplData->CompletedCommandResults.find(requestId) != ImplData->CompletedCommandResults.end();
}

bool SmatchetImGuiHost::TakeCommandResultJson(std::uint64_t requestId, std::string& outJson) {
    outJson.clear();
    if (!ImplData || requestId == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ImplData->CommandMutex);
    const auto it = ImplData->CompletedCommandResults.find(requestId);
    if (it == ImplData->CompletedCommandResults.end()) {
        return false;
    }
    outJson = std::move(it->second);
    ImplData->CompletedCommandResults.erase(it);
    const auto orderIt =
        std::find(ImplData->CompletedCommandOrder.begin(), ImplData->CompletedCommandOrder.end(), requestId);
    if (orderIt != ImplData->CompletedCommandOrder.end()) {
        ImplData->CompletedCommandOrder.erase(orderIt);
    }
    return true;
}

void SmatchetImGuiHost::DrainCommandQueue(std::size_t maxCount) {
    if (!ImplData || maxCount == 0 || !ImplData->Initialized.load(std::memory_order_acquire)) {
        return;
    }

    for (std::size_t processed = 0; processed < maxCount; ++processed) {
        PendingHostCommand req;
        {
            std::lock_guard<std::mutex> lock(ImplData->CommandMutex);
            if (ImplData->PendingCommands.empty()) {
                return;
            }
            req = std::move(ImplData->PendingCommands.front());
            ImplData->PendingCommands.pop_front();
        }

        std::string resultJson;
        try {
            nlohmann::json args = nlohmann::json::object();
            if (!req.ArgsJson.empty()) {
                std::string parseErr;
                args = smatchet::json_safe::ParseBounded(req.ArgsJson, parseErr);
                if (!parseErr.empty()) {
                    resultJson = MakeCommandFailureResultJson(
                        req.CommandName, smatchet::cmd::ErrorCode::ValidationError,
                        std::string("Command arguments must be valid JSON: ") + parseErr, std::string(), req.DryRun);
                } else if (!args.is_object()) {
                    resultJson = MakeCommandFailureResultJson(
                        req.CommandName, smatchet::cmd::ErrorCode::ValidationError,
                        "Command arguments must be a JSON object.", std::string(), req.DryRun);
                }
            }

            if (resultJson.empty()) {
                smatchet::cmd::CommandContext ctx;
                ctx.ScenarioHost = &ImplData->App;
                ctx.Threading = &ImplData->App;
                ctx.Source = smatchet::cmd::CommandSource::Unreal;
                ctx.ConfirmedDestructive = req.ConfirmedDestructive;
                ctx.DryRun = req.DryRun;

                const smatchet::cmd::CommandResult result =
                    ImplData->App.Commands().Dispatch(req.CommandName, args, ctx);
                resultJson = result.ToWireJson(req.CommandName, req.DryRun).dump();
            }
        } catch (const std::exception& ex) {
            // ParseBounded never throws (failures surface via parseErr above), so any
            // exception reaching here came from Dispatch/ToWireJson — a handler-side
            // failure, not malformed arguments. Report it as such.
            resultJson = MakeCommandFailureResultJson(req.CommandName, smatchet::cmd::ErrorCode::HandlerError,
                                                      std::string("Command dispatch failed: ") + ex.what(),
                                                      std::string(), req.DryRun);
        } catch (...) { // catch-all-ok: unknown command failure is returned as JSON.
            resultJson = MakeCommandFailureResultJson(req.CommandName, smatchet::cmd::ErrorCode::HandlerError,
                                                      "Command dispatch failed with an unknown exception.",
                                                      std::string(), req.DryRun);
        }

        {
            std::lock_guard<std::mutex> lock(ImplData->CommandMutex);
            StoreCompletedHostCommandResult(ImplData->CompletedCommandResults, ImplData->CompletedCommandOrder,
                                            req.RequestId, std::move(resultJson));
        }
    }
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
    std::snprintf(buf, bufSize, "backend=%u dev=%p cq=%p heap=%p h1=%p h2=%p fmt=%d frames=%d opt=%d ui=%d",
                  static_cast<unsigned>(r.Backend), r.NativeDevice, r.NativeCommandQueue, r.RendererResource0,
                  r.RendererResource1, r.RendererResource2, r.ColorFormat, r.NumFramesInFlight,
                  ImplData->OptionsSet.load() ? 1 : 0, ImplData->UiVisible.load() ? 1 : 0);
}

// C ABI wrappers (avoid C++ ABI mismatch between MinGW-built native libs and MSVC-built Unreal module)
#if defined(_WIN32)
namespace {
std::mutex gHostHandleSetMutex;
std::unordered_set<SmatchetImGuiHost*> gLiveHostHandles;

/// Safe handle resolution: checks gLiveHostHandles so stale handles from Unreal hot-reload
/// or use-after-Destroy return nullptr rather than dereferencing freed memory.
SmatchetImGuiHost* LookupHost(SmatchetImGuiHostHandle handle) {
    if (!handle)
        return nullptr;
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(handle);
    std::lock_guard<std::mutex> lock(gHostHandleSetMutex);
    return gLiveHostHandles.count(h) ? h : nullptr;
}
} // namespace

extern "C" {

SmatchetImGuiHostHandle SmatchetHost_Create() {
    auto* host = new SmatchetImGuiHost(); // C-ABI handle — raw pointer is the public contract
    {
        std::lock_guard<std::mutex> lock(gHostHandleSetMutex);
        gLiveHostHandles.insert(host);
    }
    return reinterpret_cast<SmatchetImGuiHostHandle>(host);
}

void SmatchetHost_Destroy(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gHostHandleSetMutex);
        const auto it = gLiveHostHandles.find(h);
        if (it == gLiveHostHandles.end()) {
            LOG_WARN("SmatchetHost_Destroy: ignored stale/double-free handle.");
            return;
        }
        gLiveHostHandles.erase(it);
    }
    delete h;
}

void SmatchetHost_SetInitOptions(SmatchetImGuiHostHandle host, const char* dbPathUtf8, const char* backendTypeUtf8,
                                 int mcpPort, int rendererBackend, int numFramesInFlight, int colorFormat,
                                 void* nativeDevice, void* rendererResource0, void* rendererResource1,
                                 void* rendererResource2, void* nativeCommandQueue, SmatchetOpenUrlFn openUrlFn,
                                 void* openUrlUserData, SmatchetAttachmentViewerFn attachmentViewerFn,
                                 void* attachmentViewerUserData) {
    auto* h = LookupHost(host);
    if (!h)
        return;

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
        opts.AttachmentViewerHandler = [attachmentViewerFn, attachmentViewerUserData](const std::string& localPathUtf8,
                                                                                      const std::string& mimeTypeUtf8,
                                                                                      const std::string& filenameUtf8) {
            attachmentViewerFn(localPathUtf8.c_str(), mimeTypeUtf8.c_str(), filenameUtf8.c_str(),
                               attachmentViewerUserData);
        };
    }

    h->SetInitOptions(opts);
}

bool SmatchetHost_UpdateRendererColorFormat(SmatchetImGuiHostHandle host, int colorFormat) {
    auto* h = LookupHost(host);
    if (!h)
        return false;
    std::string err;
    return h->UpdateRendererColorFormat(colorFormat, err);
}

void SmatchetHost_SetNumSrvDescriptors(SmatchetImGuiHostHandle host, int numSrvDescriptors) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->SetRendererNumSrvDescriptors(numSrvDescriptors);
}

SmatchetCommandRequestId SmatchetHost_EnqueueCommand(SmatchetImGuiHostHandle host, const char* commandNameUtf8,
                                                     const char* argsJsonUtf8, bool confirmedDestructive, bool dryRun) {
    auto* h = LookupHost(host);
    if (!h) {
        return 0;
    }
    return h->EnqueueCommand(commandNameUtf8 ? std::string(commandNameUtf8) : std::string(),
                             argsJsonUtf8 ? std::string(argsJsonUtf8) : std::string(), confirmedDestructive, dryRun);
}

bool SmatchetHost_IsCommandResultReady(SmatchetImGuiHostHandle host, SmatchetCommandRequestId requestId) {
    auto* h = LookupHost(host);
    if (!h) {
        return false;
    }
    return h->IsCommandResultReady(requestId);
}

char* SmatchetHost_TakeCommandResultJson(SmatchetImGuiHostHandle host, SmatchetCommandRequestId requestId) {
    auto* h = LookupHost(host);
    if (!h) {
        return nullptr;
    }
    std::string resultJson;
    if (!h->TakeCommandResultJson(requestId, resultJson)) {
        return nullptr;
    }
    const std::size_t byteCount = resultJson.size() + 1;
    char* out = static_cast<char*>(std::malloc(byteCount));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, resultJson.c_str(), byteCount);
    return out;
}

void SmatchetHost_ReleaseCommandResultJson(char* resultJsonUtf8) { std::free(resultJsonUtf8); }

void SmatchetHost_SetUiVisible(SmatchetImGuiHostHandle host, bool visible) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->SetUiVisible(visible);
}

void SmatchetHost_ToggleUiVisible(SmatchetImGuiHostHandle host) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->ToggleUiVisible();
}

void SmatchetHost_SetSuppressSoftwareCursor(SmatchetImGuiHostHandle host, bool suppress) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->SetSuppressSoftwareCursor(suppress);
}

bool SmatchetHost_GetSuppressSoftwareCursor(SmatchetImGuiHostHandle host) {
    auto* h = LookupHost(host);
    if (!h)
        return false;
    return h->GetSuppressSoftwareCursor();
}

bool SmatchetHost_IsUiVisible(SmatchetImGuiHostHandle host) {
    auto* h = LookupHost(host);
    if (!h)
        return false;
    return h->IsUiVisible();
}

bool SmatchetHost_IsInitialized(SmatchetImGuiHostHandle host) {
    auto* h = LookupHost(host);
    if (!h)
        return false;
    return h->IsInitialized();
}

bool SmatchetHost_IsFrameActive(SmatchetImGuiHostHandle host) {
    auto* h = LookupHost(host);
    if (!h)
        return false;
    return h->IsFrameActive();
}

void SmatchetHost_TickApplicationWork(SmatchetImGuiHostHandle host) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->TickApplicationWork();
}

void SmatchetHost_BeginFrame(SmatchetImGuiHostHandle host, float deltaTimeSeconds, float viewportWidth,
                             float viewportHeight) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->BeginFrame(deltaTimeSeconds, viewportWidth, viewportHeight);
}

void SmatchetHost_DrawUI(SmatchetImGuiHostHandle host) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->DrawUI();
}

void SmatchetHost_RenderDrawData(SmatchetImGuiHostHandle host, int rendererBackend, void* nativeCommandList) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->RenderDrawData(static_cast<SmatchetRendererBackend>(rendererBackend), nativeCommandList);
}

void SmatchetHost_SetMousePosition(SmatchetImGuiHostHandle host, float x, float y) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->SetMousePosition(x, y);
}

void SmatchetHost_SetMouseButton(SmatchetImGuiHostHandle host, int button, bool isDown) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->SetMouseButton(button, isDown);
}

void SmatchetHost_AddMouseWheel(SmatchetImGuiHostHandle host, float wheelX, float wheelY) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->AddMouseWheel(wheelX, wheelY);
}

void SmatchetHost_SetKeyDown(SmatchetImGuiHostHandle host, int imguiKey, bool isDown) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->SetKeyDown(imguiKey, isDown);
}

void SmatchetHost_SetKeyModifiers(SmatchetImGuiHostHandle host, bool ctrl, bool shift, bool alt, bool superKey) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->SetKeyModifiers(ctrl, shift, alt, superKey);
}

void SmatchetHost_ApplyKeyChordDown(SmatchetImGuiHostHandle host, int imguiKey, bool ctrl, bool shift, bool alt,
                                    bool superKey) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->ApplyKeyChordDown(imguiKey, ctrl, shift, alt, superKey);
}

void SmatchetHost_ApplyKeyChordUp(SmatchetImGuiHostHandle host, int imguiKey, bool ctrl, bool shift, bool alt,
                                  bool superKey) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->ApplyKeyChordUp(imguiKey, ctrl, shift, alt, superKey);
}

void SmatchetHost_AddInputCharacter(SmatchetImGuiHostHandle host, unsigned int character) {
    auto* h = LookupHost(host);
    if (!h)
        return;
    h->AddInputCharacter(character);
}

} // extern "C"
#endif
