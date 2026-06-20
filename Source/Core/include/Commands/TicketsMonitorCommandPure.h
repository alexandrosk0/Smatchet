#pragma once
// Pure decision helper for the `tickets.monitor on|off|status` command
// (ticket-change-monitor plan, S1c — Files-to-modify #18). Extracted as a header-pure free
// function (ParamBoundsPure.h precedent) so the subcommand → action mapping is unit-testable
// without the CommandRegistry / ConfigManager filesystem surface, which the pure ctest rig
// does not exercise. The thin command handler in BuiltinCommands_Config.cpp parses the
// `action` arg, calls Decide..., and only when WriteEnabled persists the resulting flag.

#include <string>

namespace smatchet {
namespace cmd {

/// What `tickets.monitor <action>` resolves to. `Status` (and an empty/omitted action) is a
/// read-only query; `On`/`Off` flip the persisted enabled flag; `Invalid` is an unknown verb.
enum class TicketsMonitorAction { Status, On, Off, Invalid };

/// Outcome of one `tickets.monitor` invocation, decoupled from persistence + JSON shaping.
struct TicketsMonitorDecision {
    TicketsMonitorAction Action = TicketsMonitorAction::Status;
    bool WriteEnabled = false; ///< True when the handler must persist `EnabledValue`.
    bool EnabledValue = false; ///< The flag to persist (meaningful only when WriteEnabled).
    std::string Error;         ///< Non-empty (with usage hint) only when Action == Invalid.
};

/// Map a raw `action` string to a TicketsMonitorAction. Case-sensitive lower-case verbs match
/// the CLI surface; an empty string is treated as `status` (the no-arg default).
inline TicketsMonitorAction ParseTicketsMonitorAction(const std::string& action) {
    if (action.empty() || action == "status") {
        return TicketsMonitorAction::Status;
    }
    if (action == "on") {
        return TicketsMonitorAction::On;
    }
    if (action == "off") {
        return TicketsMonitorAction::Off;
    }
    return TicketsMonitorAction::Invalid;
}

/// Resolve a `tickets.monitor` invocation: `on`/`off` request a persist of the matching flag,
/// `status` (or no arg) is read-only, anything else is an error carrying a usage hint.
inline TicketsMonitorDecision DecideTicketsMonitor(const std::string& action) {
    TicketsMonitorDecision d;
    d.Action = ParseTicketsMonitorAction(action);
    switch (d.Action) {
    case TicketsMonitorAction::On:
        d.WriteEnabled = true;
        d.EnabledValue = true;
        break;
    case TicketsMonitorAction::Off:
        d.WriteEnabled = true;
        d.EnabledValue = false;
        break;
    case TicketsMonitorAction::Status:
        break;
    case TicketsMonitorAction::Invalid:
        d.Error = "Unknown action '" + action + "'. Usage: tickets.monitor [on|off|status]";
        break;
    }
    return d;
}

} // namespace cmd
} // namespace smatchet
