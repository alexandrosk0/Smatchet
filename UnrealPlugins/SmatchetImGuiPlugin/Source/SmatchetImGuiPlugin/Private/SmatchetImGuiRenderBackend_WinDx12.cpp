#include "SmatchetImGuiRenderBackend.h"

#if PLATFORM_WINDOWS

#include "Containers/StringConv.h"
#include "ID3D12DynamicRHI.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "PixelFormat.h"
#include "RHICommandList.h"

#include "imgui.h"

#include <d3d12.h>
#include <wrl/client.h>

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetWinDx12RenderBackend, Log, All);

namespace {

// Slate "back buffer ready" hook executes this via FRHICommandListImmediate::EnqueueLambda, so the
// command list is bottom-of-pipe. GetNativeCommandBuffer() is usually null there, but the D3D12 RHI
// can still resolve the underlying graphics command list via RHIGetGraphicsCommandList.
ID3D12GraphicsCommandList* ResolveD3D12GraphicsCommandList(FRHICommandListImmediate* RHICmdList,
                                                           ID3D12DynamicRHI* D3D12RHI) {
    if (!RHICmdList) {
        return nullptr;
    }
    if (void* Native = RHICmdList->GetNativeCommandBuffer()) {
        return reinterpret_cast<ID3D12GraphicsCommandList*>(Native);
    }
    if (D3D12RHI) {
        return D3D12RHI->RHIGetGraphicsCommandList(*RHICmdList, /*InDeviceIndex=*/0);
    }
    return nullptr;
}

class FWinDx12RenderBackend final : public ISmatchetImGuiRenderBackend {
  public:
    virtual const TCHAR* GetBackendName() const override { return TEXT("Win64-DX12"); }

    virtual int GetRendererBackendId() const override { return SMATCHET_RENDERER_BACKEND_DX12; }

    virtual bool BuildInitParams(FSmatchetRendererInitParams& OutParams) override {
        OutParams = {};
        OutParams.RendererBackend = GetRendererBackendId();
        OutParams.NumFramesInFlight = 3;
        OutParams.ColorFormat = CachedColorFormat;

        ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
        if (!D3D12RHI) {
            UE_LOG(LogSmatchetWinDx12RenderBackend, Warning, TEXT("ID3D12DynamicRHI is not available yet."));
            return false;
        }

        Dx12Device = D3D12RHI->RHIGetDevice(0);
        if (!Dx12Device) {
            UE_LOG(LogSmatchetWinDx12RenderBackend, Warning, TEXT("DX12 device is unavailable."));
            return false;
        }

        ID3D12CommandQueue* GfxQueue = D3D12RHI->RHIGetCommandQueue();
        if (!GfxQueue) {
            UE_LOG(LogSmatchetWinDx12RenderBackend, Warning, TEXT("D3D12 graphics command queue is unavailable."));
            return false;
        }

        // Slot 0 is used by the ImGui font atlas (legacy single-SRV path). Slots 1..N-1 are used by
        // the Smatchet DX12 SRV allocator for dynamic textures (attachment thumbnails). 128 entries
        // is ample for our previews and costs ~4KB of descriptor heap memory.
        constexpr int NumSrvDescriptors = 128;

        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc{};
        HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = NumSrvDescriptors;
        HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HeapDesc.NodeMask = 0;

        HRESULT Hr = Dx12Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&ImGuiFontSrvHeap));
        if (FAILED(Hr) || !ImGuiFontSrvHeap) {
            UE_LOG(LogSmatchetWinDx12RenderBackend, Warning, TEXT("CreateDescriptorHeap failed (0x%08x)."),
                   static_cast<uint32>(Hr));
            return false;
        }

        ImGuiFontSrvCpuHandle = ImGuiFontSrvHeap->GetCPUDescriptorHandleForHeapStart();
        ImGuiFontSrvGpuHandle = ImGuiFontSrvHeap->GetGPUDescriptorHandleForHeapStart();

