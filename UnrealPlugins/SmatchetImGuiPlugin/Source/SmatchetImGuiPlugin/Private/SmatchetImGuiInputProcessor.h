#pragma once

#include "Containers/Array.h"
#include "Framework/Application/IInputProcessor.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

#include <atomic>
#include <cstdint>

#include "SmatchetImGuiHostC.h"

class UGameViewportClient;

// Written on the game thread from FSmatchetImGuiInputProcessor::Tick and read on the render thread from
// SmatchetImGuiPluginModule's OnBackBufferReadyToPresent hook. Holds the ISlateViewport pointer identity
// (as uintptr_t) currently under the OS pointer, or 0 when not over a game viewport.
extern std::atomic<std::uintptr_t> GSmatchetPointerOverSlateViewportId;

class FSmatchetImGuiInputProcessor : public IInputProcessor {
public:
    explicit FSmatchetImGuiInputProcessor(SmatchetImGuiHostHandle InHost);

    /** Call before UnregisterInputPreProcessor so editor tooltips are not left disabled. */
    void EnsureTooltipsRestored();

    virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
    virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
    virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override;
    virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override;

private:
    void SyncSlateTooltipPolicy(FSlateApplication& SlateApp);
    void RestoreViewportMouseSnapshotIfActive();

    struct FSmatchetSavedViewportMouse {
        TWeakObjectPtr<UGameViewportClient> WeakClient;
        EMouseCaptureMode CaptureMode = EMouseCaptureMode::NoCapture;
        EMouseLockMode LockMode = EMouseLockMode::DoNotLock;
        bool bSceneHadMouseCapture = false;
        /** Slate lock-to-viewport; approximated from lock mode (no public query on FSceneViewport). */
        bool bWantsSlateLockToViewport = false;
    };

    int32 ToImGuiMouseButton(const FPointerEvent& MouseEvent) const;
    int32 ToImGuiKey(const FKeyEvent& InKeyEvent) const;
    void PushModifierState(const FInputEvent& InputEvent) const;

    SmatchetImGuiHostHandle Host = nullptr;
    bool bSlateTooltipSuppressionActive = false;
    bool bAllowTooltipsBeforeSmatchet = true;

    /** True while Tick is applying NoCapture / DoNotLock / unlock to game viewports for the overlay. */
    bool bOverlayViewportStompActive = false;
    TArray<FSmatchetSavedViewportMouse> SavedViewportMouseModes;
};
