#ifndef SMATCHET_INTERFACES_IAPP_COMMANDS_H
#define SMATCHET_INTERFACES_IAPP_COMMANDS_H

// Narrow facet exposing the command registry the app owns (AppController fan-in Phase 6).
// AppController implements it; TUs that only register/inspect commands depend on this
// instead of the full AppController.h. By-ref returns of a forward-declared registry
// need no completeness here; consumers include Commands/CommandRegistry.h themselves.

namespace smatchet {
namespace cmd {
class CommandRegistry;
}
} // namespace smatchet

class IAppCommands {
  public:
    virtual ~IAppCommands() = default;

    virtual smatchet::cmd::CommandRegistry& Commands() = 0;
    virtual const smatchet::cmd::CommandRegistry& Commands() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_COMMANDS_H
