#include "SmatchetImGuiInputProcessor.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/ICursor.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "Slate/SceneViewport.h"
#include "SmatchetImGuiHostC.h"
#include "Widgets/SWindow.h"
#include "HAL/Platform.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif
#include "imgui.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetImGuiInputProcessor, Log, All);

std::atomic<std::uintptr_t> GSmatchetPointerOverSlateViewportId{0};

namespace {

/**
 * While a mouse button is held, Slate's "window under mouse" can change (e.g. crossing a table column
 * splitter or chrome), which makes per-event LocateWindowUnderMouse() subtract a different client
 * origin and teleports ImGui's io.MousePos — ImGui table column resize then breaks. We snapshot the
 * top-level SWindow on the first button down and keep subtracting that window's client rect until
 * all tracked buttons are released.
 */
static TWeakPtr<SWindow> GSmatchetMouseCoordWindow;
static int32 GSmatchetMouseButtonSessionDepth = 0;

static void ResetMouseCoordMapping() {
    GSmatchetMouseCoordWindow.Reset();
    GSmatchetMouseButtonSessionDepth = 0;
}

// ImGui DisplaySize matches the presenting window's client in pixel space; Slate pointer events use
// global "screen" coordinates. Subtract the top-level window client origin so editor (large offset)
// matches PIE/standalone where the offset is often small.
FVector2f MapPointerToClientLocal(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) {
    const FVector2f ScreenPos(MouseEvent.GetScreenSpacePosition());
    if (GSmatchetMouseButtonSessionDepth > 0) {
        if (const TSharedPtr<SWindow> Win = GSmatchetMouseCoordWindow.Pin()) {
            const FSlateRect ClientRect = Win->GetClientRectInScreen();
            return FVector2f(ScreenPos.X - ClientRect.Left, ScreenPos.Y - ClientRect.Top);
        }
    }
    const FWidgetPath Path =
        SlateApp.LocateWindowUnderMouse(ScreenPos, SlateApp.GetInteractiveTopLevelWindows(),
                                        /*bIgnoreEnabledStatus=*/true, static_cast<int32>(MouseEvent.GetUserIndex()));
    if (!Path.IsValid()) {
        return ScreenPos;
    }
    const FSlateRect ClientRect = Path.GetWindow()->GetClientRectInScreen();
    return FVector2f(ScreenPos.X - ClientRect.Left, ScreenPos.Y - ClientRect.Top);
}

} // namespace

FSmatchetImGuiInputProcessor::FSmatchetImGuiInputProcessor(SmatchetImGuiHostHandle InHost) : Host(InHost) {}

void FSmatchetImGuiInputProcessor::EnsureTooltipsRestored() {
    ResetMouseCoordMapping();
    RestoreViewportMouseSnapshotIfActive();
    if (!FSlateApplication::IsInitialized()) {
        return;
    }
    if (bSlateTooltipSuppressionActive) {
        FSlateApplication::Get().SetAllowTooltips(bAllowTooltipsBeforeSmatchet);
        bSlateTooltipSuppressionActive = false;
    }
}

void FSmatchetImGuiInputProcessor::RestoreViewportMouseSnapshotIfActive() {
    if (!bOverlayViewportStompActive) {
        return;
    }
    for (const FSmatchetSavedViewportMouse& Saved : SavedViewportMouseModes) {
        UGameViewportClient* Client = Saved.WeakClient.Get();
        if (!Client) {
            continue;
        }
        Client->SetMouseCaptureMode(Saved.CaptureMode);
        Client->SetMouseLockMode(Saved.LockMode);
        if (FSceneViewport* SceneViewport = Client->GetGameViewport()) {
            SceneViewport->LockMouseToViewport(Saved.bWantsSlateLockToViewport);
            SceneViewport->CaptureMouse(Saved.bSceneHadMouseCapture);
        }
    }
    SavedViewportMouseModes.Reset();
    bOverlayViewportStompActive = false;
}

