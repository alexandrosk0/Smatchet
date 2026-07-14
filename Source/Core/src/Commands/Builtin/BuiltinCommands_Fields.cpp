// fields.* — tracker field catalog: list, get, refresh, icon_for.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "Interfaces/IAppFields.h"
#include <nlohmann/json.hpp>
#include "ConfigManager.h"
#include "Tracker/TrackerFieldSchema.h"

#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PaginateJsonArray;
using builtin_detail::PInt;
using builtin_detail::PString;

void RegisterFieldsCommands(CommandRegistry& reg, IAppFields& app) {
    {
        Command c = MakeCommand("fields.list_available", "List tracker fields from the local catalog.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const int limit = args.value("limit", 50);
                                    const int offset = args.value("offset", 0);
                                    const std::vector<TrackerField>& fields = app.GetAvailableFields();
                                    nlohmann::json items = nlohmann::json::array();
                                    for (const TrackerField& f : fields) {
                                        nlohmann::json one;
                                        one["id"] = f.Id;
                                        one["name"] = f.Name;
                                        one["type"] = f.Type;
                                        items.push_back(std::move(one));
                                    }
                                    return CommandResult::Success(PaginateJsonArray(items, limit, offset));
                                });
        c.Params = {
            PInt("limit", "Max items.", 50),
            PInt("offset", "Pagination offset.", 0),
        };
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("fields.get", "Get metadata for a single field by id.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string id = args.value("id", std::string());
                                    const TrackerField* f = app.FindFieldById(id);
                                    if (!f) {
                                        return CommandResult::Failure(ErrorCode::NotFound,
                                                                      "Field '" + id + "' not found in catalog.",
                                                                      "Run fields.refresh_catalog to update.");
                                    }
                                    nlohmann::json out;
                                    out["id"] = f->Id;
                                    out["name"] = f->Name;
                                    out["type"] = f->Type;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {PString("id", "Field id.", true)};
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("fields.refresh_catalog", "Re-fetch the tracker field catalog from the backend.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    const TrackerConfig cfg = ConfigManager::Load();
                                    const bool ok = app.RefreshFieldCatalog(cfg);
                                    nlohmann::json out;
                                    out["ok"] = ok;
                                    out["fieldCount"] = static_cast<int>(app.GetAvailableFields().size());
                                    if (!ok)
                                        out["error"] = app.GetFieldCatalogError();
                                    return CommandResult::Success(std::move(out));
                                });
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("fields.icon_for", "Resolve the icon asset path/URL for a field+value pair.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string fieldId = args.value("field", std::string());
                                    const std::string rawValue = args.value("value", std::string());
                                    const TrackerField* f = app.FindFieldById(fieldId);
                                    std::string outPathOrUrl;
                                    const bool found = app.TryGetFieldIconMapTarget(fieldId, f, rawValue, outPathOrUrl);
                                    nlohmann::json out;
                                    out["found"] = found;
                                    out["pathOrUrl"] = found ? outPathOrUrl : std::string();
                                    out["field"] = fieldId;
                                    out["value"] = rawValue;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {
            PString("field", "Field id (e.g. 'priority').", true),
            PString("value", "Raw field value.", true),
        };
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
