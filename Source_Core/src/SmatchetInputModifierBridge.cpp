#include "SmatchetInputModifierBridge.h"

#include <atomic>

namespace {
std::atomic<bool> gPluginCtrlPushed{false};
std::atomic<bool> gPluginShiftPushed{false};
} // namespace

void SmatchetInput_SetPluginModifiersPushed(bool ctrl, bool shift) {
    gPluginCtrlPushed.store(ctrl, std::memory_order_relaxed);
    gPluginShiftPushed.store(shift, std::memory_order_relaxed);
}

bool SmatchetInput_GetPluginModifiersPushedCtrl() { return gPluginCtrlPushed.load(std::memory_order_relaxed); }

bool SmatchetInput_GetPluginModifiersPushedShift() { return gPluginShiftPushed.load(std::memory_order_relaxed); }






