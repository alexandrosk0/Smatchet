#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include <atomic>

#include "SmatchetImGuiHostC.h"

class FSmatchetImGuiViewExtension : public FSceneViewExtensionBase {
public:
    FSmatchetImGuiViewExtension(const FAutoRegister& AutoRegister, SmatchetImGuiHostHandle InHost);

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
    virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
    SmatchetImGuiHostHandle Host = nullptr;
    // Signals game-thread created a new ImGui frame; consumed on render thread.
    std::atomic<int32> PendingRenderFrame{0};
};
