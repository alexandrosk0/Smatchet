#include "Dx12Bootstrap.h"

#if SMATCHET_STANDALONE_HAS_DX12

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Logger.h"
#include "imgui_impl_dx12.h"

#include <cstring>
#include <mutex>

namespace {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Shader-visible SRV heap allocator for the ImGui DX12 backend's dynamic
// textures. Slot 0 is reserved for the font atlas; further slots come from a
// tiny free-list. Mirrored from Source/Core/src/Ui/SmatchetImGuiHost.cpp (the
// Unreal/DX12 host) — the standalone path needs the identical contract but
// can't share that file's Impl. Copy-paste DRY WARN accepted; see plan
// Deviations. Standalone drives exactly one window, so a file-static instance
// is sufficient (matches the host).
struct Dx12SrvAllocator {
    D3D12_CPU_DESCRIPTOR_HANDLE CpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE GpuBase{};
    UINT IncrementSize = 0;
    int Capacity = 0;      // total descriptors, including slot 0 (font)
    int OverflowSlot = -1; // reserved non-font slot used only on exhaustion
    int NextSlot = 1;      // next fresh non-overflow slot; 1..OverflowSlot-1
    std::vector<int> FreeList;
    bool bLoggedExhausted = false;
    std::mutex Mutex;
};

Dx12SrvAllocator gSrvAllocator;

void SrvAllocCb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    Dx12SrvAllocator& a = gSrvAllocator;
    std::lock_guard<std::mutex> lk(a.Mutex);
    int slot = -1;
    if (!a.FreeList.empty()) {
        slot = a.FreeList.back();
        a.FreeList.pop_back();
    } else if (a.OverflowSlot > 0 ? (a.NextSlot < a.OverflowSlot) : (a.NextSlot < a.Capacity)) {
        slot = a.NextSlot++;
    }
    if (slot < 0) {
        slot = (a.OverflowSlot > 0) ? a.OverflowSlot : 0;
        if (!a.bLoggedExhausted) {
            LOG_WARN("Standalone DX12: SRV heap exhausted (capacity=%d, overflowSlot=%d). Extra textures may alias.",
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

void SrvFreeCb(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
    Dx12SrvAllocator& a = gSrvAllocator;
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

std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return std::string();
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
    return out;
}

// Try every hardware adapter in turn; fall back to the WARP software adapter
// (the Windows-on-ARM64 path when no DX12 hardware driver is present).
bool SelectAdapterAndCreateDevice(IDXGIFactory4* factory, ComPtr<ID3D12Device>& outDevice, std::string& outDesc,
                                  bool& outWarp, std::string& err) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(outDevice.ReleaseAndGetAddressOf())))) {
            outDesc = WideToUtf8(desc.Description);
            outWarp = false;
            return true;
        }
    }
    ComPtr<IDXGIAdapter> warp;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
        SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(outDevice.ReleaseAndGetAddressOf())))) {
        DXGI_ADAPTER_DESC wdesc{};
        warp->GetDesc(&wdesc);
        outDesc = WideToUtf8(wdesc.Description);
        outWarp = true;
        return true;
    }
    err = "No Direct3D 12 capable adapter (hardware or WARP) found.";
    return false;
}

} // namespace

namespace smatchet {
namespace standalone {

struct Dx12Bootstrap::Impl {
    static const int NUM_FRAMES_IN_FLIGHT = 2;
    static const int NUM_BACK_BUFFERS = 2;
    static const int SRV_HEAP_SIZE = 64;

    struct FrameContext {
        ComPtr<ID3D12CommandAllocator> CommandAllocator;
        UINT64 FenceValue = 0;
    };

    ComPtr<ID3D12Device> Device;
    ComPtr<ID3D12DescriptorHeap> RtvHeap;
    ComPtr<ID3D12DescriptorHeap> SrvHeap;
    ComPtr<ID3D12CommandQueue> CommandQueue;
    ComPtr<ID3D12GraphicsCommandList> CommandList;
    ComPtr<ID3D12Fence> Fence;
    ComPtr<IDXGISwapChain3> SwapChain;
    ComPtr<ID3D12CommandAllocator> CaptureAllocator;

