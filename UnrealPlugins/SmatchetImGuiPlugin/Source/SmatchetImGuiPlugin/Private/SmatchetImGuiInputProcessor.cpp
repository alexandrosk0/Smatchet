#include "SmatchetImGuiInputProcessor.h"

#include "Framework/Application/SlateApplication.h"
#include "Logging/LogMacros.h"
#include "InputCoreTypes.h"
#include "SmatchetImGuiHostC.h"
#include "imgui.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetImGuiInputProcessor, Log, All);

FSmatchetImGuiInputProcessor::FSmatchetImGuiInputProcessor(SmatchetImGuiHostHandle InHost)
    : Host(InHost) {
}

void FSmatchetImGuiInputProcessor::Tick(const float, FSlateApplication&, TSharedRef<ICursor>) {
}

bool FSmatchetImGuiInputProcessor::HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& InKeyEvent) {
    if (!Host) {
        return false;
    }

    // Ctrl+Shift+J toggles ImGui visibility.
    // We handle this regardless of current visibility so the shortcut works when hidden.
    if (InKeyEvent.GetKey() == EKeys::J &&
        InKeyEvent.IsControlDown() &&
        InKeyEvent.IsShiftDown() &&
        !InKeyEvent.IsRepeat()) {
        SmatchetHost_ToggleUiVisible(Host);
        UE_LOG(LogSmatchetImGuiInputProcessor,
            Log,
            TEXT("Hotkey Ctrl+Shift+J toggled UI. UiVisible=%d Initialized=%d"),
            SmatchetHost_IsUiVisible(Host) ? 1 : 0,
            SmatchetHost_IsInitialized(Host) ? 1 : 0);
        return true;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const int32 ImGuiKey = ToImGuiKey(InKeyEvent);
    if (ImGuiKey >= 0) {
        SmatchetHost_SetKeyDown(Host, ImGuiKey, true);
    }

    // UE's IInputProcessor has no character callback; use FKeyEvent's character value instead.
    // This helps ImGui `InputText` widgets without implementing a custom Slate text input path.
    const TCHAR ch = InKeyEvent.GetCharacter();
    if (ch != 0) {
        SmatchetHost_AddInputCharacter(Host, static_cast<unsigned int>(ch));
    }
    PushModifierState(InKeyEvent);
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& InKeyEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const int32 ImGuiKey = ToImGuiKey(InKeyEvent);
    if (ImGuiKey >= 0) {
        SmatchetHost_SetKeyDown(Host, ImGuiKey, false);
    }
    PushModifierState(InKeyEvent);
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleAnalogInputEvent(FSlateApplication&, const FAnalogInputEvent&) {
    return false;
}

bool FSmatchetImGuiInputProcessor::HandleMouseMoveEvent(FSlateApplication&, const FPointerEvent& MouseEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const FVector2D P = MouseEvent.GetScreenSpacePosition();
    SmatchetHost_SetMousePosition(Host, static_cast<float>(P.X), static_cast<float>(P.Y));
    PushModifierState(MouseEvent);
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleMouseButtonDownEvent(FSlateApplication&, const FPointerEvent& MouseEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const int32 Button = ToImGuiMouseButton(MouseEvent);
    if (Button >= 0) {
        SmatchetHost_SetMouseButton(Host, Button, true);
    }
    PushModifierState(MouseEvent);
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleMouseButtonUpEvent(FSlateApplication&, const FPointerEvent& MouseEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const int32 Button = ToImGuiMouseButton(MouseEvent);
    if (Button >= 0) {
        SmatchetHost_SetMouseButton(Host, Button, false);
    }
    PushModifierState(MouseEvent);
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleMouseWheelOrGestureEvent(FSlateApplication&, const FPointerEvent& InWheelEvent, const FPointerEvent*) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    SmatchetHost_AddMouseWheel(Host, 0.0f, InWheelEvent.GetWheelDelta());
    PushModifierState(InWheelEvent);
    return true;
}

int32 FSmatchetImGuiInputProcessor::ToImGuiMouseButton(const FPointerEvent& MouseEvent) const {
    const FKey Key = MouseEvent.GetEffectingButton();
    if (Key == EKeys::LeftMouseButton) {
        return 0;
    }
    if (Key == EKeys::RightMouseButton) {
        return 1;
    }
    if (Key == EKeys::MiddleMouseButton) {
        return 2;
    }
    return -1;
}

int32 FSmatchetImGuiInputProcessor::ToImGuiKey(const FKeyEvent& InKeyEvent) const {
    const FKey Key = InKeyEvent.GetKey();

    if (Key == EKeys::Tab) return ImGuiKey_Tab;
    if (Key == EKeys::Left) return ImGuiKey_LeftArrow;
    if (Key == EKeys::Right) return ImGuiKey_RightArrow;
    if (Key == EKeys::Up) return ImGuiKey_UpArrow;
    if (Key == EKeys::Down) return ImGuiKey_DownArrow;
    if (Key == EKeys::PageUp) return ImGuiKey_PageUp;
    if (Key == EKeys::PageDown) return ImGuiKey_PageDown;
    if (Key == EKeys::Home) return ImGuiKey_Home;
    if (Key == EKeys::End) return ImGuiKey_End;
    if (Key == EKeys::Insert) return ImGuiKey_Insert;
    if (Key == EKeys::Delete) return ImGuiKey_Delete;
    if (Key == EKeys::BackSpace) return ImGuiKey_Backspace;
    if (Key == EKeys::SpaceBar) return ImGuiKey_Space;
    if (Key == EKeys::Enter) return ImGuiKey_Enter;
    if (Key == EKeys::Escape) return ImGuiKey_Escape;

    if (Key == EKeys::A) return ImGuiKey_A;
    if (Key == EKeys::C) return ImGuiKey_C;
    if (Key == EKeys::V) return ImGuiKey_V;
    if (Key == EKeys::X) return ImGuiKey_X;
    if (Key == EKeys::Y) return ImGuiKey_Y;
    if (Key == EKeys::Z) return ImGuiKey_Z;

    return -1;
}

void FSmatchetImGuiInputProcessor::PushModifierState(const FInputEvent& InputEvent) const {
    if (!Host) {
        return;
    }
    SmatchetHost_SetKeyModifiers(Host,
                                  InputEvent.IsControlDown(),
                                  InputEvent.IsShiftDown(),
                                  InputEvent.IsAltDown(),
                                  InputEvent.IsCommandDown());
}
