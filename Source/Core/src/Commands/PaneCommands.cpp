// pane.* command group — registered from SmatchetUI once grid panes are loaded.
// Multi-grid-tabs Slice 4 (plan item 20). See Commands/PaneCommands.h for the
// UI-thread + request-latch contract; mirrors ViewCommands.cpp's registration shape.

#include "Commands/PaneCommands.h"

// fan-in Phase 6 T3: pane.* needs only the command registry (IAppCommands) and the
// UI-thread hop (IMainThreadPoster) — not the full AppController.
#include "Interfaces/IAppCommands.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "Config/ConfigManager.h"
#include "GridPane.h"
#include "SmatchetGridPaneWindows.h"
#include "SmatchetUiSession.h"

#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

nlohmann::json PaneToJson(const GridPane& p) {
    nlohmann::json j;
    j["id"] = p.id;
    j["title"] = p.title;
    j["backendKey"] = p.backendKey;
    j["viewId"] = p.viewId;
    j["focused"] = p.focused;
    return j;
}

/// Resolve the pane an `[id]`-or-focused command targets. Returns nullptr (and sets a
/// NotFound failure) when the explicit id is unknown; falls back to the focused pane
/// when no id was supplied. Empty pane set → nullptr with a HandlerError.
GridPane* ResolveNamedOrFocusedPane(UiDrawSession& d, const nlohmann::json& args, CommandResult& outFailure) {
    if (d.gridPanes.empty()) {
        outFailure = CommandResult::Failure(ErrorCode::HandlerError, "No grid panes are loaded yet.");
        return nullptr;
    }
    const std::string id = args.value("id", std::string());
    if (!id.empty()) {
        GridPane* byId = FindGridPaneById(d.gridPanes, id);
        if (byId == nullptr) {
            outFailure = CommandResult::Failure(ErrorCode::NotFound, "Pane '" + id + "' not found.",
                                                "Call pane.list to see valid pane ids.");
        }
        return byId;
    }
    return &d.focusedPane();
}

void RegisterPaneListCommand(IMainThreadPoster& poster, UiDrawSession& d, CommandRegistry& reg) {
    Command c;
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=the Command-struct registration boilerplate (Name/Category/Summary + poster-hop handler shell) is grandfathered across the pane.*/view.* command TU siblings; retyping the capture to the narrow poster (fan-in Phase 6 T3) re-hashed the clone window vs ViewCommands.cpp — not a real copy-paste; owner=orchestrator; revisit=when the command-registration shell is factored into a shared builder)
    // clang-format on
    c.Name = "pane.list";
    c.Category = "pane";
    c.Summary = "List all open grid panes (id, title, backend, view, focus).";
    // Reads d.gridPanes — mutated only on the UI thread by the pane-window host; hop
    // to the UI thread so the read never races a pane add/close/focus switch.
    c.Handler = [&poster, &d](const nlohmann::json&, const CommandContext&) {
        return RunOnUiThreadAsCommandResult(poster, [&d]() {
            nlohmann::json arr = nlohmann::json::array();
            for (const GridPane& p : d.gridPanes) {
                arr.push_back(PaneToJson(p));
            }
            nlohmann::json out;
            out["items"] = std::move(arr);
            out["total"] = static_cast<int>(d.gridPanes.size());
            out["focusedId"] = d.focusedPaneId;
            return CommandResult::Success(std::move(out));
        });
    };
    reg.Register(std::move(c));
}

void RegisterPaneFocusCommand(IMainThreadPoster& poster, UiDrawSession& d, CommandRegistry& reg) {
    Command c;
    c.Name = "pane.focus";
    c.Category = "pane";
    c.Summary = "Focus the grid pane with the given id.";
    c.Idempotent = false;
    c.Params = {[] {
        ParamSpec p;
        p.Name = "id";
        p.Type = ParamType::String;
        p.Required = true;
        p.Description = "Pane id from pane.list.";
        return p;
    }()};
    c.Handler = [&poster, &d](const nlohmann::json& args, const CommandContext&) {
        return RunOnUiThreadAsCommandResult(poster, [&d, args]() {
            const std::string id = args.value("id", std::string());
            if (FindGridPaneById(d.gridPanes, id) == nullptr) {
                return CommandResult::Failure(ErrorCode::NotFound, "Pane '" + id + "' not found.",
                                              "Call pane.list to see valid pane ids.");
            }
            // Same latch the window-focus / "+" / close paths use: set focusedPaneId and
            // flag a host-side reassignment so drawGridPaneWindows replays it as a real
            // focus switch (activating the pane's saved view) next frame.
            d.focusedPaneId = id;
            d.gridPaneFocusReassigned = true;
            // CRITICAL: also move ImGui window focus to the target pane. Without this the
            // still-ImGui-focused old pane window reports paneWindowFocusedThisFrame next
            // frame and the host reverts focusedPaneId back to it (windowFocusMoved). The
            // focused pane honors this via wantFocus = pane.focused && requestActiveProjectFocus.
            d.requestActiveProjectFocus = true;
            return CommandResult::Success({{"focusedId", id}});
        });
    };
    reg.Register(std::move(c));
}

