#include "SmatchetImGuiRenderBackend.h"

#include "DynamicRHI.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "PixelFormat.h"
#include "RHICommandList.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetPlatformRenderBackend, Log, All);

namespace {
class FPlatformRenderBackend final : public ISmatchetImGuiRenderBackend {
  public:
    virtual const TCHAR* GetBackendName() const override {
#if defined(PLATFORM_PS5) && PLATFORM_PS5
        return TEXT("PS5");
#elif (defined(PLATFORM_XBOXONE) && PLATFORM_XBOXONE) || (defined(PLATFORM_XSX) && PLATFORM_XSX)
        return TEXT("Xbox");
#else
        return TEXT("Unsupported");
#endif
    }

    virtual int GetRendererBackendId() const override {
#if defined(PLATFORM_PS5) && PLATFORM_PS5
        return SMATCHET_RENDERER_BACKEND_PS5;
#elif (defined(PLATFORM_XBOXONE) && PLATFORM_XBOXONE) || (defined(PLATFORM_XSX) && PLATFORM_XSX)
        return SMATCHET_RENDERER_BACKEND_XBOX;
#else
        return SMATCHET_RENDERER_BACKEND_UNKNOWN;
#endif
    }

    virtual bool BuildInitParams(FSmatchetRendererInitParams& OutParams) override {
        OutParams = {};
        OutParams.RendererBackend = GetRendererBackendId();
        OutParams.NumFramesInFlight = 3;

        if (!GDynamicRHI) {
            UE_LOG(LogSmatchetPlatformRenderBackend, Warning, TEXT("[%s] GDynamicRHI not ready yet."),
                   GetBackendName());
            return false;
        }

        OutParams.NativeDevice = GDynamicRHI->RHIGetNativeDevice();
        if (!OutParams.NativeDevice) {
            UE_LOG(LogSmatchetPlatformRenderBackend, Warning, TEXT("[%s] Native device not available yet."),
                   GetBackendName());
            return false;
        }
        return true;
    }

    virtual bool RenderToSlateBackBuffer(SmatchetImGuiHostHandle Host, FRHITexture* BackBufferTexture,
                                         FRHICommandListImmediate* RHICmdList, bool /*bDrawSoftwareCursor*/) override {
        if (!Host || !BackBufferTexture || !RHICmdList) {
            return false;
        }
        if (SmatchetHost_IsUiVisible(Host) == 0) {
            return false;
        }

        const FIntPoint BackBufferSize = BackBufferTexture->GetSizeXY();
        constexpr float FallbackDelta = 1.0f / 60.0f;
        SmatchetHost_BeginFrame(Host, FallbackDelta, static_cast<float>(BackBufferSize.X),
                                static_cast<float>(BackBufferSize.Y));
        SmatchetHost_DrawUI(Host);
        if (SmatchetHost_IsInitialized(Host) == 0 || SmatchetHost_IsFrameActive(Host) == 0) {
            return false;
        }

        void* NativeCmd = RHICmdList->GetNativeCommandBuffer();
        if (!NativeCmd) {
            return false;
        }
        SmatchetHost_RenderDrawData(Host, GetRendererBackendId(), NativeCmd);

        static double LastLogSeconds = 0.0;
        const double Now = FPlatformTime::Seconds();
        if (Now - LastLogSeconds > 2.0) {
            UE_LOG(LogSmatchetPlatformRenderBackend, Log, TEXT("[%s] rendered on Slate backbuffer hook."),
                   GetBackendName());
            LastLogSeconds = Now;
        }
        return true;
    }
};
} // namespace

TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend_Platform() {
    return MakeUnique<FPlatformRenderBackend>();
}