    HANDLE FenceEvent = nullptr;
    HANDLE SwapChainWaitableObject = nullptr; // owned by the swapchain
    UINT64 FenceLastSignaledValue = 0;
    UINT FrameIndex = 0;

    FrameContext Frames[NUM_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12Resource> RenderTargets[NUM_BACK_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandles[NUM_BACK_BUFFERS] = {};

    int Width = 0;
    int Height = 0;
    UINT CurrentBackBufferIdx = 0;
    int LastRenderedBackBufferIdx = 0;
    FrameContext* PendingFrameCtx = nullptr;
    std::string AdapterDesc;
    bool Warp = false;
    bool ImguiBackendReady = false;

    bool CreateDevice(HWND hwnd, std::string& err);
    bool CreateSwapChain(IDXGIFactory4* factory, HWND hwnd, std::string& err);
    void CreateRenderTargets();
    void CleanupRenderTargets();
    void FlushGpu();
    FrameContext* WaitForNextFrameResources();
};

bool Dx12Bootstrap::Impl::CreateDevice(HWND hwnd, std::string& err) {
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }
#endif
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        err = "CreateDXGIFactory1 failed.";
        return false;
    }
    if (!SelectAdapterAndCreateDevice(factory.Get(), Device, AdapterDesc, Warp, err)) {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CommandQueue)))) {
        err = "CreateCommandQueue failed.";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = NUM_BACK_BUFFERS;
    if (FAILED(Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&RtvHeap)))) {
        err = "RTV descriptor heap creation failed.";
        return false;
    }
    const SIZE_T rtvIncrement = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
        RtvHandles[i] = rtvHandle;
        rtvHandle.ptr += rtvIncrement;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = SRV_HEAP_SIZE;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&SrvHeap)))) {
        err = "SRV descriptor heap creation failed.";
        return false;
    }

    for (int i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i) {
        if (FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&Frames[i].CommandAllocator)))) {
            err = "CreateCommandAllocator failed.";
            return false;
        }
    }
    if (FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CaptureAllocator)))) {
        err = "Capture command allocator creation failed.";
        return false;
    }

    if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Frames[0].CommandAllocator.Get(), nullptr,
                                         IID_PPV_ARGS(&CommandList)))) {
        err = "CreateCommandList failed.";
        return false;
    }
    CommandList->Close();

    if (FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)))) {
        err = "CreateFence failed.";
        return false;
    }
    FenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!FenceEvent) {
        err = "Fence event creation failed.";
        return false;
    }

    if (!CreateSwapChain(factory.Get(), hwnd, err)) {
        return false;
    }
    CreateRenderTargets();
    return true;
}

bool Dx12Bootstrap::Impl::CreateSwapChain(IDXGIFactory4* factory, HWND hwnd, std::string& err) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount = NUM_BACK_BUFFERS;
    sd.Width = static_cast<UINT>(Width);
    sd.Height = static_cast<UINT>(Height);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Stereo = FALSE;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(CommandQueue.Get(), hwnd, &sd, nullptr, nullptr, &swapChain1))) {
        err = "CreateSwapChainForHwnd failed.";
        return false;
    }
    if (FAILED(swapChain1.As(&SwapChain))) {
        err = "IDXGISwapChain3 query failed.";
        return false;
    }
    SwapChain->SetMaximumFrameLatency(NUM_BACK_BUFFERS);
    SwapChainWaitableObject = SwapChain->GetFrameLatencyWaitableObject();
    return true;
}

void Dx12Bootstrap::Impl::CreateRenderTargets() {
    for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
        ComPtr<ID3D12Resource> backBuffer;
        SwapChain->GetBuffer(static_cast<UINT>(i), IID_PPV_ARGS(&backBuffer));
        Device->CreateRenderTargetView(backBuffer.Get(), nullptr, RtvHandles[i]);
        RenderTargets[i] = backBuffer;
    }
}

void Dx12Bootstrap::Impl::CleanupRenderTargets() {
    for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
        RenderTargets[i].Reset();
    }
}

void Dx12Bootstrap::Impl::FlushGpu() {
    if (!CommandQueue || !Fence || !FenceEvent) {
        return;
    }
    const UINT64 fenceValue = FenceLastSignaledValue + 1;
    if (FAILED(CommandQueue->Signal(Fence.Get(), fenceValue))) {
        return;
    }
    FenceLastSignaledValue = fenceValue;
    if (Fence->GetCompletedValue() < fenceValue) {
        Fence->SetEventOnCompletion(fenceValue, FenceEvent);
        WaitForSingleObject(FenceEvent, INFINITE);
    }
}

