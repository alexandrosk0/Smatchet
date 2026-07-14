#include "SmatchetHostContextCollector.h"

#include "SmatchetOutputLogTail.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Engine/Selection.h"
#endif

namespace {

// Mirrors the formatter-side cap in Smatchet core (EngineContextFormat.cpp) so we never
// serialize actors the popup would drop anyway.
constexpr int32 kMaxSelectedActors = 25;

// Resolve the world whose map/PIE state describes what the user is looking at:
// the PIE world when playing, else the editor world, else the game world.
UWorld* ResolveContextWorld() {
#if WITH_EDITOR
    if (GEditor) {
        if (GEditor->PlayWorld) {
            return GEditor->PlayWorld;
        }
        return GEditor->GetEditorWorldContext().World();
    }
#endif
    if (GEngine && GEngine->GameViewport) {
        return GEngine->GameViewport->GetWorld();
    }
    return nullptr;
}

} // namespace

FString SmatchetHostContextCollector_BuildJson(const FSmatchetOutputLogTail* LogTail) {
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("source"), TEXT("unreal"));
    Root->SetStringField(TEXT("capturedAtIso"), FDateTime::UtcNow().ToIso8601());
    Root->SetStringField(TEXT("engineVersion"),
                         FString::Printf(TEXT("Unreal Engine %s"), *FEngineVersion::Current().ToString()));
    Root->SetStringField(TEXT("projectName"), FApp::GetProjectName());
    Root->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
    Root->SetStringField(TEXT("platform"), FPlatformProperties::IniPlatformName());
    Root->SetStringField(TEXT("osVersion"), FPlatformMisc::GetOSVersion());
    Root->SetStringField(TEXT("buildConfig"), LexToString(FApp::GetBuildConfiguration()));

    if (UWorld* World = ResolveContextWorld()) {
        Root->SetStringField(TEXT("mapName"), UWorld::RemovePIEPrefix(World->GetMapName()));
    }

#if WITH_EDITOR
    if (GEditor) {
        const TSharedRef<FJsonObject> Pie = MakeShared<FJsonObject>();
        Pie->SetBoolField(TEXT("active"), GEditor->PlayWorld != nullptr);
        Pie->SetBoolField(TEXT("simulating"), GEditor->bIsSimulatingInEditor);
        Root->SetObjectField(TEXT("pie"), Pie);

        if (USelection* Selection = GEditor->GetSelectedActors()) {
            TArray<TSharedPtr<FJsonValue>> Actors;
            for (int32 i = 0; i < Selection->Num() && Actors.Num() < kMaxSelectedActors; ++i) {
                const AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
                if (!Actor) {
                    continue;
                }
                const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
                Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
                Actors.Add(MakeShared<FJsonValueObject>(Entry));
            }
            if (Actors.Num() > 0) {
                Root->SetArrayField(TEXT("selectedActors"), Actors);
            }
        }
    }
#endif

    if (LogTail) {
        const TArray<FString> Lines = LogTail->SnapshotLines();
        if (Lines.Num() > 0) {
            TArray<TSharedPtr<FJsonValue>> JsonLines;
            JsonLines.Reserve(Lines.Num());
            for (const FString& Line : Lines) {
                JsonLines.Add(MakeShared<FJsonValueString>(Line));
            }
            Root->SetArrayField(TEXT("logTail"), JsonLines);
        }
    }

    FString Out;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    FJsonSerializer::Serialize(Root, Writer);
    return Out;
}