/// pane.next / pane.prev share the cycle-then-latch body; `forward` picks the helper.
void RegisterPaneCycleCommand(IMainThreadPoster& poster, UiDrawSession& d, CommandRegistry& reg, bool forward) {
    Command c;
    c.Name = forward ? "pane.next" : "pane.prev";
    c.Category = "pane";
    c.Summary = forward ? "Focus the next grid pane (wraps past the last)."
                        : "Focus the previous grid pane (wraps past the first).";
    c.Idempotent = false;
    c.Handler = [&poster, &d, forward](const nlohmann::json&, const CommandContext&) {
        return RunOnUiThreadAsCommandResult(poster, [&d, forward]() {
            if (d.gridPanes.empty()) {
                return CommandResult::Failure(ErrorCode::HandlerError, "No grid panes are loaded yet.");
            }
            const std::string next = forward ? SmatchetGridPaneWindows::NextPaneId(d.gridPanes, d.focusedPaneId)
                                             : SmatchetGridPaneWindows::PrevPaneId(d.gridPanes, d.focusedPaneId);
            d.focusedPaneId = next;
            d.gridPaneFocusReassigned = true;
            // Move ImGui window focus too — see pane.focus for why (else the host reverts).
            d.requestActiveProjectFocus = true;
            return CommandResult::Success({{"focusedId", next}});
        });
    };
    reg.Register(std::move(c));
}

/// pane.new / pane.duplicate / pane.split all duplicate the focused pane via the exact
/// "+" mechanism (paneAddRequestSourceId latch) — the host applies the add next frame,
/// so the command never mutates d.gridPanes mid-render. `summary`/`description` differ
/// per verb; `acceptDirection` adds the advisory split-direction arg;
/// `acceptBackend` (pane.new only) adds `backend` + `view` params for cross-backend creation.
void RegisterPaneAddCommand(IMainThreadPoster& poster, UiDrawSession& d, CommandRegistry& reg, const std::string& name,
                            const std::string& summary, const std::string& description, bool acceptDirection,
                            bool acceptBackend = false) {
    Command c;
    c.Name = name;
    c.Category = "pane";
    c.Summary = summary;
    c.Description = description;
    c.Idempotent = false;
    if (acceptDirection) {
        c.Params.push_back([] {
            ParamSpec p;
            p.Name = "direction";
            p.Type = ParamType::String;
            p.Description = "Advisory split hint (left/right/up/down) — recorded only; "
                            "native ImGui docking decides tab-vs-split geometry.";
            p.Enum = {"left", "right", "up", "down"};
            return p;
        }());
    }
    if (acceptBackend) {
        const std::vector<std::string>& keys = ConfigManager::KnownBackendKeys();
        c.Params.push_back([&keys] {
            ParamSpec p;
            p.Name = "backend";
            p.Type = ParamType::String;
            p.Description = "Backend to open the new pane on. Absent = duplicate "
                            "the focused pane's backend (unchanged behaviour). "
                            "Rejected if the backend has no credentials configured.";
            p.Enum = keys;
            return p;
        }());
        c.Params.push_back([] {
            ParamSpec p;
            p.Name = "view";
            p.Type = ParamType::String;
            p.Description = "View id to open in the new pane. Absent = the backend's "
                            "active/default view.";
            return p;
        }());
    }
    c.Handler = [&poster, &d, acceptDirection, acceptBackend](const nlohmann::json& args, const CommandContext&) {
        return RunOnUiThreadAsCommandResult(poster, [&d, args, acceptDirection, acceptBackend]() {
            if (d.gridPanes.empty()) {
                return CommandResult::Failure(ErrorCode::HandlerError, "No grid panes are loaded yet.");
            }
            // Validate-first: resolve the whole request into a local and arm d.paneAddRequest
            // ONLY on the success path. A failure return must not leave the latch armed, or the
            // host's ApplyPaneAddAndCloseRequestsCore (keys on !sourceId.empty()) spawns a
            // spurious duplicate pane next frame (issue #1458).
            const detail::PaneAddDecision decision =
                detail::DecidePaneAddRequest(d.focusedPane().id, acceptBackend, args.value("backend", std::string()),
                                             args.value("view", std::string()), d.cfg);
            if (!decision.Ok) {
                return CommandResult::Failure(ErrorCode::HandlerError, decision.FailureMessage);
            }
            d.paneAddRequest = decision.Request;
            nlohmann::json out;
            out["requested"] = true;
            out["sourceId"] = d.paneAddRequest.sourceId;
            if (!d.paneAddRequest.targetBackendKey.empty()) {
                out["targetBackend"] = d.paneAddRequest.targetBackendKey;
                out["targetView"] = d.paneAddRequest.targetViewId;
            }
            if (acceptDirection) {
                out["direction"] = args.value("direction", std::string());
            }
            return CommandResult::Success(std::move(out));
        });
    };
    reg.Register(std::move(c));
}

