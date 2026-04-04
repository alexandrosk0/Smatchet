#include "SmatchetImGuiViewExtension.h"

#include "Engine/Engine.h"
#include "Logging/LogMacros.h"
#include "RHICommandList.h"
#include "DynamicRHI.h"
#include "RenderGraphBuilder.h"
#include "Misc/EngineVersionComparison.h"
#include "SmatchetImGuiHostC.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetImGuiViewExtension, Log, All);

FSmatchetImGuiViewExtension::FSmatchetImGuiViewExtension(const FAutoRegister& AutoRegister, SmatchetImGuiHostHandle InHost)
    : FSceneViewExtensionBase(AutoRegister), Host(InHost) {
}

void FSmatchetImGuiViewExtension::SetupViewFamily(FSceneViewFamily&) {
}

void FSmatchetImGuiViewExtension::SetupView(FSceneViewFamily&, FSceneView&) {
}

void FSmatchetImGuiViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily) {
    if (!Host) {
        return;
    }
    static bool bLoggedRuntimePath = false;
    if (!bLoggedRuntimePath) {
#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
        UE_LOG(LogSmatchetImGuiViewExtension, Log, TEXT("Render integration active: UE 5.7+ path."));
#else
        UE_LOG(LogSmatchetImGuiViewExtension, Warning, TEXT("Render integration active on unsupported/untested UE version path."));
#endif
        bLoggedRuntimePath = true;
    }

    const bool bUiVisible = SmatchetHost_IsUiVisible(Host) != 0;
    const bool bInitialized = SmatchetHost_IsInitialized(Host) != 0;
    static double LastUiStateLogSeconds = 0.0;
    if (bUiVisible) {
        const double Now = FPlatformTime::Seconds();
        if (Now - LastUiStateLogSeconds > 2.0) {
            const FIntPoint Size = InViewFamily.RenderTarget ? InViewFamily.RenderTarget->GetSizeXY() : FIntPoint(0, 0);
            UE_LOG(
                LogSmatchetImGuiViewExtension,
                Log,
                TEXT("Host state: UiVisible=%d Initialized=%d ImGuiDisplaySize=(%d,%d)"),
                bUiVisible ? 1 : 0,
                bInitialized ? 1 : 0,
                Size.X,
                Size.Y);
            LastUiStateLogSeconds = Now;
        }
    }
    PendingRenderFrame.store(0, std::memory_order_relaxed);
}

void FSmatchetImGuiViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder&, FSceneViewFamily&) {
}

void FSmatchetImGuiViewExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily&) {
    (void)GraphBuilder;
    // Rendering is handled from the Slate backbuffer present callback in module.
}

bool FSmatchetImGuiViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext&) const {
    return Host != nullptr;
}
