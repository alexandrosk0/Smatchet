#include "SmatchetImGuiCommandBridge.h"

#include "Containers/Set.h"
#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <cstdarg>
#include <cstdio>
#include <string>

DEFINE_LOG_CATEGORY_STATIC(LogSmatchetImGuiConsole, Log, All);

#define LOG_INFO(fmt, ...) SmatchetConsoleLogf(ELogVerbosity::Log, (fmt), ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) SmatchetConsoleLogf(ELogVerbosity::Warning, (fmt), ##__VA_ARGS__)

namespace {

enum class ESmatchetConsoleRequestKind { Command, Discovery };

struct FPendingSmatchetConsoleRequest {
    int64 RequestId = 0;
    FString CommandName;
    ESmatchetConsoleRequestKind Kind = ESmatchetConsoleRequestKind::Command;
};

struct FCompletedSmatchetConsoleRequest {
    FString CommandName;
    FString ResultJson;
    ESmatchetConsoleRequestKind Kind = ESmatchetConsoleRequestKind::Command;
};

struct FParsedSmatchetConsoleArgs {
    FString ArgsJson;
    bool bConfirmedDestructive = false;
    bool bDryRun = false;
};

const TCHAR* const GSmatchetConsolePrefixes[] = {TEXT("smartchat"), TEXT("smatchet")};

TArray<FPendingSmatchetConsoleRequest> GPendingSmatchetConsoleRequests;
TArray<IConsoleObject*> GRegisteredSmatchetConsoleObjects;
TSet<FString> GRegisteredSmatchetConsoleNames;
FTSTicker::FDelegateHandle GSmatchetConsoleTicker;
FCriticalSection GSmatchetConsoleMutex;
bool GDiscoveryQueued = false;

bool TickSmatchetConsoleRequests(float);
void ExecuteSmatchetConsoleCommand(const FString& CommandName, const TArray<FString>& Args);
bool RequestSmatchetConsoleDiscovery();

void SmatchetConsoleLogf(ELogVerbosity::Type Verbosity, const char* Fmt, ...) {
    char Buffer[4096];
    va_list Args;
    va_start(Args, Fmt);
    std::vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);
    Buffer[sizeof(Buffer) - 1] = '\0';
    FMsg::Logf(__FILE__, __LINE__, LogSmatchetImGuiConsole.GetCategoryName(), Verbosity, TEXT("%s"),
               UTF8_TO_TCHAR(Buffer));
}

std::string ToLogString(const FString& Value) {
    FTCHARToUTF8 Utf8(*Value);
    return std::string(Utf8.Get(), Utf8.Length());
}

FString JoinConsoleJsonArgs(const TArray<FString>& Args, int32 StartIndex, bool& bConfirmedDestructive, bool& bDryRun) {
    TArray<FString> JsonParts;
    for (int32 Index = StartIndex; Index < Args.Num(); ++Index) {
        const FString& Arg = Args[Index];
        if (Arg.Equals(TEXT("--yes"), ESearchCase::IgnoreCase)) {
            bConfirmedDestructive = true;
            continue;
        }
        if (Arg.Equals(TEXT("--dry-run"), ESearchCase::IgnoreCase)) {
            bDryRun = true;
            continue;
        }
        JsonParts.Add(Arg);
    }

    FString ArgsJson = FString::Join(JsonParts, TEXT(" "));
    ArgsJson.TrimStartAndEndInline();
    return ArgsJson.IsEmpty() ? FString(TEXT("{}")) : ArgsJson;
}

FParsedSmatchetConsoleArgs ParseSmatchetConsoleArgs(const TArray<FString>& Args, int32 StartIndex) {
    FParsedSmatchetConsoleArgs Parsed;
    Parsed.ArgsJson = JoinConsoleJsonArgs(Args, StartIndex, Parsed.bConfirmedDestructive, Parsed.bDryRun);
    return Parsed;
}

