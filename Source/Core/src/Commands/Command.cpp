#include "Commands/Command.h"

#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace cmd {

const char* ErrorCodeString(ErrorCode c) {
    switch (c) {
    case ErrorCode::None:
        return "ok";
    case ErrorCode::UnknownCommand:
        return "unknown-command";
    case ErrorCode::MissingRequiredArg:
        return "missing-required-arg";
    case ErrorCode::ValidationError:
        return "validation-error";
    case ErrorCode::HandlerError:
        return "handler-error";
    case ErrorCode::ConfirmRequired:
        return "confirm-required";
    case ErrorCode::NotConnected:
        return "not-connected";
    case ErrorCode::AppendOnly:
        return "append-only";
    case ErrorCode::NotFound:
        return "not-found";
    case ErrorCode::BackendError:
        return "backend-error";
    case ErrorCode::DryRunUnsupported:
        return "dry-run-unsupported";
    case ErrorCode::Timeout:
        return "timeout";
    }
    return "unknown";
}

nlohmann::json CommandError::ToJson() const {
    nlohmann::json j;
    j["code"] = ErrorCodeString(Code);
    j["message"] = Message;
    if (!Hint.empty()) {
        j["hint"] = Hint;
    }
    if (!Suggestions.empty()) {
        j["suggestions"] = Suggestions;
    }
    if (Details && !Details->is_null()) {
        j["details"] = *Details;
    }
    return j;
}

CommandResult CommandResult::Success(nlohmann::json data) {
    CommandResult r;
    r.Ok = true;
    r.Data = std::make_shared<nlohmann::json>(std::move(data));
    return r;
}

CommandResult CommandResult::Failure(ErrorCode code, std::string message, std::string hint,
                                     std::vector<std::string> suggestions) {
    CommandResult r;
    r.Ok = false;
    r.Error.Code = code;
    r.Error.Message = std::move(message);
    r.Error.Hint = std::move(hint);
    r.Error.Suggestions = std::move(suggestions);
    return r;
}

nlohmann::json CommandResult::ToWireJson(const std::string& commandName, bool dryRun) const {
    nlohmann::json j;
    j["ok"] = Ok;
    j["command"] = commandName;
    if (dryRun) {
        j["dryRun"] = true;
    }
    if (Ok) {
        j["data"] = (Data && !Data->is_null()) ? *Data : nlohmann::json::object();
    } else {
        j["error"] = Error.ToJson();
    }
    return j;
}

static const char* ParamTypeJsonName(ParamType t) {
    switch (t) {
    case ParamType::String:
        return "string";
    case ParamType::Int:
        return "integer";
    case ParamType::Bool:
        return "boolean";
    case ParamType::Number:
        return "number";
    case ParamType::Json:
        return "object";
    }
    return "string";
}

nlohmann::json Command::BuildJsonSchema() const {
    nlohmann::json props = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    for (const ParamSpec& p : Params) {
        nlohmann::json one;
        one["type"] = ParamTypeJsonName(p.Type);
        if (!p.Description.empty()) {
            one["description"] = p.Description;
        }
        if (p.Default && !p.Default->is_null()) {
            one["default"] = *p.Default;
        }
        if (!p.Enum.empty()) {
            one["enum"] = p.Enum;
        }
        props[p.Name] = std::move(one);
        if (p.Required) {
            required.push_back(p.Name);
        }
    }
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

static const char* ParamTypeHelpName(ParamType t) {
    switch (t) {
    case ParamType::String:
        return "string";
    case ParamType::Int:
        return "int";
    case ParamType::Bool:
        return "bool";
    case ParamType::Number:
        return "number";
    case ParamType::Json:
        return "json";
    }
    return "string";
}

std::string Command::BuildHelpText() const {
    std::ostringstream os;
    os << Name << " \xE2\x80\x94 " << Summary << "\n\n"; // em-dash (UTF-8)
    if (!Description.empty()) {
        os << Description << "\n\n";
    }
    os << "Idempotent: " << (Idempotent ? "yes" : "no") << "\n";
    os << "Destructive: " << (Destructive ? "yes" : "no") << "\n";
    os << "Dry-run:    " << (DryRunSupported ? "yes" : "no") << "\n";
    os << "Async-safe: " << (AsyncSafe ? "yes" : "no") << "\n";
    if (!Aliases.empty()) {
        os << "Aliases:   ";
        for (size_t i = 0; i < Aliases.size(); ++i) {
            if (i)
                os << ", ";
            os << Aliases[i];
        }
        os << "\n";
    }
    os << "\n";

    bool anyRequired = false;
    for (const ParamSpec& p : Params) {
        if (p.Required) {
            anyRequired = true;
            break;
        }
    }
    if (anyRequired) {
        os << "Required:\n";
        for (const ParamSpec& p : Params) {
            if (!p.Required)
                continue;
            os << "  --" << p.Name << "=<" << ParamTypeHelpName(p.Type) << ">";
            if (!p.Description.empty())
                os << "  " << p.Description;
            os << "\n";
        }
    }
    bool anyOptional = false;
    for (const ParamSpec& p : Params) {
        if (!p.Required) {
            anyOptional = true;
            break;
        }
    }
    if (anyOptional) {
        os << "Optional:\n";
        for (const ParamSpec& p : Params) {
            if (p.Required)
                continue;
            os << "  --" << p.Name << "=<" << ParamTypeHelpName(p.Type) << ">";
            if (p.Default && !p.Default->is_null())
                os << " (default: " << p.Default->dump() << ")";
            if (!p.Description.empty())
                os << "  " << p.Description;
            os << "\n";
        }
    }

    os << "\nExample:\n";
    os << "  Smatchet.exe cmd " << Name;
    for (const ParamSpec& p : Params) {
        if (p.Required) {
            os << " --" << p.Name << "=<" << ParamTypeHelpName(p.Type) << ">";
        }
    }
    os << "\n";

    // Agent-friendly tip — humans see plain text here, but agents should fetch the
    // machine-readable JSON schema. This is the only documented stdout-non-JSON path.
    os << "\nFor JSON schema (agent-friendly):\n";
    os << "  Smatchet.exe cmd commands.help --name=" << Name << "\n";
    return os.str();
}

std::string TitleCasedSlugFromCommandId(const std::string& cmdId) {
    static const char kPrefix[] = "view.toggle.";
    const std::string slug =
        (cmdId.compare(0U, sizeof(kPrefix) - 1U, kPrefix) == 0) ? cmdId.substr(sizeof(kPrefix) - 1U) : cmdId;
    std::string out;
    out.reserve(slug.size());
    bool wordStart = true;
    for (std::size_t i = 0; i < slug.size(); ++i) {
        const char sc = slug[i];
        if (sc == '-' || sc == '_' || sc == '.') {
            out.push_back(' ');
            wordStart = true;
            continue;
        }
        out.push_back(wordStart ? static_cast<char>(std::toupper(static_cast<unsigned char>(sc))) : sc);
        wordStart = false;
    }
    return out;
}
} // namespace cmd
} // namespace smatchet
