#include "SmatchetImGuiCommandBridge.h"

#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"

#include "SmatchetImGuiHostC.h"

extern SmatchetImGuiHostHandle SmatchetImGuiPlugin_GetNativeHostForCommands();

namespace {

struct FPendingSmatchetCommandCallback {
    uint64 RequestId = 0;
    FSmatchetCommandResultDelegate OnComplete;
};

struct FCompletedSmatchetCommandCallback {
    FSmatchetCommandResultDelegate OnComplete;
    FString ResultJson;
};

TArray<FPendingSmatchetCommandCallback> GPendingSmatchetCommandCallbacks;
FTSTicker::FDelegateHandle GSmatchetCommandCallbackTicker;
FCriticalSection GSmatchetCommandCallbackMutex;

FString MakeHostUnavailableResult() {
    return TEXT("{\"ok\":false,\"command\":\"\",\"error\":{\"code\":\"not-connected\",\"message\":\"Smatchet native "
                "host is unavailable.\"}}");
}

bool TakeNativeCommandResult(SmatchetImGuiHostHandle Host, uint64 RequestId, FString& ResultJson) {
    ResultJson.Reset();
    if (!Host || RequestId == 0) {
        return false;
    }
    char* ResultUtf8 = SmatchetHost_TakeCommandResultJson(Host, static_cast<SmatchetCommandRequestId>(RequestId));
    if (!ResultUtf8) {
        return false;
    }
    ResultJson = UTF8_TO_TCHAR(ResultUtf8);
    SmatchetHost_ReleaseCommandResultJson(ResultUtf8);
    return true;
}

bool TickSmatchetCommandCallbacks(float) {
    SmatchetImGuiHostHandle Host = SmatchetImGuiPlugin_GetNativeHostForCommands();
    TArray<FCompletedSmatchetCommandCallback> CompletedCallbacks;
    bool bKeepTicking = true;

    {
        FScopeLock Lock(&GSmatchetCommandCallbackMutex);
        if (!Host) {
            const FString ResultJson = MakeHostUnavailableResult();
            for (const FPendingSmatchetCommandCallback& Pending : GPendingSmatchetCommandCallbacks) {
                FCompletedSmatchetCommandCallback Completed;
                Completed.OnComplete = Pending.OnComplete;
                Completed.ResultJson = ResultJson;
                CompletedCallbacks.Add(Completed);
            }
            GPendingSmatchetCommandCallbacks.Reset();
            GSmatchetCommandCallbackTicker = FTSTicker::FDelegateHandle();
            bKeepTicking = false;
        } else {
            for (int32 Index = GPendingSmatchetCommandCallbacks.Num() - 1; Index >= 0; --Index) {
                const uint64 RequestId = GPendingSmatchetCommandCallbacks[Index].RequestId;
                if (SmatchetHost_IsCommandResultReady(Host, static_cast<SmatchetCommandRequestId>(RequestId)) == 0) {
                    continue;
                }

                FString ResultJson;
                if (!TakeNativeCommandResult(Host, RequestId, ResultJson)) {
                    ResultJson = TEXT("{\"ok\":false,\"command\":\"\",\"error\":{\"code\":\"result-unavailable\","
                                      "\"message\":\"Command result was not available to consume.\"}}");
                }
                FCompletedSmatchetCommandCallback Completed;
                Completed.OnComplete = GPendingSmatchetCommandCallbacks[Index].OnComplete;
                Completed.ResultJson = ResultJson;
                CompletedCallbacks.Add(Completed);
                GPendingSmatchetCommandCallbacks.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            }

            if (GPendingSmatchetCommandCallbacks.Num() == 0) {
                GSmatchetCommandCallbackTicker = FTSTicker::FDelegateHandle();
                bKeepTicking = false;
            }
        }
    }

    for (const FCompletedSmatchetCommandCallback& Completed : CompletedCallbacks) {
        Completed.OnComplete.ExecuteIfBound(Completed.ResultJson);
    }

    return bKeepTicking;
}

int64 EnqueueNativeCommand(const FString& CommandName, const FString& ArgsJson, bool bConfirmedDestructive,
                           bool bDryRun) {
    SmatchetImGuiHostHandle Host = SmatchetImGuiPlugin_GetNativeHostForCommands();
    if (!Host) {
        return 0;
    }

    FTCHARToUTF8 CommandNameUtf8(*CommandName);
    FTCHARToUTF8 ArgsJsonUtf8(*ArgsJson);
    const SmatchetCommandRequestId RequestId =
        SmatchetHost_EnqueueCommand(Host, CommandNameUtf8.Get(), ArgsJsonUtf8.Get(), bConfirmedDestructive, bDryRun);
    return static_cast<int64>(RequestId);
}

} // namespace

int64 USmatchetImGuiCommandBridge::EnqueueSmatchetCommand(const FString& CommandName, const FString& ArgsJson,
                                                          bool bConfirmedDestructive, bool bDryRun) {
    return EnqueueNativeCommand(CommandName, ArgsJson, bConfirmedDestructive, bDryRun);
}

int64 USmatchetImGuiCommandBridge::EnqueueSmatchetCommandWithCallback(
    const FString& CommandName, const FString& ArgsJson, bool bConfirmedDestructive, bool bDryRun,
    const FSmatchetCommandResultDelegate& OnComplete) {
    const int64 RequestId = EnqueueNativeCommand(CommandName, ArgsJson, bConfirmedDestructive, bDryRun);
    if (RequestId <= 0) {
        OnComplete.ExecuteIfBound(MakeHostUnavailableResult());
        return 0;
    }

    FPendingSmatchetCommandCallback Pending;
    Pending.RequestId = static_cast<uint64>(RequestId);
    Pending.OnComplete = OnComplete;
    {
        FScopeLock Lock(&GSmatchetCommandCallbackMutex);
        GPendingSmatchetCommandCallbacks.Add(Pending);
        if (!GSmatchetCommandCallbackTicker.IsValid()) {
            GSmatchetCommandCallbackTicker = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateStatic(&TickSmatchetCommandCallbacks), 0.0f);
        }
    }
    return RequestId;
}

bool USmatchetImGuiCommandBridge::IsSmatchetCommandResultReady(int64 RequestId) {
    if (RequestId <= 0) {
        return false;
    }
    SmatchetImGuiHostHandle Host = SmatchetImGuiPlugin_GetNativeHostForCommands();
    return Host && SmatchetHost_IsCommandResultReady(Host, static_cast<SmatchetCommandRequestId>(RequestId)) != 0;
}

bool USmatchetImGuiCommandBridge::TakeSmatchetCommandResultJson(int64 RequestId, FString& ResultJson) {
    if (RequestId <= 0) {
        ResultJson.Reset();
        return false;
    }
    return TakeNativeCommandResult(SmatchetImGuiPlugin_GetNativeHostForCommands(), static_cast<uint64>(RequestId),
                                   ResultJson);
}

void SmatchetImGuiCommandBridge_ShutdownModule() {
    FScopeLock Lock(&GSmatchetCommandCallbackMutex);
    if (GSmatchetCommandCallbackTicker.IsValid()) {
        FTSTicker::GetCoreTicker().RemoveTicker(GSmatchetCommandCallbackTicker);
        GSmatchetCommandCallbackTicker = FTSTicker::FDelegateHandle();
    }
    GPendingSmatchetCommandCallbacks.Reset();
}