void FSmatchetImGuiInputProcessor::SyncSlateTooltipPolicy(FSlateApplication& SlateApp) {
    if (!Host) {
        return;
    }
    const bool bUiVisible = SmatchetHost_IsUiVisible(Host) != 0;
    if (!bUiVisible) {
        // Restore as soon as visibility goes false, even before/without the Tick() branch.
        RestoreViewportMouseSnapshotIfActive();
    }
    if (bUiVisible && !bSlateTooltipSuppressionActive) {
        bAllowTooltipsBeforeSmatchet = SlateApp.GetAllowTooltips();
        SlateApp.SetAllowTooltips(false);
        bSlateTooltipSuppressionActive = true;
    } else if (!bUiVisible && bSlateTooltipSuppressionActive) {
        SlateApp.SetAllowTooltips(bAllowTooltipsBeforeSmatchet);
        bSlateTooltipSuppressionActive = false;
    }
}

void FSmatchetImGuiInputProcessor::Tick(const float, FSlateApplication& SlateApp, TSharedRef<ICursor>) {
    SyncSlateTooltipPolicy(SlateApp);

    // When the Smatchet overlay is up, keep the pointer free from any game viewport lock/capture so the
    // cursor can reach ImGui windows that extend beyond the PIE viewport rect. Reassert every frame
    // because the game loop (PlayerController input mode, FSceneViewport focus handlers, etc.) re-applies
    // capture and the Windows ClipCursor rect on its own. The crucial bit for "game is consuming the
    // mouse" is calling Cursor->Lock(nullptr): that issues ClipCursor(NULL) on Win32 and releases the
    // OS-level confinement even when UE left MouseLockMode=LockAlways behind. ImGui draws its own sprite
    // so we don't care if the OS cursor is hidden. When the UI hides we restore the snapshot taken on
    // first overlay frame so games that do not re-apply input mode every tick are not left in NoCapture.
    const bool bWantViewportStomp = GEngine && Host && (SmatchetHost_IsUiVisible(Host) != 0);

    if (!bWantViewportStomp && bOverlayViewportStompActive) {
        ResetMouseCoordMapping();
        RestoreViewportMouseSnapshotIfActive();
    }

    std::uintptr_t PointerOverSlateViewportId = 0;
    if (bWantViewportStomp) {
        if (!bOverlayViewportStompActive) {
            SavedViewportMouseModes.Reset();
            bOverlayViewportStompActive = true;
        }
        // Capture once per viewport before we stomp: first overlay frame (full list) or when a new PIE
        // viewport appears mid-overlay (append only). Do not refresh existing entries — they hold the
        // pre-overlay state to restore on hide.
        for (UGameViewportClient* Client = GEngine->GameViewport; Client != nullptr;
             Client = GEngine->GetNextPIEViewport(Client)) {
            bool bAlreadySaved = false;
            for (const FSmatchetSavedViewportMouse& Existing : SavedViewportMouseModes) {
                if (Existing.WeakClient.Get() == Client) {
                    bAlreadySaved = true;
                    break;
                }
            }
            if (bAlreadySaved) {
                continue;
            }
            FSmatchetSavedViewportMouse Saved;
            Saved.WeakClient = Client;
            Saved.CaptureMode = Client->GetMouseCaptureMode();
            Saved.LockMode = Client->GetMouseLockMode();
            FSceneViewport* SceneViewport = Client->GetGameViewport();
            Saved.bSceneHadMouseCapture = SceneViewport && SceneViewport->HasMouseCapture();
            Saved.bWantsSlateLockToViewport = (Saved.LockMode != EMouseLockMode::DoNotLock);
            SavedViewportMouseModes.Add(Saved);
        }

        SlateApp.ReleaseAllPointerCapture();
        // Query the live OS cursor state: EMouseCursor::None means the game hid the native pointer (e.g.
        // FInputModeGameOnly, or PC->bShowMouseCursor=false after Slate applied it). That's the only time
        // we want ImGui to draw its sprite inside the viewport — otherwise Slate already renders the OS
        // arrow and adding ours produces a double cursor.
        EMouseCursor::Type OsCursorType = EMouseCursor::Default;
        if (const TSharedPtr<GenericApplication> PlatformApp = SlateApp.GetPlatformApplication()) {
            if (const TSharedPtr<ICursor> PlatformCursor = PlatformApp->Cursor) {
                PlatformCursor->Lock(nullptr);
                OsCursorType = PlatformCursor->GetType();
            }
        }
        const bool bOsCursorHidden = OsCursorType == EMouseCursor::None;
        const FVector2D MousePos = SlateApp.GetCursorPos();
        for (UGameViewportClient* Client = GEngine->GameViewport; Client != nullptr;
             Client = GEngine->GetNextPIEViewport(Client)) {
            Client->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
            Client->SetMouseLockMode(EMouseLockMode::DoNotLock);
            FSceneViewport* SceneViewport = Client->GetGameViewport();
            if (!SceneViewport) {
                continue;
            }
            SceneViewport->LockMouseToViewport(false);
            SceneViewport->CaptureMouse(false);
            if (!bOsCursorHidden) {
                continue;
            }
            const FGeometry& Geo = SceneViewport->GetCachedGeometry();
            const FVector2D TopLeft = Geo.GetAbsolutePosition();
            const FVector2D Size = Geo.GetAbsoluteSize();
            if (MousePos.X >= TopLeft.X && MousePos.Y >= TopLeft.Y && MousePos.X < TopLeft.X + Size.X &&
                MousePos.Y < TopLeft.Y + Size.Y) {
                if (PointerOverSlateViewportId == 0) {
                    PointerOverSlateViewportId =
                        reinterpret_cast<std::uintptr_t>(static_cast<ISlateViewport*>(SceneViewport));
                }
            }
        }
    }
    GSmatchetPointerOverSlateViewportId.store(PointerOverSlateViewportId, std::memory_order_relaxed);
}