bool IsSafeConsoleCommandName(const FString& CommandName) {
    if (CommandName.IsEmpty()) {
        return false;
    }
    for (int32 Index = 0; Index < CommandName.Len(); ++Index) {
        const TCHAR Ch = CommandName[Index];
        if (!FChar::IsAlnum(Ch) && Ch != TEXT('.') && Ch != TEXT('_') && Ch != TEXT('-')) {
            return false;
        }
    }
    return true;
}

FString MakeSmatchetConsoleName(const TCHAR* Prefix, const FString& CommandName) {
    return FString::Printf(TEXT("%s.%s"), Prefix, *CommandName);
}

void EnsureSmatchetConsoleTickerLocked() {
    if (!GSmatchetConsoleTicker.IsValid()) {
        GSmatchetConsoleTicker =
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickSmatchetConsoleRequests), 0.0f);
    }
}

void QueueSmatchetConsoleRequest(int64 RequestId, const FString& CommandName, ESmatchetConsoleRequestKind Kind) {
    if (RequestId <= 0) {
        LOG_WARN("Smatchet console command '%s' could not be queued because the native host is unavailable.",
                 ToLogString(CommandName).c_str());
        return;
    }

    FScopeLock Lock(&GSmatchetConsoleMutex);
    FPendingSmatchetConsoleRequest Pending;
    Pending.RequestId = RequestId;
    Pending.CommandName = CommandName;
    Pending.Kind = Kind;
    GPendingSmatchetConsoleRequests.Add(Pending);
    EnsureSmatchetConsoleTickerLocked();
}

int64 EnqueueSmatchetConsoleCommand(const FString& CommandName, const FParsedSmatchetConsoleArgs& Parsed,
                                    ESmatchetConsoleRequestKind Kind) {
    const int64 RequestId = USmatchetImGuiCommandBridge::EnqueueSmatchetCommand(
        CommandName, Parsed.ArgsJson, Parsed.bConfirmedDestructive, Parsed.bDryRun);
    if (RequestId > 0) {
        LOG_INFO("Queued Smatchet command %s as request %lld.", ToLogString(CommandName).c_str(),
                 static_cast<long long>(RequestId));
    }
    QueueSmatchetConsoleRequest(RequestId, CommandName, Kind);
    return RequestId;
}

void ExecuteSmatchetConsoleCommand(const FString& CommandName, const TArray<FString>& Args) {
    const FParsedSmatchetConsoleArgs Parsed = ParseSmatchetConsoleArgs(Args, 0);
    EnqueueSmatchetConsoleCommand(CommandName, Parsed, ESmatchetConsoleRequestKind::Command);
}

void ExecuteSmatchetGenericConsoleCommand(const TArray<FString>& Args) {
    if (Args.Num() == 0) {
        LOG_INFO("Usage: smartchat <command.name> [--yes] [--dry-run] [json-object]. Example: smartchat "
                 "commands.list {\"full\":true}");
        return;
    }

    const FString CommandName = Args[0];
    if (CommandName.Equals(TEXT("refresh"), ESearchCase::IgnoreCase)) {
        RequestSmatchetConsoleDiscovery();
        return;
    }

    const FParsedSmatchetConsoleArgs Parsed = ParseSmatchetConsoleArgs(Args, 1);
    EnqueueSmatchetConsoleCommand(CommandName, Parsed, ESmatchetConsoleRequestKind::Command);
}

IConsoleObject* RegisterSmatchetConsoleObject(const FString& ConsoleName, const FString& Help,
                                              const FConsoleCommandWithArgsDelegate& Delegate) {
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    if (GRegisteredSmatchetConsoleNames.Contains(ConsoleName)) {
        return nullptr;
    }
    if (ConsoleManager.FindConsoleObject(*ConsoleName) != nullptr) {
        LOG_WARN("Skipping Smatchet console command '%s' because another console object already owns that name.",
                 ToLogString(ConsoleName).c_str());
        return nullptr;
    }

    IConsoleObject* Registered = ConsoleManager.RegisterConsoleCommand(*ConsoleName, *Help, Delegate, ECVF_Default);
    if (Registered) {
        GRegisteredSmatchetConsoleObjects.Add(Registered);
        GRegisteredSmatchetConsoleNames.Add(ConsoleName);
    }
    return Registered;
}

