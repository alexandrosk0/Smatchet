#include "Modules/ModuleManager.h"

#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "PixelFormat.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "Rendering/SlateRenderer.h"
#include "SceneViewExtension.h"
#include "Widgets/SWindow.h"

#include "SmatchetImGuiHostC.h"
#include "SmatchetImGuiInputProcessor.h"
#include "SmatchetImGuiRenderBackend.h"
#include "SmatchetImGuiViewExtension.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetImGuiPlugin, Log, All);

// Used only to satisfy RDG validation:
// If we specify ERDGPassFlags::Raster / Compute, the pass must have a non-null parameter struct.
// The struct doesn't need to contain actual RDG resources for this pass.
BEGIN_SHADER_PARAMETER_STRUCT(FImGuiBackBufferPassParams, )
END_SHADER_PARAMETER_STRUCT()

namespace {
void SmatchetOpenUrlCallback(const char* UrlUtf8, void*) {
    const FString UEUrl(UrlUtf8 ? UTF8_TO_TCHAR(UrlUtf8) : TEXT(""));
    FPlatformProcess::LaunchURL(*UEUrl, nullptr, nullptr);
}

void SmatchetAttachmentViewerCallback(const char* LocalPathUtf8, const char*, const char*, void*) {
    const FString Path(LocalPathUtf8 ? UTF8_TO_TCHAR(LocalPathUtf8) : TEXT(""));
    FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
}
} // namespace

class FSmatchetImGuiPluginModule : public IModuleInterface {
public:
    virtual void StartupModule() override {
        Host = SmatchetHost_Create();
        UE_LOG(
            LogSmatchetImGuiPlugin,
            Log,
            TEXT("Smatchet packaged native host tag: %s"),
            ANSI_TO_TCHAR(SmatchetHost_GetBuildTag()));
        RenderBackend = CreateSmatchetImGuiRenderBackend();
        DbPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Smatchet_LocalCache.sqlite"));

        if (!RenderBackend) {
            UE_LOG(LogSmatchetImGuiPlugin, Warning, TEXT("No Smatchet render backend available for this platform."));
        } else {
            bInitOptionsSet = ConfigureAndCacheInitOptions();
            UE_LOG(
                LogSmatchetImGuiPlugin,
                Log,
                TEXT("Smatchet render backend selected: %s | init options primed: %s"),
                RenderBackend->GetBackendName(),
                bInitOptionsSet ? TEXT("yes") : TEXT("no"));
        }

        if (!bInitOptionsSet) {
            InitOptionsRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([this](float) {
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
            FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
        }

        if (!TryRegisterBackBufferHook()) {
            BackBufferHookRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([this](float) {
                    return TryRegisterBackBufferHook() ? false : true;
                }));
        }

        if (!TryCreateViewExtension()) {
            ViewExtensionRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([this](float) {
                    return TryCreateViewExtension() ? false : true;
                }));
        }
    }

    virtual void ShutdownModule() override {
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

        if (BackBufferReadyHandle.IsValid() && FSlateApplication::IsInitialized()) {
            FSlateApplication::Get().GetRenderer()->OnAddBackBufferReadyToPresentPass().Remove(BackBufferReadyHandle);
            BackBufferReadyHandle = {};
        }

        if (InputProcessor.IsValid() && FSlateApplication::IsInitialized()) {
            FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
            InputProcessor.Reset();
        }

        ViewExtension.Reset();

        if (RenderBackend) {
            RenderBackend->Shutdown();
            RenderBackend.Reset();
        }

        if (Host) {
            SmatchetHost_Destroy(Host);
            Host = nullptr;
        }
    }

private:
    bool ApplyInitOptions(const FSmatchetRendererInitParams& Params) {
        if (!Host || !RenderBackend) {
            return false;
        }

        FTCHARToUTF8 DbPathUtf8(*DbPath);
        SmatchetHost_SetInitOptions(
            Host,
            DbPathUtf8.Get(),
            "Jira",
            8080,
            Params.RendererBackend,
            Params.NumFramesInFlight,
            Params.ColorFormat,
            Params.NativeDevice,
            Params.RendererResource0,
            Params.RendererResource1,
            Params.RendererResource2,
            Params.NativeCommandQueue,
            &SmatchetOpenUrlCallback,
            nullptr,
            &SmatchetAttachmentViewerCallback,
            nullptr);
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

    bool TryRenderToSlateBackBuffer(FRHITexture* BackBufferTexture, FRHICommandListImmediate* RHICmdList) {
        if (!Host || !RenderBackend || !BackBufferTexture) {
            return false;
        }
        if (SmatchetHost_IsUiVisible(Host) == 0) {
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
                    UE_LOG(
                        LogSmatchetImGuiPlugin,
                        Warning,
                        TEXT("Backbuffer color format changed after host init (cached=%d, current=%d). Reinit required to apply."),
                        CachedColorFormat,
                        BackBufferColorFormat);
                    bWarnedColorFormatAfterInit = true;
                }
            }
        }

        return RenderBackend->RenderToSlateBackBuffer(Host, BackBufferTexture, RHICmdList);
    }

    void OnAddBackBufferReadyToPresentPass(FRDGBuilder& GraphBuilder, SWindow&, FRDGTexture* BackBufferTexture) {
        if (!BackBufferTexture) {
            return;
        }
        FRHITexture* BackBufferRHI = BackBufferTexture->GetRHI();
        if (!BackBufferRHI) {
            return;
        }

        FImGuiBackBufferPassParams* PassParams = GraphBuilder.AllocParameters<FImGuiBackBufferPassParams>();

        GraphBuilder.AddPass(
            RDG_EVENT_NAME("SmatchetImGuiBackBuffer"),
            PassParams,
            ERDGPassFlags::NeverCull | ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass,
            [this, PassParams, BackBufferRHI](FRHICommandListImmediate& RHICmdList) {
                (void)PassParams;
                TryRenderToSlateBackBuffer(BackBufferRHI, &RHICmdList);
            });
    }

    bool TryRegisterBackBufferHook() {
        if (!RenderBackend) {
            return false;
        }
        if (BackBufferReadyHandle.IsValid()) {
            return true;
        }
        if (!FSlateApplication::IsInitialized()) {
            return false;
        }
        BackBufferReadyHandle = FSlateApplication::Get().GetRenderer()->OnAddBackBufferReadyToPresentPass().AddRaw(
            this,
            &FSmatchetImGuiPluginModule::OnAddBackBufferReadyToPresentPass);
        return BackBufferReadyHandle.IsValid();
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

    FDelegateHandle BackBufferReadyHandle{};
    FTSTicker::FDelegateHandle BackBufferHookRetryHandle{};
    FTSTicker::FDelegateHandle InitOptionsRetryHandle{};
    FTSTicker::FDelegateHandle ViewExtensionRetryHandle{};
};

IMPLEMENT_MODULE(FSmatchetImGuiPluginModule, SmatchetImGuiPlugin)