Dx12Bootstrap::Impl::FrameContext* Dx12Bootstrap::Impl::WaitForNextFrameResources() {
    const UINT nextFrameIndex = FrameIndex + 1;
    FrameIndex = nextFrameIndex;

    HANDLE waitableObjects[] = {SwapChainWaitableObject, nullptr};
    DWORD numWaitableObjects = 1;

    FrameContext* frameCtx = &Frames[nextFrameIndex % NUM_FRAMES_IN_FLIGHT];
    const UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue != 0) {
        frameCtx->FenceValue = 0;
        Fence->SetEventOnCompletion(fenceValue, FenceEvent);
        waitableObjects[1] = FenceEvent;
        numWaitableObjects = 2;
    }
    WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE);
    return frameCtx;
}

Dx12Bootstrap::Dx12Bootstrap() : d(std::make_unique<Impl>()) {}

Dx12Bootstrap::~Dx12Bootstrap() { Shutdown(); }

bool Dx12Bootstrap::Init(void* hwnd, int width, int height, std::string& err) {
    if (!hwnd) {
        err = "DX12 Init: null window handle.";
        return false;
    }
    d->Width = width > 0 ? width : 1;
    d->Height = height > 0 ? height : 1;
    if (!d->CreateDevice(static_cast<HWND>(hwnd), err)) {
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE fontCpu = d->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE fontGpu = d->SrvHeap->GetGPUDescriptorHandleForHeapStart();
    {
        std::lock_guard<std::mutex> lk(gSrvAllocator.Mutex);
        gSrvAllocator.CpuBase = fontCpu;
        gSrvAllocator.GpuBase = fontGpu;
        gSrvAllocator.IncrementSize = d->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        gSrvAllocator.Capacity = Impl::SRV_HEAP_SIZE;
        gSrvAllocator.OverflowSlot = Impl::SRV_HEAP_SIZE - 1;
        gSrvAllocator.NextSlot = 1;
        gSrvAllocator.FreeList.clear();
        gSrvAllocator.bLoggedExhausted = false;
    }

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = d->Device.Get();
    initInfo.CommandQueue = d->CommandQueue.Get();
    initInfo.NumFramesInFlight = Impl::NUM_FRAMES_IN_FLIGHT;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = d->SrvHeap.Get();
    initInfo.LegacySingleSrvCpuDescriptor = fontCpu;
    initInfo.LegacySingleSrvGpuDescriptor = fontGpu;
    initInfo.SrvDescriptorAllocFn = &SrvAllocCb;
    initInfo.SrvDescriptorFreeFn = &SrvFreeCb;
    if (!ImGui_ImplDX12_Init(&initInfo)) {
        err = "ImGui_ImplDX12_Init failed.";
        return false;
    }
    d->ImguiBackendReady = true;
    LOG_INFO("Standalone DX12 renderer initialised (adapter=\"%s\"%s).", d->AdapterDesc.c_str(),
             d->Warp ? ", WARP software" : "");
    return true;
}

void Dx12Bootstrap::NewFrame() {
    if (!d || !d->ImguiBackendReady) {
        return;
    }
    ImGui_ImplDX12_NewFrame();
}

void Dx12Bootstrap::BeginFrame(const ImVec4& clearColor) {
    if (!d || !d->SwapChain) {
        return;
    }
    Impl::FrameContext* frameCtx = d->WaitForNextFrameResources();
    const UINT backBufferIdx = d->SwapChain->GetCurrentBackBufferIndex();
    d->PendingFrameCtx = frameCtx;
    d->CurrentBackBufferIdx = backBufferIdx;
    d->LastRenderedBackBufferIdx = static_cast<int>(backBufferIdx);

    frameCtx->CommandAllocator->Reset();
    d->CommandList->Reset(frameCtx->CommandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = d->RenderTargets[backBufferIdx].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    d->CommandList->ResourceBarrier(1, &barrier);

    // Premultiply by alpha to match the GL path's glClearColor(bg*bg.w, bg.w).
    const float clear[4] = {clearColor.x * clearColor.w, clearColor.y * clearColor.w, clearColor.z * clearColor.w,
                            clearColor.w};
    d->CommandList->ClearRenderTargetView(d->RtvHandles[backBufferIdx], clear, 0, nullptr);
    d->CommandList->OMSetRenderTargets(1, &d->RtvHandles[backBufferIdx], FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {d->SrvHeap.Get()};
    d->CommandList->SetDescriptorHeaps(1, heaps);
}

void Dx12Bootstrap::RenderDrawData(ImDrawData* drawData) {
    if (!d || !drawData || !d->CommandList) {
        return;
    }
    ImGui_ImplDX12_RenderDrawData(drawData, d->CommandList.Get());
}

void Dx12Bootstrap::Present(int syncInterval) {
    if (!d || !d->SwapChain || !d->PendingFrameCtx) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = d->RenderTargets[d->CurrentBackBufferIdx].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    d->CommandList->ResourceBarrier(1, &barrier);
    d->CommandList->Close();

    ID3D12CommandList* lists[] = {d->CommandList.Get()};
    d->CommandQueue->ExecuteCommandLists(1, lists);

    d->SwapChain->Present(static_cast<UINT>(syncInterval), 0);

    const UINT64 fenceValue = d->FenceLastSignaledValue + 1;
    d->CommandQueue->Signal(d->Fence.Get(), fenceValue);
    d->FenceLastSignaledValue = fenceValue;
    d->PendingFrameCtx->FenceValue = fenceValue;
    d->PendingFrameCtx = nullptr;
}

void Dx12Bootstrap::Resize(int width, int height) {
    if (!d || !d->SwapChain) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    if (width == d->Width && height == d->Height) {
        return;
    }
    d->FlushGpu();
    d->CleanupRenderTargets();
    const HRESULT hr = d->SwapChain->ResizeBuffers(Impl::NUM_BACK_BUFFERS, static_cast<UINT>(width),
                                                   static_cast<UINT>(height), DXGI_FORMAT_R8G8B8A8_UNORM,
                                                   DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        LOG_ERROR("Standalone DX12: ResizeBuffers failed (0x%08lx).", static_cast<unsigned long>(hr));
        return;
    }
    d->Width = width;
    d->Height = height;
    d->CreateRenderTargets();
}

bool Dx12Bootstrap::CaptureBackbuffer(std::vector<uint8_t>& rgba, int& width, int& height) {
    if (!d || !d->Device || !d->SwapChain) {
        return false;
    }
    d->FlushGpu();
    ID3D12Resource* src = d->RenderTargets[d->LastRenderedBackBufferIdx].Get();
    if (!src) {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = src->GetDesc();
    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    d->Device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(d->Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
        return false;
    }

    d->CaptureAllocator->Reset();
    d->CommandList->Reset(d->CaptureAllocator.Get(), nullptr);
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = src;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    d->CommandList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = src;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;
    d->CommandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toCopy;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    d->CommandList->ResourceBarrier(1, &toPresent);
    d->CommandList->Close();

    ID3D12CommandList* lists[] = {d->CommandList.Get()};
    d->CommandQueue->ExecuteCommandLists(1, lists);
    d->FlushGpu();

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
    if (FAILED(readback->Map(0, &readRange, &mapped))) {
        return false;
    }
    rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&rgba[static_cast<size_t>(y) * static_cast<size_t>(w) * 4u],
                    srcBytes + static_cast<size_t>(footprint.Footprint.RowPitch) * static_cast<size_t>(y),
                    static_cast<size_t>(w) * 4u);
    }
    const D3D12_RANGE noWrite = {0, 0};
    readback->Unmap(0, &noWrite);
    width = w;
    height = h;
    return true;
}

void Dx12Bootstrap::Shutdown() {
    if (!d) {
        return;
    }
    if (d->CommandQueue && d->Fence && d->FenceEvent) {
        d->FlushGpu();
    }
    if (d->ImguiBackendReady) {
        ImGui_ImplDX12_Shutdown();
        d->ImguiBackendReady = false;
    }
    if (d->FenceEvent) {
        CloseHandle(d->FenceEvent);
        d->FenceEvent = nullptr;
    }
}

std::string Dx12Bootstrap::AdapterDescription() const { return d ? d->AdapterDesc : std::string(); }

bool Dx12Bootstrap::IsWarp() const { return d ? d->Warp : false; }

} // namespace standalone
} // namespace smatchet