void RegisterSmatchetNativeConsoleCommand(const FString& CommandName) {
    if (!IsSafeConsoleCommandName(CommandName)) {
        LOG_WARN("Skipping unsafe Smatchet console command name '%s'.", ToLogString(CommandName).c_str());
        return;
    }

    for (const TCHAR* Prefix : GSmatchetConsolePrefixes) {
        const FString ConsoleName = MakeSmatchetConsoleName(Prefix, CommandName);
        const FString Help =
            FString::Printf(TEXT("Runs Smatchet command '%s'. Args: [--yes] [--dry-run] [json-object]."), *CommandName);
        RegisterSmatchetConsoleObject(
            ConsoleName, Help,
            FConsoleCommandWithArgsDelegate::CreateLambda(
                [CommandName](const TArray<FString>& Args) { ExecuteSmatchetConsoleCommand(CommandName, Args); }));
    }
}

void RegisterSmatchetGenericConsoleCommand(const TCHAR* Prefix) {
    const FString ConsoleName(Prefix);
    const FString Help = FString::Printf(
        TEXT("Runs a Smatchet command. Usage: %s <command.name> [--yes] [--dry-run] [json-object]."), Prefix);
    RegisterSmatchetConsoleObject(ConsoleName, Help,
                                  FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteSmatchetGenericConsoleCommand));
}

void RegisterSmatchetRefreshConsoleCommand(const TCHAR* Prefix) {
    const FString ConsoleName = FString::Printf(TEXT("%s.refresh"), Prefix);
    RegisterSmatchetConsoleObject(ConsoleName, TEXT("Refreshes Smatchet console aliases from commands.list."),
                                  FConsoleCommandWithArgsDelegate::CreateLambda(
                                      [](const TArray<FString>&) { RequestSmatchetConsoleDiscovery(); }));
}

void RegisterSmatchetBootstrapConsoleCommands() {
    for (const TCHAR* Prefix : GSmatchetConsolePrefixes) {
        RegisterSmatchetGenericConsoleCommand(Prefix);
        RegisterSmatchetRefreshConsoleCommand(Prefix);
    }

    RegisterSmatchetNativeConsoleCommand(TEXT("commands.list"));
    RegisterSmatchetNativeConsoleCommand(TEXT("commands.help"));
    RegisterSmatchetNativeConsoleCommand(TEXT("commands.search"));
    RegisterSmatchetNativeConsoleCommand(TEXT("commands.recents"));
}

void RegisterSmatchetDiscoveredConsoleCommands(const FString& ResultJson) {
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) {
        LOG_WARN("Could not parse commands.list result: %s", ToLogString(ResultJson).c_str());
        return;
    }

    bool bOk = false;
    if (!Root->TryGetBoolField(TEXT("ok"), bOk) || !bOk) {
        LOG_WARN("commands.list failed while refreshing console aliases: %s", ToLogString(ResultJson).c_str());
        return;
    }

    const TSharedPtr<FJsonObject>* DataObject = nullptr;
    if (!Root->TryGetObjectField(TEXT("data"), DataObject) || !DataObject || !DataObject->IsValid()) {
        LOG_WARN("commands.list result did not include a data object: %s", ToLogString(ResultJson).c_str());
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!(*DataObject)->TryGetArrayField(TEXT("items"), Items) || !Items) {
        LOG_WARN("commands.list result did not include data.items: %s", ToLogString(ResultJson).c_str());
        return;
    }

    int32 RegisteredCount = 0;
    for (const TSharedPtr<FJsonValue>& ItemValue : *Items) {
        if (!ItemValue.IsValid()) {
            continue;
        }
        const TSharedPtr<FJsonObject> ItemObject = ItemValue->AsObject();
        if (!ItemObject.IsValid()) {
            continue;
        }
        FString CommandName;
        if (!ItemObject->TryGetStringField(TEXT("name"), CommandName)) {
            continue;
        }
        const int32 BeforeCount = GRegisteredSmatchetConsoleNames.Num();
        RegisterSmatchetNativeConsoleCommand(CommandName);
        RegisteredCount += GRegisteredSmatchetConsoleNames.Num() - BeforeCount;
    }

    LOG_INFO("Smatchet console aliases refreshed from commands.list (%d aliases).", RegisteredCount);
}

