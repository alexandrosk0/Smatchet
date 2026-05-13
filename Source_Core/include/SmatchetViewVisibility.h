#pragma once

struct TrackerConfig;

/// Slot tag for the three dockspace-node panels controlled by the View > Appearance toggles.
enum class ViewSlot {
    PrimarySideBar,
    SecondarySideBar,
    BottomPanel,
};

/// Sets the given panelFlag in cfg to |visible| and marks the corresponding dockspace node so it
/// collapses / re-expands on the next frame. Must be called from the UI thread inside Draw().
void SetViewVisible(TrackerConfig& cfg, bool& panelFlag, ViewSlot slot, bool visible);
