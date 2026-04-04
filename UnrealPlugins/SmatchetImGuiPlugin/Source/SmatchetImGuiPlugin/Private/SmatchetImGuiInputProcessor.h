#pragma once

#include "Framework/Application/IInputProcessor.h"
#include "Input/Events.h"
#include "Templates/SharedPointer.h"

#include "SmatchetImGuiHostC.h"

class FSmatchetImGuiInputProcessor : public IInputProcessor {
public:
    explicit FSmatchetImGuiInputProcessor(SmatchetImGuiHostHandle InHost);

    virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
    virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
    virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override;
    virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override;

private:
    int32 ToImGuiMouseButton(const FPointerEvent& MouseEvent) const;
    int32 ToImGuiKey(const FKeyEvent& InKeyEvent) const;
    void PushModifierState(const FInputEvent& InputEvent) const;

    SmatchetImGuiHostHandle Host = nullptr;
};
