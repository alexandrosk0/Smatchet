// SmatchetImageTextureCache destroy lifecycle against a real (headless) ImGui context: eviction queues a
// destroy, the per-frame tick walks it to Destroyed with a simulated renderer backend, and
// DropAllForContextTeardown releases everything before the context goes. Run under ASan/LeakSanitizer in
// the sanitizer lanes: a texture the cache fails to free is a reported leak.

#include <doctest/doctest.h>

#include "ScopedLimits.h"
#include "SmatchetImageTextureCache.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture

#include <string>
#include <vector>

namespace {

struct HeadlessImGui {
    HeadlessImGui() : ctx_(ImGui::CreateContext()) {
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }
    ~HeadlessImGui() {
        // Mirrors every DestroyContext site: drop the cache first (the backend has already shut down).
        SmatchetImageTextureCache::DropAllForContextTeardown();
        if (ctx_ != nullptr) {
            ImGui::DestroyContext(ctx_);
        }
    }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;
    ImGuiContext* ctx_;
};

std::vector<unsigned char> SolidRgba(int w, int h) {
    return std::vector<unsigned char>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0x7f);
}

// What a renderer backend does once it has uploaded the texture: hand out a TexID and backend data.
int g_fakeBackendObject = 0;
void SimulateUpload(ImTextureData* tex) {
    tex->SetTexID(static_cast<ImTextureID>(1));
    tex->BackendUserData = &g_fakeBackendObject;
    tex->Status = ImTextureStatus_OK;
}

// What the backend does when it frees the GPU object (GL3 DestroyTexture / DX12 DestroyTexture).
void SimulateBackendDestroy(ImTextureData* tex) {
    tex->SetTexID(ImTextureID_Invalid);
    tex->BackendUserData = nullptr;
    tex->SetStatus(ImTextureStatus_Destroyed);
}

} // namespace

TEST_CASE("TextureCacheTeardown: an evicted, never-uploaded texture is freed on the first tick") {
    HeadlessImGui gui;
    smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.ImageTextureCacheMaxEntries = 16; });
    const std::vector<unsigned char> px = SolidRgba(8, 8);
    std::vector<ImTextureData*> handed;
    for (int i = 0; i < 17; ++i) {
        const Result<SmatchetLoadedIconTexture> r =
            SmatchetImageTextureCache::GetOrCreateFromRgba("k" + std::to_string(i), px, 8, 8);
        REQUIRE(r.has_value());
        handed.push_back(r.value().Texture);
    }
    // The 17th insert evicted the LRU tail ("k0"): queued, not yet freed, status untouched.
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 1);
    CHECK(handed[0]->WantDestroyNextFrame);
    CHECK(handed[0]->Status == ImTextureStatus_WantCreate);

    SmatchetImageTextureCache::TickPendingDestroys();
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 0);
    CHECK(SmatchetImageTextureCache::IconCacheEntryCount() == 16);
}

TEST_CASE("TextureCacheTeardown: an uploaded texture walks WantDestroy to Destroyed before it is freed") {
    HeadlessImGui gui;
    smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.ImageTextureCacheMaxEntries = 16; });
    const std::vector<unsigned char> px = SolidRgba(8, 8);
    const Result<SmatchetLoadedIconTexture> first = SmatchetImageTextureCache::GetOrCreateFromRgba("k0", px, 8, 8);
    REQUIRE(first.has_value());
    ImTextureData* victim = first.value().Texture;
    SimulateUpload(victim);
    for (int i = 1; i < 17; ++i) {
        REQUIRE(SmatchetImageTextureCache::GetOrCreateFromRgba("k" + std::to_string(i), px, 8, 8).has_value());
    }
    REQUIRE(SmatchetImageTextureCache::PendingDestroyCount() == 1);
    CHECK(victim->Status == ImTextureStatus_OK); // eviction alone leaves the status to the tick

    SmatchetImageTextureCache::TickPendingDestroys(); // promote
    CHECK(victim->Status == ImTextureStatus_WantDestroy);
    CHECK(victim->UnusedFrames == 1);
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 1);

    SmatchetImageTextureCache::TickPendingDestroys(); // backend has not freed yet: still waiting
    CHECK(victim->UnusedFrames == 2);
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 1);

    SimulateBackendDestroy(victim);
    CHECK(victim->Status == ImTextureStatus_Destroyed); // stays Destroyed: WantDestroyNextFrame is set
    SmatchetImageTextureCache::TickPendingDestroys();
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 0);
}