bool FSmatchetImGuiInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) {
    if (!Host) {
        return false;
    }

    // Ctrl+Alt+J: toggle suppression of ImGui's software mouse sprite inside game viewports. Default is
    // "draw" so PIE / standalone / packaged always have a visible pointer; press this if you ever see two
    // cursors (the OS one plus ImGui's) and want to hide the software one.
    if (InKeyEvent.GetKey() == EKeys::J && InKeyEvent.IsControlDown() && InKeyEvent.IsAltDown() &&
        !InKeyEvent.IsShiftDown() && !InKeyEvent.IsRepeat()) {
        const bool bNext = !SmatchetHost_GetSuppressSoftwareCursor(Host);
        SmatchetHost_SetSuppressSoftwareCursor(Host, bNext);
        UE_LOG(LogSmatchetImGuiInputProcessor, Log,
               TEXT("Hotkey Ctrl+Alt+J toggled software cursor suppression. Suppress=%d"), bNext ? 1 : 0);
        return true;
    }

    // Ctrl+Shift+J toggles ImGui visibility.
    // We handle this regardless of current visibility so the shortcut works when hidden.
    if (InKeyEvent.GetKey() == EKeys::J && InKeyEvent.IsControlDown() && InKeyEvent.IsShiftDown() &&
        !InKeyEvent.IsAltDown() && !InKeyEvent.IsRepeat()) {
        SmatchetHost_ToggleUiVisible(Host);
        SyncSlateTooltipPolicy(SlateApp);
        UE_LOG(LogSmatchetImGuiInputProcessor, Log, TEXT("Hotkey Ctrl+Shift+J toggled UI. UiVisible=%d Initialized=%d"),
               SmatchetHost_IsUiVisible(Host) ? 1 : 0, SmatchetHost_IsInitialized(Host) ? 1 : 0);
        return true;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    // Modifiers must reach ImGui before the key event so Ctrl+V / Ctrl+C / Ctrl+A shortcuts
    // see io.KeyCtrl on the same frame. UE often reports IsControlDown() on the V keydown
    // without a prior Left-Control key event when the chord is pressed quickly.
    PushModifierState(InKeyEvent);

    const int32 ImGuiKey = ToImGuiKey(InKeyEvent);
    if (ImGuiKey >= 0) {
        SmatchetHost_SetKeyDown(Host, ImGuiKey, true);
    }

    // UE's IInputProcessor has no character callback; use FKeyEvent's character value instead.
    // This helps ImGui `InputText` widgets without implementing a custom Slate text input path.
    // GetCharacter() is uint32; avoid TCHAR (may be 16-bit) to prevent C4244 narrowing warnings.
    // Do not inject characters when modifiers are held — UE still returns 'v' for Ctrl+V and
    // ImGui would type it instead of pasting from the clipboard.
    if (!HasTextInputBlockingModifier(InKeyEvent)) {
        const uint32 Character = InKeyEvent.GetCharacter();
        if (Character != 0) {
            SmatchetHost_AddInputCharacter(Host, static_cast<unsigned int>(Character));
        }
    }
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& InKeyEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    PushModifierState(InKeyEvent);

    const int32 ImGuiKey = ToImGuiKey(InKeyEvent);
    if (ImGuiKey >= 0) {
        SmatchetHost_SetKeyDown(Host, ImGuiKey, false);
    }
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleAnalogInputEvent(FSlateApplication&, const FAnalogInputEvent&) {
    return false;
}

bool FSmatchetImGuiInputProcessor::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const FVector2f P = MapPointerToClientLocal(SlateApp, MouseEvent);
    SmatchetHost_SetMousePosition(Host, P.X, P.Y);
    PushModifierState(MouseEvent);
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleMouseButtonDownEvent(FSlateApplication& SlateApp,
                                                              const FPointerEvent& MouseEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const int32 Button = ToImGuiMouseButton(MouseEvent);
    if (Button >= 0) {
        if (GSmatchetMouseButtonSessionDepth == 0) {
            const FVector2f ScreenPos(MouseEvent.GetScreenSpacePosition());
            const FWidgetPath Path = SlateApp.LocateWindowUnderMouse(
                ScreenPos, SlateApp.GetInteractiveTopLevelWindows(),
                /*bIgnoreEnabledStatus=*/true, static_cast<int32>(MouseEvent.GetUserIndex()));
            if (Path.IsValid()) {
                GSmatchetMouseCoordWindow = Path.GetWindow();
            } else {
                GSmatchetMouseCoordWindow.Reset();
            }
        }
        ++GSmatchetMouseButtonSessionDepth;
    }

    const FVector2f P = MapPointerToClientLocal(SlateApp, MouseEvent);
    SmatchetHost_SetMousePosition(Host, P.X, P.Y);
    // Apply modifier keys before mouse button so ImGui io.KeyCtrl/io.KeyShift match this click
    // (was: button first → io still stale until PushModifierState; grid + widgets read wrong branch).
    PushModifierState(MouseEvent);

    if (Button >= 0) {
        SmatchetHost_SetMouseButton(Host, Button, true);
    }
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleMouseButtonUpEvent(FSlateApplication& SlateApp,
                                                            const FPointerEvent& MouseEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const FVector2f P = MapPointerToClientLocal(SlateApp, MouseEvent);
    SmatchetHost_SetMousePosition(Host, P.X, P.Y);
    PushModifierState(MouseEvent);

    const int32 Button = ToImGuiMouseButton(MouseEvent);
    if (Button >= 0) {
        SmatchetHost_SetMouseButton(Host, Button, false);
        GSmatchetMouseButtonSessionDepth = FMath::Max(0, GSmatchetMouseButtonSessionDepth - 1);
        if (GSmatchetMouseButtonSessionDepth == 0) {
            GSmatchetMouseCoordWindow.Reset();
        }
    }
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp,
                                                                     const FPointerEvent& MouseEvent) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    const FVector2f P = MapPointerToClientLocal(SlateApp, MouseEvent);
    SmatchetHost_SetMousePosition(Host, P.X, P.Y);
    PushModifierState(MouseEvent);

    // Slate routes double-clicks through a separate path from MouseButtonDown. If we do not
    // handle it here, the event reaches editor widgets (asset opens, play bar, etc.) even
    // though the Smatchet UI is visible.
    const int32 Button = ToImGuiMouseButton(MouseEvent);
    if (Button >= 0) {
        SmatchetHost_SetMouseButton(Host, Button, true);
    }
    return true;
}

bool FSmatchetImGuiInputProcessor::HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp,
                                                                  const FPointerEvent& InWheelEvent,
                                                                  const FPointerEvent*) {
    if (!Host) {
        return false;
    }

    if (!SmatchetHost_IsUiVisible(Host)) {
        return false;
    }

    // Wheel deltas feed ImGui io the same way as standalone; table-level routing
    // (e.g. vertical-end horizontal scroll) lives in shared UI code on the host side.

    const FVector2f P = MapPointerToClientLocal(SlateApp, InWheelEvent);
    SmatchetHost_SetMousePosition(Host, P.X, P.Y);

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
    if (Key == EKeys::ThumbMouseButton) {
        return 3;
    }
    if (Key == EKeys::ThumbMouseButton2) {
        return 4;
    }
    return -1;
}

int32 FSmatchetImGuiInputProcessor::ToImGuiKey(const FKeyEvent& InKeyEvent) const {
    const FKey Key = InKeyEvent.GetKey();

    if (Key == EKeys::Tab)
        return ImGuiKey_Tab;
    if (Key == EKeys::Left)
        return ImGuiKey_LeftArrow;
    if (Key == EKeys::Right)
        return ImGuiKey_RightArrow;
    if (Key == EKeys::Up)
        return ImGuiKey_UpArrow;
    if (Key == EKeys::Down)
        return ImGuiKey_DownArrow;
    if (Key == EKeys::PageUp)
        return ImGuiKey_PageUp;
    if (Key == EKeys::PageDown)
        return ImGuiKey_PageDown;
    if (Key == EKeys::Home)
        return ImGuiKey_Home;
    if (Key == EKeys::End)
        return ImGuiKey_End;
    if (Key == EKeys::Insert)
        return ImGuiKey_Insert;
    if (Key == EKeys::Delete)
        return ImGuiKey_Delete;
    if (Key == EKeys::BackSpace)
        return ImGuiKey_Backspace;
    if (Key == EKeys::SpaceBar)
        return ImGuiKey_Space;
    if (Key == EKeys::Enter)
        return ImGuiKey_Enter;
    if (Key == EKeys::Escape)
        return ImGuiKey_Escape;

    if (Key == EKeys::A)
        return ImGuiKey_A;
    if (Key == EKeys::C)
        return ImGuiKey_C;
    if (Key == EKeys::V)
        return ImGuiKey_V;
    if (Key == EKeys::X)
        return ImGuiKey_X;
    if (Key == EKeys::Y)
        return ImGuiKey_Y;
    if (Key == EKeys::Z)
        return ImGuiKey_Z;

    return -1;
}

bool FSmatchetImGuiInputProcessor::HasTextInputBlockingModifier(const FKeyEvent& InKeyEvent) const {
#if PLATFORM_WINDOWS
    const bool asyncCtrl =
        ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
    const bool asyncAlt =
        ((::GetAsyncKeyState(VK_LMENU) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RMENU) & 0x8000) != 0);
#else
    const bool asyncCtrl = false;
    const bool asyncAlt = false;
#endif
    return InKeyEvent.IsControlDown() || InKeyEvent.IsAltDown() || InKeyEvent.IsCommandDown() || asyncCtrl ||
           asyncAlt;
}

void FSmatchetImGuiInputProcessor::PushModifierState(const FInputEvent& InputEvent) const {
    if (!Host) {
        return;
    }
    const FModifierKeysState Mods = FSlateApplication::Get().GetModifierKeys();
#if PLATFORM_WINDOWS
    const bool asyncCtrl =
        ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
    const bool asyncShift =
        ((::GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
#else
    const bool asyncCtrl = false;
    const bool asyncShift = false;
#endif
    const bool ctrl = InputEvent.IsControlDown() || Mods.IsControlDown() || asyncCtrl;
    const bool shift = InputEvent.IsShiftDown() || Mods.IsShiftDown() || asyncShift;
    const bool alt = InputEvent.IsAltDown() || Mods.IsAltDown();
    const bool superKey = InputEvent.IsCommandDown() || Mods.IsCommandDown();
    SmatchetHost_SetKeyModifiers(Host, ctrl, shift, alt, superKey);
}
