#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"

#include "SmatchetImGuiHostC.h"

struct FSmatchetRendererInitParams {
    int RendererBackend = SMATCHET_RENDERER_BACKEND_UNKNOWN;
    int NumFramesInFlight = 3;
    int ColorFormat = 0;
    void* NativeDevice = nullptr;
    /** D3D12: ID3D12CommandQueue* from ID3D12DynamicRHI::RHIGetCommandQueue(). */
    void* NativeCommandQueue = nullptr;
    void* RendererResource0 = nullptr;
    void* RendererResource1 = nullptr;
    void* RendererResource2 = nullptr;
    /** D3D12: total SRV descriptors in the heap (slot 0 = font, 1..N-1 = dynamic thumbnails). */
    int NumSrvDescriptors = 1;
};

class FRHICommandListImmediate;

class ISmatchetImGuiRenderBackend {
public:
    virtual ~ISmatchetImGuiRenderBackend() = default;

    virtual const TCHAR* GetBackendName() const = 0;
    virtual int GetRendererBackendId() const = 0;

    virtual bool BuildInitParams(FSmatchetRendererInitParams& OutParams) = 0;
    // bDrawSoftwareCursor asks ImGui to render its own mouse cursor sprite. Enable only when the
    // game viewport has hidden the hardware cursor; leave off when the OS cursor is still visible.
    virtual bool RenderToSlateBackBuffer(
        SmatchetImGuiHostHandle Host,
        FRHITexture* BackBufferTexture,
        FRHICommandListImmediate* RHICmdList,
        bool bDrawSoftwareCursor) = 0;

    virtual void Shutdown() {}
};

TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend();