TEST_CASE("TextureCacheTeardown: an external owner's texture shares the queue and is queued once") {
    HeadlessImGui gui;
    // The attachment preview owns textures outside the LRU and hands them over through
    // QueueUserTextureDestroy.
    ImTextureData* tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, 4, 4);
    tex->Status = ImTextureStatus_WantCreate;
    ImGui::RegisterUserTexture(tex);

    SmatchetImageTextureCache::QueueUserTextureDestroy(tex);
    SmatchetImageTextureCache::QueueUserTextureDestroy(tex); // idempotent
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 1);
    SmatchetImageTextureCache::QueueUserTextureDestroy(nullptr); // tolerated
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 1);

    SmatchetImageTextureCache::TickPendingDestroys(); // never uploaded: freed on the first tick
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 0);
}

TEST_CASE("TextureCacheTeardown: DropAllForContextTeardown frees resident and queued textures") {
    {
        HeadlessImGui gui;
        smatchet_tests::ScopedLimits limits(
            [](smatchet::limits::LimitsConfig& l) { l.ImageTextureCacheMaxEntries = 16; });
        const std::vector<unsigned char> px = SolidRgba(8, 8);
        for (int i = 0; i < 20; ++i) {
            REQUIRE(SmatchetImageTextureCache::GetOrCreateFromRgba("k" + std::to_string(i), px, 8, 8).has_value());
        }
        CHECK(SmatchetImageTextureCache::IconCacheEntryCount() == 16);
        CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 4);

        SmatchetImageTextureCache::DropAllForContextTeardown();
        CHECK(SmatchetImageTextureCache::IconCacheEntryCount() == 0);
        CHECK(SmatchetImageTextureCache::IconCacheApproxBytes() == 0);
        CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 0);
    } // ~HeadlessImGui drops again (idempotent) and destroys the context; LeakSanitizer checks the rest

    // A second context after the first one is gone starts from an empty cache.
    HeadlessImGui second;
    SmatchetLoadedIconTexture out;
    CHECK_FALSE(SmatchetImageTextureCache::TryGetCached("k0", out));
    CHECK(SmatchetImageTextureCache::GetOrCreateFromRgba("k0", SolidRgba(8, 8), 8, 8).has_value());
}

TEST_CASE("TextureCacheTeardown: DropAllForContextTeardown with no context does not touch ImGui") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    REQUIRE(SmatchetImageTextureCache::GetOrCreateFromRgba("orphan", SolidRgba(4, 4), 4, 4).has_value());
    ImGui::DestroyContext(ctx);                             // the host tore the context down first (wrong order)
    SmatchetImageTextureCache::DropAllForContextTeardown(); // must neither crash nor unregister
    CHECK(SmatchetImageTextureCache::IconCacheEntryCount() == 0);
}

TEST_CASE("TextureCacheTeardown: CreateUserTexture validates its input and registers the texture") {
    HeadlessImGui gui;
    ImTextureData* tex = nullptr;
    std::string err;
    CHECK_FALSE(SmatchetImageTextureCache::CreateUserTexture(std::vector<unsigned char>(), 4, 4, tex, err));
    CHECK(tex == nullptr);
    CHECK_FALSE(err.empty());
    CHECK_FALSE(SmatchetImageTextureCache::CreateUserTexture(SolidRgba(2, 2), 4, 4, tex, err)); // short buffer
    CHECK(tex == nullptr);
    REQUIRE(SmatchetImageTextureCache::CreateUserTexture(SolidRgba(4, 4), 4, 4, tex, err));
    REQUIRE(tex != nullptr);
    CHECK(tex->Status == ImTextureStatus_WantCreate);
    CHECK(tex->RefCount == 1); // RegisterUserTexture
    SmatchetImageTextureCache::QueueUserTextureDestroy(tex);
    SmatchetImageTextureCache::TickPendingDestroys();
    CHECK(SmatchetImageTextureCache::PendingDestroyCount() == 0);
}
