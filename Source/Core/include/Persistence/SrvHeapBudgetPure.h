#pragma once

// Descriptor budget of the shader-visible SRV heap behind ImGui user textures on DX12. Both DX12 hosts
// (standalone Dx12Bootstrap, and SmatchetImGuiHost for the Unreal plugin) give each live user texture one
// descriptor; once the heap is full every further texture spills onto one shared overflow slot and unrelated
// textures alias. The heap must out-size SmatchetImageTextureCache (ImageTextureCacheMaxEntries) plus the
// attachment previews and the evictions still waiting out their frames in flight. Header-only for the Linux
// doctest subset.

namespace smatchet {
namespace persistence {

/// Slots that never hold a cache icon: the font atlas, the overflow slot, and headroom for thumbnails and
/// textures awaiting their destroy frames.
constexpr int kSrvHeapReservedDescriptors = 64;

/// Descriptors the standalone host creates its heap with (the Unreal plugin mirrors this literal).
constexpr int kDx12SrvHeapDescriptors = 1024;

/// True when `heapCapacity` descriptors hold `cacheMaxEntries` icon textures plus the reserved slots.
constexpr bool SrvHeapCoversTextureCache(int heapCapacity, int cacheMaxEntries) {
    return heapCapacity > 0 && cacheMaxEntries >= 0 && cacheMaxEntries <= heapCapacity - kSrvHeapReservedDescriptors;
}

} // namespace persistence
} // namespace smatchet
