#pragma once

// Last modifier flags pushed from the plugin input thread (SmatchetImGuiHost::SetKeyModifiers).
// SmatchetCore UI reads these to avoid races where ImGui io.Key* / GetAsyncKeyState disagree briefly.
void SmatchetInput_SetPluginModifiersPushed(bool ctrl, bool shift);
bool SmatchetInput_GetPluginModifiersPushedCtrl();
bool SmatchetInput_GetPluginModifiersPushedShift();






