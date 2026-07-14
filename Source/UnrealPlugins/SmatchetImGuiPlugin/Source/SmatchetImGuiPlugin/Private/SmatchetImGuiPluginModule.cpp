#include "Modules/ModuleManager.h"

#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "PixelFormat.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Rendering/RenderingCommon.h"
#include "SceneViewExtension.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SWindow.h"

#include "SmatchetHostContextCollector.h"
#include "SmatchetImGuiHostC.h"
#include "SmatchetImGuiCommandBridge.h"
#include "SmatchetImGuiInputProcessor.h"
#include "SmatchetImGuiRenderBackend.h"
#include "SmatchetImGuiViewExtension.h"
#include "SmatchetOutputLogTail.h"
#include "SmatchetDefaults.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetImGuiPlugin, Log, All);

namespace {

// If the presenting Slate window is (or contains) a UGameViewportClient's FSceneViewport, draw ImGui's
// software mouse sprite so PIE / standalone / packaged always have a visible pointer — UE's own flags
// (IsCursorVisible, show-cursor settings) are unreliable across embedded/separate PIE and captured game
// modes. The Ctrl+Alt+J hotkey flips SmatchetHost's Suppress flag if the user ever sees a double pointer.
// Render thread only: input processor publishes hovered ISlateViewport pointer identity via atomic
// (no FSlateApplication access off the game thread).
bool ShouldDrawSoftwareCursorForSlateWindow(SmatchetImGuiHostHandle Host, SWindow& SlateWindow) {
    if (Host && SmatchetHost_GetSuppressSoftwareCursor(Host)) {
        return false;
    }
    const TSharedPtr<ISlateViewport> SlateViewport = SlateWindow.GetViewport();
    if (!SlateViewport.IsValid()) {
        return false;
    }
    const std::uintptr_t HoveredViewportId = GSmatchetPointerOverSlateViewportId.load(std::memory_order_relaxed);
    if (HoveredViewportId == 0) {
        return false;
    }
    const std::uintptr_t WindowViewportId = reinterpret_cast<std::uintptr_t>(SlateViewport.Get());
    return HoveredViewportId == WindowViewportId;
}

void SmatchetOpenUrlCallback(const char* UrlUtf8, void*) {
    const FString UEUrl(UrlUtf8 ? UTF8_TO_TCHAR(UrlUtf8) : TEXT(""));
    // Scheme allowlist mirrors AppController::OpenUrl to avoid `javascript:` / `file:` / etc.
    const bool bHttp = UEUrl.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase);
    const bool bHttps = UEUrl.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
    const bool bMailto = UEUrl.StartsWith(TEXT("mailto:"), ESearchCase::IgnoreCase);
    if (!bHttp && !bHttps && !bMailto) {
        UE_LOG(LogTemp, Warning, TEXT("SmatchetOpenUrlCallback rejected url with non-allowlisted scheme."));
        return;
    }
    FPlatformProcess::LaunchURL(*UEUrl, nullptr, nullptr);
}

void SmatchetAttachmentViewerCallback(const char* LocalPathUtf8, const char*, const char*, void*) {
    const FString Path(LocalPathUtf8 ? UTF8_TO_TCHAR(LocalPathUtf8) : TEXT(""));
    FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
}

/** Streaming sync + offline replay normally run inside ImGui Draw; when the overlay is hidden, Unreal skips DrawUI
 * entirely, so drain work here (throttled — many Slate windows can present per frame). */
void TickSmatchetApplicationIfHostReadyAndUiHidden(SmatchetImGuiHostHandle InHost) {
    if (!InHost || SmatchetHost_IsUiVisible(InHost) != 0) {
        return;
    }
    if (SmatchetHost_IsInitialized(InHost) == 0) {
        return;
    }
    static thread_local double s_lastSeconds = 0.0;
    const double now = FPlatformTime::Seconds();
    if ((now - s_lastSeconds) < (1.0 / 60.0)) {
        return;
    }
    s_lastSeconds = now;
    SmatchetHost_TickApplicationWork(InHost);
}
} // namespace

class FSmatchetImGuiPluginModule;
static FSmatchetImGuiPluginModule* GSmatchetImGuiPluginModule = nullptr;