#else // SMATCHET_STANDALONE_HAS_DX12

// Linkable no-op shell so std::unique_ptr<Dx12Bootstrap> compiles and links on
// every toolchain (macOS/Linux/MinGW). The render loops branch on a runtime
// StandaloneRenderer enum that is never set to Dx12 here, so none of these run.

namespace smatchet {
namespace standalone {

struct Dx12Bootstrap::Impl {};

Dx12Bootstrap::Dx12Bootstrap() : d(nullptr) {}
Dx12Bootstrap::~Dx12Bootstrap() = default;

bool Dx12Bootstrap::Init(void*, int, int, std::string& err) {
    err = "DX12 renderer is not available on this platform/toolchain.";
    return false;
}
void Dx12Bootstrap::NewFrame() {}
void Dx12Bootstrap::BeginFrame(const ImVec4&) {}
void Dx12Bootstrap::RenderDrawData(ImDrawData*) {}
void Dx12Bootstrap::Present(int) {}
void Dx12Bootstrap::Resize(int, int) {}
bool Dx12Bootstrap::CaptureBackbuffer(std::vector<uint8_t>&, int&, int&) { return false; }
void Dx12Bootstrap::Shutdown() {}
std::string Dx12Bootstrap::AdapterDescription() const { return std::string(); }
bool Dx12Bootstrap::IsWarp() const { return false; }

} // namespace standalone
} // namespace smatchet

