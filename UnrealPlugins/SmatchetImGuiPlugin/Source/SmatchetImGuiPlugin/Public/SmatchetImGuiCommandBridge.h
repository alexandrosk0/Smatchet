#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "SmatchetImGuiCommandBridge.generated.h"

DECLARE_DYNAMIC_DELEGATE_OneParam(FSmatchetCommandResultDelegate, const FString&, ResultJson);

UCLASS()
class SMATCHETIMGUIPLUGIN_API USmatchetImGuiCommandBridge : public UBlueprintFunctionLibrary {
    GENERATED_BODY()

  public:
    UFUNCTION(BlueprintCallable, Category = "Smatchet|Commands")
    static int64 EnqueueSmatchetCommand(const FString& CommandName, const FString& ArgsJson, bool bConfirmedDestructive,
                                        bool bDryRun);

    UFUNCTION(BlueprintCallable, Category = "Smatchet|Commands", meta = (AutoCreateRefTerm = "OnComplete"))
    static int64 EnqueueSmatchetCommandWithCallback(const FString& CommandName, const FString& ArgsJson,
                                                    bool bConfirmedDestructive, bool bDryRun,
                                                    const FSmatchetCommandResultDelegate& OnComplete);

    UFUNCTION(BlueprintCallable, Category = "Smatchet|Commands")
    static bool IsSmatchetCommandResultReady(int64 RequestId);

    UFUNCTION(BlueprintCallable, Category = "Smatchet|Commands")
    static bool TakeSmatchetCommandResultJson(int64 RequestId, FString& ResultJson);
};

/// Stops pending async command callbacks (plugin module shutdown).
SMATCHETIMGUIPLUGIN_API void SmatchetImGuiCommandBridge_ShutdownModule();