class FSmatchetImGuiPluginModule : public IModuleInterface {
  public:
    virtual void StartupModule() override {
        GSmatchetImGuiPluginModule = this;
        Host = SmatchetHost_Create();
        UE_LOG(LogSmatchetImGuiPlugin, Log, TEXT("Smatchet packaged native host tag: %s"),
               ANSI_TO_TCHAR(SmatchetHost_GetBuildTag()));
        RenderBackend = CreateSmatchetImGuiRenderBackend();
        DbPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Smatchet_LocalCache.sqlite"));
        {
            const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
            UE_LOG(LogSmatchetImGuiPlugin, Log,
                   TEXT("Smatchet paths (diagnostics): ProjectDir=%s | SavedDir(db)=%s — native core resolves "
                        "Lua under ConfigManager files base or cwd-relative Scripts/; standalone sets base from exe "
                        "dir; Unreal does not unless wired. Check Output Log + Smatchet Logger for "
                        "luaScriptsDirectory / probes."),
                   *ProjectDir, *DbPath);
        }

        if (!RenderBackend) {
            UE_LOG(LogSmatchetImGuiPlugin, Warning, TEXT("No Smatchet render backend available for this platform."));
        } else {
            bInitOptionsSet = ConfigureAndCacheInitOptions();
            UE_LOG(LogSmatchetImGuiPlugin, Log, TEXT("Smatchet render backend selected: %s | init options primed: %s"),
                   RenderBackend->GetBackendName(), bInitOptionsSet ? TEXT("yes") : TEXT("no"));
        }

        if (!bInitOptionsSet) {
            InitOptionsRetryHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float) {
                if (bInitOptionsSet || !Host || !RenderBackend) {
                    return false;
                }
                if (ConfigureAndCacheInitOptions()) {
                    return false;
                }
                return true;
            }));
        }

        SmatchetHost_SetUiVisible(Host, false);

        if (FSlateApplication::IsInitialized()) {
            InputProcessor = MakeShared<FSmatchetImGuiInputProcessor>(Host);
            // Default registration uses EInputPreProcessorType::Game (lowest priority). Editor and
            // game preprocessors run first; when none consume, we still run — but double-clicks
            // and some paths still leaked to Slate. Register in PreEditor at the front of that
            // bucket so we can reliably swallow input while the Smatchet UI is visible.
            FInputPreprocessorRegistrationKey PreProcKey;
            PreProcKey.Type = EInputPreProcessorType::PreEditor;
            PreProcKey.Priority = 0;
            FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor, PreProcKey);
        }

        if (!TryRegisterBackBufferHook()) {
            BackBufferHookRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([this](float) { return TryRegisterBackBufferHook() ? false : true; }));
        }

        if (!TryCreateViewExtension()) {
            ViewExtensionRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([this](float) { return TryCreateViewExtension() ? false : true; }));
        }

        // Host-context snapshot for the quick-create issue popup: buffer recent Output Log
        // lines for the snapshot's logTail field, and push a fresh snapshot at ~1 Hz on the
        // game thread while the overlay is visible (editor state — GEditor, selection,
        // worlds — is game-thread-only, so a core-side pull callback is not an option).
        LogTail = MakeUnique<FSmatchetOutputLogTail>();
        if (GLog) {
            GLog->AddOutputDevice(LogTail.Get());
        }
        ContextPushHandle =
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float) {
                                                     if (Host && SmatchetHost_IsUiVisible(Host) != 0) {
                                                         const FString ContextJson =
                                                             SmatchetHostContextCollector_BuildJson(LogTail.Get());
                                                         FTCHARToUTF8 ContextUtf8(*ContextJson);
                                                         SmatchetHost_SetHostContextJson(Host, ContextUtf8.Get());
                                                     }
                                                     return true;
                                                 }),
                                                 1.0f);

        SmatchetImGuiConsoleCommands_StartupModule();
    }

    virtual void ShutdownModule() override {
        GSmatchetImGuiPluginModule = nullptr;

        SmatchetImGuiConsoleCommands_ShutdownModule();
        SmatchetImGuiCommandBridge_ShutdownModule();

        // Stop the context push before anything else and detach the log device before the
        // host teardown below — GLog would otherwise Serialize into a freed device during
        // the shutdown log traffic.
        if (ContextPushHandle.IsValid()) {
            FTSTicker::GetCoreTicker().RemoveTicker(ContextPushHandle);
            ContextPushHandle = {};
        }
        if (LogTail) {
            if (GLog) {
                GLog->RemoveOutputDevice(LogTail.Get());
            }
            LogTail.Reset();
        }

        if (InitOptionsRetryHandle.IsValid()) {
            FTSTicker::GetCoreTicker().RemoveTicker(InitOptionsRetryHandle);
            InitOptionsRetryHandle = {};
        }

        if (ViewExtensionRetryHandle.IsValid()) {
            FTSTicker::GetCoreTicker().RemoveTicker(ViewExtensionRetryHandle);
            ViewExtensionRetryHandle = {};
        }

        if (BackBufferHookRetryHandle.IsValid()) {
            FTSTicker::GetCoreTicker().RemoveTicker(BackBufferHookRetryHandle);
            BackBufferHookRetryHandle = {};
        }

        if (BackBufferPresentHandle.IsValid() && FSlateApplication::IsInitialized()) {
            FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferPresentHandle);
            BackBufferPresentHandle = {};
        }

        if (InputProcessor.IsValid() && FSlateApplication::IsInitialized()) {
            InputProcessor->EnsureTooltipsRestored();
            FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
            InputProcessor.Reset();
        }

        ViewExtension.Reset();

        // OnBackBufferReadyToPresent has been unhooked above, but RHI lambdas it already enqueued
        // (see EnqueueLambda in OnBackBufferReadyToPresent) capture this/Host/RenderBackend and may
        // still be in flight on the RHI thread. Flush the rendering commands here so every queued
        // lambda has run before we tear down the render backend and destroy the native host below —
        // otherwise those lambdas dereference freed module members and a destroyed host.
        FlushRenderingCommands();

        if (RenderBackend) {
            RenderBackend->Shutdown();
            RenderBackend.Reset();
        }

        if (Host) {
            SmatchetHost_Destroy(Host);
            Host = nullptr;
        }
    }

    SmatchetImGuiHostHandle GetNativeHostForCommands() const { return Host; }

  private:
    bool ApplyInitOptions(const FSmatchetRendererInitParams& Params) {
        if (!Host || !RenderBackend) {
            return false;
        }

        FTCHARToUTF8 DbPathUtf8(*DbPath);
        SmatchetHost_SetInitOptions(
            Host, DbPathUtf8.Get(), SmatchetDefaults::kDefaultBackendType, SmatchetDefaults::Mcp::kDefaultPort,
            Params.RendererBackend, Params.NumFramesInFlight, Params.ColorFormat, Params.NativeDevice,
            Params.RendererResource0, Params.RendererResource1, Params.RendererResource2, Params.NativeCommandQueue,
            &SmatchetOpenUrlCallback, nullptr, &SmatchetAttachmentViewerCallback, nullptr);
        // DX12 dynamic-texture SRV allocator needs to know the heap capacity so it can hand out
        // slots 1..N-1 for attachment thumbnails (slot 0 stays reserved for the font atlas).
        SmatchetHost_SetNumSrvDescriptors(Host, Params.NumSrvDescriptors);
        CachedColorFormat = Params.ColorFormat;
        return true;
    }

    bool ConfigureAndCacheInitOptions() {
        if (!RenderBackend) {
            return false;
        }
        FSmatchetRendererInitParams Params;
        if (!RenderBackend->BuildInitParams(Params)) {
            return false;
        }
        if (CachedColorFormat > 0) {
            Params.ColorFormat = CachedColorFormat;
        }
        if (Params.ColorFormat == 0) {
            Params.ColorFormat = 87; // DXGI_FORMAT_B8G8R8A8_UNORM fallback where available.
        }
        return ApplyInitOptions(Params);
    }

    bool TryRenderToSlateBackBuffer(FRHITexture* BackBufferTexture, FRHICommandListImmediate* RHICmdList,
                                    bool bDrawSoftwareCursor) {
        if (!Host || !RenderBackend || !BackBufferTexture) {
            return false;
        }
        if (SmatchetHost_IsUiVisible(Host) == 0) {
            TickSmatchetApplicationIfHostReadyAndUiHidden(Host);
            return false;
        }

        const EPixelFormat BackBufferPF = BackBufferTexture->GetFormat();
        const int BackBufferColorFormat = static_cast<int>(GPixelFormats[BackBufferPF].PlatformFormat);
        if (BackBufferColorFormat != 0 && BackBufferColorFormat != CachedColorFormat) {
            CachedColorFormat = BackBufferColorFormat;
            if (SmatchetHost_IsInitialized(Host) == 0) {
                ConfigureAndCacheInitOptions();
            } else {
                static bool bWarnedColorFormatAfterInit = false;
                if (!bWarnedColorFormatAfterInit) {
                    UE_LOG(LogSmatchetImGuiPlugin, Warning,
                           TEXT("Backbuffer color format changed after host init (cached=%d, current=%d). Reinit "
                                "required to apply."),
                           CachedColorFormat, BackBufferColorFormat);
                    bWarnedColorFormatAfterInit = true;
                }
            }
        }

        return RenderBackend->RenderToSlateBackBuffer(Host, BackBufferTexture, RHICmdList, bDrawSoftwareCursor);
    }

    void OnBackBufferReadyToPresent(SWindow& SlateWindow, const FTextureRHIRef& BackBufferTexture) {
        if (!IsInRenderingThread()) {
            return;
        }
        if (!BackBufferTexture.IsValid() || !Host || !RenderBackend) {
            return;
        }
        if (SmatchetHost_IsUiVisible(Host) == 0) {
            TickSmatchetApplicationIfHostReadyAndUiHidden(Host);
            return;
        }
        // Unreal fires this hook for every Slate window it presents, including tooltips, menus,
        // drag-and-drop cursor decorators, and notification popups. Each of those has its own
        // tiny back buffer; drawing the full ImGui overlay into them scribbles over the editor
        // (e.g. the screen goes black while hovering Play because we render the overlay into the
        // tooltip window's back buffer). Restrict to top-level editor / game / PIE windows.
        const EWindowType WindowType = SlateWindow.GetType();
        if (WindowType != EWindowType::Normal && WindowType != EWindowType::GameWindow) {
            return;
        }

        // PIE hides the OS cursor but embedded PIE still uses EWindowType::Normal; only GameWindow
        // is not enough. Editor chrome keeps the OS cursor — do not draw ImGui's there.
        const bool bDrawSoftwareCursor = ShouldDrawSoftwareCursorForSlateWindow(Host, SlateWindow);
        const FTextureRHIRef BackBufferCopy = BackBufferTexture;

        // Enqueue into the RHI command stream; the lambda runs when the list is bottom-of-pipe,
        // which is the only time FD3D12CommandContext::Get / RHIGetGraphicsCommandList are safe.
        FRHICommandListImmediate& ImmediateCmdList = FRHICommandListExecutor::GetImmediateCommandList();
        ImmediateCmdList.EnqueueLambda(
            TEXT("SmatchetImGuiDrawSlateOverlay"),
            [this, BackBufferCopy, bDrawSoftwareCursor](FRHICommandListImmediate& RHICmdList) {
                if (!Host || !RenderBackend) {
                    return;
                }
                if (SmatchetHost_IsUiVisible(Host) == 0) {
                    TickSmatchetApplicationIfHostReadyAndUiHidden(Host);
                    return;
                }
                TryRenderToSlateBackBuffer(BackBufferCopy.GetReference(), &RHICmdList, bDrawSoftwareCursor);
            });
    }

    bool TryRegisterBackBufferHook() {
        if (!RenderBackend) {
            return false;
        }
        if (BackBufferPresentHandle.IsValid()) {
            return true;
        }
        if (!FSlateApplication::IsInitialized()) {
            return false;
        }
        BackBufferPresentHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(
            this, &FSmatchetImGuiPluginModule::OnBackBufferReadyToPresent);
        return BackBufferPresentHandle.IsValid();
    }

    bool TryCreateViewExtension() {
        if (ViewExtension.IsValid() || !Host) {
            return true;
        }
        if (!GEngine) {
            return false;
        }
        ViewExtension = FSceneViewExtensions::NewExtension<FSmatchetImGuiViewExtension>(Host);
        return ViewExtension.IsValid();
    }

  private:
    SmatchetImGuiHostHandle Host = nullptr;
    TUniquePtr<ISmatchetImGuiRenderBackend> RenderBackend;
    TSharedPtr<FSmatchetImGuiInputProcessor> InputProcessor;
    TSharedPtr<FSmatchetImGuiViewExtension, ESPMode::ThreadSafe> ViewExtension;

    FString DbPath;
    int CachedColorFormat = 0;
    bool bInitOptionsSet = false;

    FDelegateHandle BackBufferPresentHandle{};
    FTSTicker::FDelegateHandle BackBufferHookRetryHandle{};
    FTSTicker::FDelegateHandle InitOptionsRetryHandle{};
    FTSTicker::FDelegateHandle ViewExtensionRetryHandle{};

    // Quick-create host-context snapshot (docs/plans/quick-create-issue-unreal-context.md).
    TUniquePtr<FSmatchetOutputLogTail> LogTail;
    FTSTicker::FDelegateHandle ContextPushHandle{};
};

SmatchetImGuiHostHandle SmatchetImGuiPlugin_GetNativeHostForCommands() {
    return GSmatchetImGuiPluginModule ? GSmatchetImGuiPluginModule->GetNativeHostForCommands() : nullptr;
}

IMPLEMENT_MODULE(FSmatchetImGuiPluginModule, SmatchetImGuiPlugin)