void RegisterPaneCloseCommand(IMainThreadPoster& poster, UiDrawSession& d, CommandRegistry& reg) {
    Command c;
    c.Name = "pane.close";
    c.Category = "pane";
    c.Summary = "Close a grid pane (focused pane when no id given).";
    c.Description = "Marks the named (or focused) pane closed; the host applies the close "
                    "next frame and enforces the min-1-pane invariant + focus fallback. "
                    "Refuses to close the only remaining pane.";
    c.Idempotent = false;
    c.Params = {[] {
        ParamSpec p;
        p.Name = "id";
        p.Type = ParamType::String;
        p.Description = "Pane id to close (default: the focused pane).";
        return p;
    }()};
    c.Handler = [&poster, &d](const nlohmann::json& args, const CommandContext&) {
        return RunOnUiThreadAsCommandResult(poster, [&d, args]() {
            CommandResult failure;
            GridPane* target = ResolveNamedOrFocusedPane(d, args, failure);
            if (target == nullptr) {
                return failure;
            }
            if (d.gridPanes.size() <= 1) {
                return CommandResult::Failure(ErrorCode::HandlerError, "Cannot close the only remaining grid pane.");
            }
            // close-X latch: the host's ApplyPaneAddAndCloseRequests consumes pane.open.
            target->open = false;
            return CommandResult::Success({{"closing", target->id}});
        });
    };
    reg.Register(std::move(c));
}

void RegisterPaneRenameCommand(IMainThreadPoster& poster, UiDrawSession& d, CommandRegistry& reg) {
    Command c;
    c.Name = "pane.rename";
    c.Category = "pane";
    c.Summary = "Rename a grid pane's tab label (focused pane when no id given).";
    c.Description = "Pins a custom tab title on the named (or focused) pane. The title "
                    "otherwise tracks the active view name every frame; this command sets a "
                    "session-scoped override so the label stays put across view switches. "
                    "NOTE: the override is not persisted — on the next launch the title "
                    "reverts to the active view's name on the first focus sync.";
    c.Idempotent = false;
    c.Params = {
        [] {
            ParamSpec p;
            p.Name = "title";
            p.Type = ParamType::String;
            p.Required = true;
            p.Description = "New tab label.";
            return p;
        }(),
        [] {
            ParamSpec p;
            p.Name = "id";
            p.Type = ParamType::String;
            p.Description = "Pane id to rename (default: the focused pane).";
            return p;
        }(),
    };
    c.Handler = [&poster, &d](const nlohmann::json& args, const CommandContext&) {
        return RunOnUiThreadAsCommandResult(poster, [&d, args]() {
            const std::string title = args.value("title", std::string());
            if (title.empty()) {
                return CommandResult::Failure(ErrorCode::ValidationError, "Pane title must not be empty.");
            }
            CommandResult failure;
            GridPane* target = ResolveNamedOrFocusedPane(d, args, failure);
            if (target == nullptr) {
                return failure;
            }
            target->title = title;
            target->titleOverridden = true; // stop the steady-state view-name lockstep.
            SmatchetGridPaneWindows::MarkPanesDirty(d);
            return CommandResult::Success({{"id", target->id}, {"title", target->title}});
        });
    };
    reg.Register(std::move(c));
}

} // namespace

void RegisterPaneCommands(IAppCommands& app, IMainThreadPoster& poster, UiDrawSession& session) {
    CommandRegistry& reg = app.Commands();
    // Idempotent guard — don't re-register on second call (mirrors RegisterViewCommands).
    if (reg.HasExact("pane.list"))
        return;

    RegisterPaneListCommand(poster, session, reg);
    RegisterPaneFocusCommand(poster, session, reg);
    RegisterPaneCycleCommand(poster, session, reg, /*forward=*/true);
    RegisterPaneCycleCommand(poster, session, reg, /*forward=*/false);
    RegisterPaneAddCommand(poster, session, reg, "pane.new", "Open a new grid pane (optionally on a chosen backend).",
                           "Without backend param: duplicates the focused pane (same backend + view) via "
                           "the pane window \"+\" mechanism. With backend param: opens a new pane on the "
                           "named backend using its active/default view (or the view id if given). "
                           "The backend must have credentials configured. The host opens the pane next "
                           "frame and focuses it.",
                           /*acceptDirection=*/false, /*acceptBackend=*/true);
    RegisterPaneAddCommand(poster, session, reg, "pane.duplicate", "Duplicate the focused grid pane.",
                           "Alias of pane.new: opens a copy of the focused pane (same backend + "
                           "view) and focuses it. The new pane id is assigned by the host.",
                           /*acceptDirection=*/false);
    RegisterPaneAddCommand(poster, session, reg, "pane.split",
                           "Open a new pane; drag its tab to a window edge to split.",
                           "Opens a new pane duplicating the focused one (same path as pane.new). "
                           "The command cannot force dock geometry without DockBuilder, so native "
                           "ImGui docking decides tab-vs-split: drag the new pane's tab to an edge "
                           "to make it a side-by-side split. An optional 'direction' arg is recorded "
                           "but advisory only.",
                           /*acceptDirection=*/true);
    RegisterPaneCloseCommand(poster, session, reg);
    RegisterPaneRenameCommand(poster, session, reg);
}

} // namespace cmd
} // namespace smatchet
