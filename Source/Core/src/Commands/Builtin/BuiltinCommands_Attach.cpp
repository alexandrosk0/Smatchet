// attach.* — open / preview-download a ticket attachment.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: depend on the narrow IAppAttachments facet, not the full AppController.h.
#include "Interfaces/IAppAttachments.h"
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.

#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

void RegisterAttachCommands(CommandRegistry& reg, IAppAttachments& app) {
    {
        Command c = MakeCommand("attach.open", "Open a ticket attachment (downloads then launches viewer).",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string url = args.value("url", std::string());
                                    const std::string filename = args.value("filename", std::string());
                                    const std::string mime = args.value("mimeType", std::string());
                                    app.OpenAttachment(url, filename, mime);
                                    return CommandResult::Success({{"opened", true}});
                                });
        c.Destructive = true;
        c.Params = {
            PString("url", "Attachment URL.", true),
            PString("filename", "Filename for download.", true),
            PString("mimeType", "MIME type (e.g. 'image/png')."),
        };
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("attach.download_preview", "Download an attachment to a local temp file for preview.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            const std::string url = args.value("url", std::string());
                            const std::string filename = args.value("filename", std::string());
                            const std::string mime = args.value("mimeType", std::string());
                            std::string err;
                            const bool ok = app.DownloadAttachmentForPreview(url, filename, mime, &err);
                            if (!ok) {
                                return CommandResult::Failure(ErrorCode::HandlerError, "Download failed: " + err);
                            }
                            return CommandResult::Success({{"ok", true}});
                        });
        c.Idempotent = false;
        c.Params = {
            PString("url", "Attachment URL.", true),
            PString("filename", "Local filename.", true),
            PString("mimeType", "MIME type."),
        };
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