bool RequestSmatchetConsoleDiscovery() {
    FParsedSmatchetConsoleArgs Parsed;
    Parsed.ArgsJson = TEXT("{\"full\":true,\"limit\":500}");
    const int64 RequestId =
        EnqueueSmatchetConsoleCommand(TEXT("commands.list"), Parsed, ESmatchetConsoleRequestKind::Discovery);
    GDiscoveryQueued = RequestId > 0;
    return GDiscoveryQueued;
}

bool TickSmatchetConsoleRequests(float) {
    TArray<FCompletedSmatchetConsoleRequest> CompletedRequests;
    bool bKeepTicking = true;

    {
        FScopeLock Lock(&GSmatchetConsoleMutex);
        for (int32 Index = GPendingSmatchetConsoleRequests.Num() - 1; Index >= 0; --Index) {
            const FPendingSmatchetConsoleRequest& Pending = GPendingSmatchetConsoleRequests[Index];
            if (!USmatchetImGuiCommandBridge::IsSmatchetCommandResultReady(Pending.RequestId)) {
                continue;
            }

            FString ResultJson;
            if (!USmatchetImGuiCommandBridge::TakeSmatchetCommandResultJson(Pending.RequestId, ResultJson)) {
                ResultJson = TEXT("{\"ok\":false,\"error\":{\"code\":\"result-unavailable\",\"message\":\"Command "
                                  "result was not available to consume.\"}}");
            }

            FCompletedSmatchetConsoleRequest Completed;
            Completed.CommandName = Pending.CommandName;
            Completed.ResultJson = ResultJson;
            Completed.Kind = Pending.Kind;
            CompletedRequests.Add(Completed);
            GPendingSmatchetConsoleRequests.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        }

        if (GPendingSmatchetConsoleRequests.Num() == 0) {
            GSmatchetConsoleTicker = FTSTicker::FDelegateHandle();
            bKeepTicking = false;
        }
    }

    for (const FCompletedSmatchetConsoleRequest& Completed : CompletedRequests) {
        if (Completed.Kind == ESmatchetConsoleRequestKind::Discovery) {
            RegisterSmatchetDiscoveredConsoleCommands(Completed.ResultJson);
        } else {
            LOG_INFO("Smatchet command %s result: %s", ToLogString(Completed.CommandName).c_str(),
                     ToLogString(Completed.ResultJson).c_str());
        }
    }

    return bKeepTicking;
}

} // namespace

void SmatchetImGuiConsoleCommands_StartupModule() {
    RegisterSmatchetBootstrapConsoleCommands();
    if (!GDiscoveryQueued) {
        RequestSmatchetConsoleDiscovery();
    }
}

void SmatchetImGuiConsoleCommands_ShutdownModule() {
    FScopeLock Lock(&GSmatchetConsoleMutex);
    if (GSmatchetConsoleTicker.IsValid()) {
        FTSTicker::GetCoreTicker().RemoveTicker(GSmatchetConsoleTicker);
        GSmatchetConsoleTicker = FTSTicker::FDelegateHandle();
    }
    GPendingSmatchetConsoleRequests.Reset();

    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    for (IConsoleObject* ConsoleObject : GRegisteredSmatchetConsoleObjects) {
        if (ConsoleObject) {
            ConsoleManager.UnregisterConsoleObject(ConsoleObject);
        }
    }
    GRegisteredSmatchetConsoleObjects.Reset();
    GRegisteredSmatchetConsoleNames.Reset();
    GDiscoveryQueued = false;
}