#endif // SMATCHET_STANDALONE_HAS_DX12

// ---------------------------------------------------------------------------
// Renderer selection — common to every toolchain (outside the DX12 guard so the
// same resolution runs whether or not a real backend is compiled in). The
// window-hint choice happens before ParseStandaloneCli in both render loops, so
// this is a deliberate early argv scan rather than going through the full CLI
// parser.

#include "Logger.h"

#include <cstdlib>
#include <cstring>

namespace smatchet {
namespace standalone {

namespace {

// Return the value of `--renderer=<val>` or `--renderer <val>`, or nullptr.
const char* RendererArgValue(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!a) {
            continue;
        }
        if (std::strncmp(a, "--renderer=", 11) == 0) {
            return a + 11;
        }
        if (std::strcmp(a, "--renderer") == 0 && i + 1 < argc && argv[i + 1]) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

} // namespace

StandaloneRenderer ResolveStandaloneRenderer(int argc, char** argv) {
    const StandaloneRenderer platformDefault =
        SMATCHET_STANDALONE_HAS_DX12 ? StandaloneRenderer::Dx12 : StandaloneRenderer::OpenGL;

    // Precedence: explicit `--renderer` flag > SMATCHET_RENDERER env > platform
    // default. The env knob lets CI pin every standalone launch — including the
    // scenario.run buried inside scripts/dev/test-screenshot-diff.sh — to `gl`
    // via one workflow-level env var, keeping the Mesa-rendered bucket goldens
    // byte-stable without threading a flag through every launch site. A literal
    // `--renderer` flag still overrides the env.
    const char* val = RendererArgValue(argc, argv);
    if (!val || !*val) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
        val = std::getenv("SMATCHET_RENDERER");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }
    if (!val || !*val) {
        return platformDefault;
    }
    if (std::strcmp(val, "gl") == 0 || std::strcmp(val, "opengl") == 0) {
        return StandaloneRenderer::OpenGL;
    }
    if (std::strcmp(val, "dx12") == 0 || std::strcmp(val, "d3d12") == 0 || std::strcmp(val, "directx12") == 0) {
#if SMATCHET_STANDALONE_HAS_DX12
        return StandaloneRenderer::Dx12;
#else
        LOG_WARN("--renderer=dx12 requested but no DX12 backend on this platform/toolchain; using OpenGL.");
        return StandaloneRenderer::OpenGL;
#endif
    }
    LOG_WARN("Unknown renderer value '%s' (from --renderer or SMATCHET_RENDERER); using platform default.", val);
    return platformDefault;
}

} // namespace standalone
} // namespace smatchet
