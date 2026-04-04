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
};

class FRHICommandListImmediate;

class ISmatchetImGuiRenderBackend {
public:
    virtual ~ISmatchetImGuiRenderBackend() = default;

    virtual const TCHAR* GetBackendName() const = 0;
    virtual int GetRendererBackendId() const = 0;

    virtual bool BuildInitParams(FSmatchetRendererInitParams& OutParams) = 0;
    virtual bool RenderToSlateBackBuffer(
        SmatchetImGuiHostHandle Host,
        FRHITexture* BackBufferTexture,
        FRHICommandListImmediate* RHICmdList) = 0;

    virtual void Shutdown() {}
};

TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend();