        OutParams.NativeDevice = Dx12Device;
        OutParams.NativeCommandQueue = GfxQueue;
        OutParams.RendererResource0 = ImGuiFontSrvHeap.Get();
        // Pass descriptor handle values (not pointers) across module/runtime boundary.
        // This avoids ABI/lifetime issues when crossing toolchains.
        OutParams.RendererResource1 = reinterpret_cast<void*>(ImGuiFontSrvCpuHandle.ptr);
        OutParams.RendererResource2 = reinterpret_cast<void*>(ImGuiFontSrvGpuHandle.ptr);
        OutParams.NumSrvDescriptors = NumSrvDescriptors;
        return true;
    }

    virtual bool RenderToSlateBackBuffer(SmatchetImGuiHostHandle Host, FRHITexture* BackBufferTexture,
                                         FRHICommandListImmediate* RHICmdList, bool bDrawSoftwareCursor) override {
        if (!Host || !BackBufferTexture) {
            return false;
        }
        if (SmatchetHost_IsUiVisible(Host) == 0) {
            return false;
        }

        ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
        if (!D3D12RHI) {
            return false;
        }

        ID3D12Device* Device = D3D12RHI->RHIGetDevice(0);
        if (!Device) {
            return false;
        }

        const FIntPoint BackBufferSize = BackBufferTexture->GetSizeXY();
        constexpr float FallbackDelta = 1.0f / 60.0f;

        if (SmatchetHost_IsInitialized(Host) == 0) {
            static double LastInitDiagSeconds = 0.0;
            const double NowSec = FPlatformTime::Seconds();
            if (NowSec - LastInitDiagSeconds > 2.0) {
                LastInitDiagSeconds = NowSec;
                char Summary[384];
                SmatchetHost_FormatCachedRendererSummary(Host, Summary, static_cast<int>(sizeof(Summary)));
                UE_LOG(LogSmatchetWinDx12RenderBackend, Warning,
                       TEXT("Smatchet host not initialized yet (lazy init runs inside BeginFrame). build=%s | %s | "
                            "lastError=%s | Also watch Visual Studio Output for [Smatchet] lines."),
                       ANSI_TO_TCHAR(SmatchetHost_GetBuildTag()), ANSI_TO_TCHAR(Summary),
                       ANSI_TO_TCHAR(SmatchetHost_GetLastInitError(Host)));
            }
        }

        SmatchetHost_BeginFrame(Host, FallbackDelta, static_cast<float>(BackBufferSize.X),
                                static_cast<float>(BackBufferSize.Y));
        // When the game viewport hides the hardware cursor, draw ImGui's sprite so clicks are visible
        // inside the overlay. When the OS cursor is still visible, keep this false to avoid two pointers.
        // BeginFrame made the ImGui context current.
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::GetIO().MouseDrawCursor = bDrawSoftwareCursor;
        }
        SmatchetHost_DrawUI(Host);
        if (SmatchetHost_IsInitialized(Host) == 0 || SmatchetHost_IsFrameActive(Host) == 0) {
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle =
            D3D12RHI->RHIGetRenderTargetView(BackBufferTexture, /*MipIndex=*/0, /*ArraySliceIndex=*/0);

        ID3D12GraphicsCommandList* Cmd = ResolveD3D12GraphicsCommandList(RHICmdList, D3D12RHI);
        if (Cmd) {
            D3D12_VIEWPORT Viewport{};
            Viewport.Width = static_cast<float>(BackBufferSize.X);
            Viewport.Height = static_cast<float>(BackBufferSize.Y);
            Viewport.MinDepth = 0.0f;
            Viewport.MaxDepth = 1.0f;
            D3D12_RECT ScissorRect{0, 0, BackBufferSize.X, BackBufferSize.Y};
            Cmd->RSSetViewports(1, &Viewport);
            Cmd->RSSetScissorRects(1, &ScissorRect);
            Cmd->OMSetRenderTargets(1, &RtvHandle, false, nullptr);
            SmatchetHost_RenderDrawData(Host, GetRendererBackendId(), Cmd);

            static double LastRenderLogSeconds = 0.0;
            const double Now = FPlatformTime::Seconds();
            if (Now - LastRenderLogSeconds > 2.0) {
                const bool bUsedNativeBuffer = (RHICmdList && RHICmdList->GetNativeCommandBuffer() != nullptr);
                UE_LOG(LogSmatchetWinDx12RenderBackend, Log,
                       TEXT("Rendered ImGui on D3D12 command list (native buffer: %s)."),
                       bUsedNativeBuffer ? TEXT("yes") : TEXT("no (RHIGetGraphicsCommandList)"));
                LastRenderLogSeconds = Now;
            }
            return true;
        }

        static double LastMissingNativeCmdLogSeconds = 0.0;
        const double MissingCmdNow = FPlatformTime::Seconds();
        if (MissingCmdNow - LastMissingNativeCmdLogSeconds > 2.0) {
            UE_LOG(LogSmatchetWinDx12RenderBackend, Warning,
                   TEXT("Could not resolve ID3D12GraphicsCommandList for Slate present hook (GetNativeCommandBuffer "
                        "null and RHIGetGraphicsCommandList null)."));
            LastMissingNativeCmdLogSeconds = MissingCmdNow;
        }
        return false;
    }

    virtual void Shutdown() override { ImGuiFontSrvHeap.Reset(); }

  private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ImGuiFontSrvHeap;
    ID3D12Device* Dx12Device = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE ImGuiFontSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE ImGuiFontSrvGpuHandle{};
    int CachedColorFormat = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
};
} // namespace

TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend_WinDx12() {
    return MakeUnique<FWinDx12RenderBackend>();
}

#else
TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend_WinDx12() { return nullptr; }
#endif
