#include "Sequencer/SequenceBindings.h"
#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h" // au lieu de "LevelSequence/LevelSequence.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "ExtensionLibraries/MovieSceneSequenceExtensions.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParameters = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodeSequenceNotFound = TEXT("SEQUENCE_NOT_FOUND");
    [[maybe_unused]] constexpr const TCHAR* ErrorCodeActorNotFound = TEXT("ACTOR_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeBindFailed = TEXT("BIND_FAILED");
    [[maybe_unused]] constexpr const TCHAR* ErrorCodeBindingNotFound = TEXT("BINDING_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeSaveFailed = TEXT("SAVE_FAILED");

    TSharedPtr<FJsonObject> MakeErrorResponse(const FString& Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetBoolField(TEXT("success"), false);
        Error->SetStringField(TEXT("errorCode"), Code);
        Error->SetStringField(TEXT("error"), Message);
        return Error;
    }

    TSharedPtr<FJsonObject> MakeSuccessResponse(const TSharedPtr<FJsonObject>& Payload)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        if (Payload.IsValid())
        {
            Result->SetObjectField(TEXT("data"), Payload);
        }
        return Result;
    }

    UWorld* GetEditorWorld()
    {
#if WITH_EDITOR
        if (!GEditor)
        {
            return nullptr;
        }

        if (FWorldContext* WorldContext = GEditor->GetPIEWorldContext())
        {
            if (WorldContext->World())
            {
                return WorldContext->World();
            }
        }

        return GEditor->GetEditorWorldContext().World();
#else
        return nullptr;
#endif
    }

    AActor* ResolveActor(const FString& ActorIdentifier)
    {
        FString Trimmed = ActorIdentifier;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return nullptr;
        }

        if (AActor* DirectActor = FindObject<AActor>(nullptr, *Trimmed))
        {
            return DirectActor;
        }

        if (UWorld* World = GetEditorWorld())
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                if (It->GetPathName() == Trimmed || It->GetName() == Trimmed)
                {
                    return *It;
                }
            }
        }

        return nullptr;
    }

    FString MakeGuidString(const FGuid& Guid)
    {
        FString Raw = Guid.ToString(EGuidFormats::DigitsWithHyphens);
        return Raw.ToUpper();
    }

    void AppendAuditAction(TArray<TSharedPtr<FJsonValue>>& AuditActions, const FString& Op, const TMap<FString, FString>& Args)
    {
        TSharedPtr<FJsonObject> ActionObject = MakeShared<FJsonObject>();
        ActionObject->SetStringField(TEXT("op"), Op);
        for (const TPair<FString, FString>& Pair : Args)
        {
            ActionObject->SetStringField(Pair.Key, Pair.Value);
        }

        AuditActions.Add(MakeShared<FJsonValueObject>(ActionObject));
    }

    void AppendSkipped(TArray<TSharedPtr<FJsonValue>>& SkippedArray, const FString& ActorPath, const FString& Reason)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actorPath"), ActorPath);
        Entry->SetStringField(TEXT("reason"), Reason);
        SkippedArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    FString GetActorDisplayName(const AActor& Actor)
    {
#if WITH_EDITOR
        return Actor.GetActorLabel();
#else
        return Actor.GetName();
#endif
    }

    TArray<FGuid> FindBindingsForActor(ULevelSequence& Sequence, AActor& Actor)
    {
        TArray<FGuid> Result;
        if (UMovieScene* MovieScene = Sequence.GetMovieScene())
        {
            for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
            {
                const FGuid& Guid = Binding.GetObjectGuid();

                TArray<UObject*, TInlineAllocator<1>> LocatedObjects;
                Sequence.LocateBoundObjects(Guid, Actor.GetWorld(), LocatedObjects);
                for (UObject* BoundObject : LocatedObjects)
                {
                    if (BoundObject == &Actor)
                    {
                        Result.Add(Guid);
                        break;
                    }
                }
            }
        }

        return Result;
    }

    bool RemoveBindingByGuid(ULevelSequence& Sequence, const FGuid& BindingId)
    {
        if (UMovieScene* MovieScene = Sequence.GetMovieScene())
        {
            if (MovieScene->RemoveBinding(BindingId))
            {
                return true;
            }
        }

        return false;
    }

    void AppendAdded(TArray<TSharedPtr<FJsonValue>>& AddedArray, const FString& ActorPath, const FGuid& BindingId, const FString& Label)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actorPath"), ActorPath);
        Entry->SetStringField(TEXT("bindingId"), MakeGuidString(BindingId));
        if (!Label.IsEmpty())
        {
            Entry->SetStringField(TEXT("label"), Label);
        }
        AddedArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    void AppendRemoved(TArray<TSharedPtr<FJsonValue>>& RemovedArray, const FGuid& BindingId)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("bindingId"), MakeGuidString(BindingId));
        RemovedArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    void AppendNotFoundBinding(TArray<TSharedPtr<FJsonValue>>& NotFoundArray, const FString& Identifier, bool bIsBinding)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        if (bIsBinding)
        {
            Entry->SetStringField(TEXT("bindingId"), Identifier);
        }
        else
        {
            Entry->SetStringField(TEXT("actorPath"), Identifier);
        }
        NotFoundArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    FString NormalizeSequencePath(const FString& SequencePath)
    {
        FString Trimmed = SequencePath;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return Trimmed;
        }

        if (!Trimmed.StartsWith(TEXT("/")))
        {
            Trimmed = FString::Printf(TEXT("/Game/%s"), *Trimmed);
        }

        return Trimmed;
    }

    FString ResolveSequenceObjectPath(const FString& SequencePath)
    {
        FString Normalized = NormalizeSequencePath(SequencePath);
        if (Normalized.Contains(TEXT(".")))
        {
            return Normalized;
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(Normalized);
        if (AssetName.IsEmpty())
        {
            return FString();
        }

        return FString::Printf(TEXT("%s.%s"), *Normalized, *AssetName);
    }

    bool SaveSequenceIfRequested(ULevelSequence& Sequence, bool bShouldSave)
    {
        if (!bShouldSave)
        {
            return true;
        }

        if (!UEditorAssetLibrary::SaveLoadedAsset(&Sequence))
        {
            return false;
        }

        return true;
    }
}

TSharedPtr<FJsonObject> FSequenceBindings::BindActors(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing parameters"));
    }

    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequencePath"), SequencePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing sequencePath"));
    }

    const FString SequenceObjectPath = ResolveSequenceObjectPath(SequencePath);
    if (SequenceObjectPath.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Invalid sequencePath"));
    }

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
    if (!LevelSequence)
    {
        return MakeErrorResponse(ErrorCodeSequenceNotFound, FString::Printf(TEXT("Sequence not found: %s"), *SequenceObjectPath));
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        return MakeErrorResponse(ErrorCodeSequenceNotFound, TEXT("Sequence is missing MovieScene"));
    }

    const TArray<TSharedPtr<FJsonValue>>* ActorArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("actorPaths"), ActorArray) || !ActorArray || ActorArray->Num() == 0)
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("actorPaths must be a non-empty array"));
    }

    const bool bSkipIfBound = Params->HasTypedField<EJson::Boolean>(TEXT("skipIfAlreadyBound")) && Params->GetBoolField(TEXT("skipIfAlreadyBound"));
    const bool bOverwriteIfExists = Params->HasTypedField<EJson::Boolean>(TEXT("overwriteIfExists")) && Params->GetBoolField(TEXT("overwriteIfExists"));
    const bool bSave = Params->HasTypedField<EJson::Boolean>(TEXT("save")) && Params->GetBoolField(TEXT("save"));

    FString LabelPrefix;
    Params->TryGetStringField(TEXT("labelPrefix"), LabelPrefix);
    LabelPrefix.TrimStartAndEndInline();

    TArray<TSharedPtr<FJsonValue>> AddedArray;
    TArray<TSharedPtr<FJsonValue>> SkippedArray;
    TArray<TSharedPtr<FJsonValue>> AuditActions;

    bool bModified = false;
    int32 LabelCounter = 1;

    for (const TSharedPtr<FJsonValue>& Value : *ActorArray)
    {
        if (!Value.IsValid() || Value->Type != EJson::String)
        {
            continue;
        }

        const FString ActorPath = Value->AsString();
        AActor* Actor = ResolveActor(ActorPath);
        if (!Actor)
        {
            AppendSkipped(SkippedArray, ActorPath, TEXT("actor_not_found"));
            continue;
        }

        bool bPreparedForChange = false;
        auto PrepareForChange = [&]()
        {
            if (!bPreparedForChange)
            {
                MovieScene->Modify();
                LevelSequence->Modify();
                bPreparedForChange = true;
            }
        };

        TArray<FGuid> ExistingBindings = FindBindingsForActor(*LevelSequence, *Actor);
        const bool bHasExistingBindings = ExistingBindings.Num() > 0;
        if (bHasExistingBindings && bSkipIfBound && !bOverwriteIfExists)
        {
            AppendSkipped(SkippedArray, Actor->GetPathName(), TEXT("already_bound"));
            continue;
        }

        const bool bRemoveExistingAfterAdd = bOverwriteIfExists && bHasExistingBindings;

        PrepareForChange();

        const FString DefaultLabel = GetActorDisplayName(*Actor);
        FMovieScenePossessable& Possessable = MovieScene->AddPossessable(DefaultLabel, Actor->GetClass());
        const FGuid BindingGuid = Possessable.GetGuid();

        if (!LevelSequence->BindPossessableObject(BindingGuid, *Actor, Actor->GetWorld()))
        {
            MovieScene->RemovePossessable(BindingGuid);
            return MakeErrorResponse(ErrorCodeBindFailed, FString::Printf(TEXT("Failed to bind actor: %s"), *Actor->GetPathName()));
        }

        FString FinalLabel = DefaultLabel;
        if (!LabelPrefix.IsEmpty())
        {
            FinalLabel = FString::Printf(TEXT("%s%d"), *LabelPrefix, LabelCounter++);
            UMovieSceneSequenceExtensions::SetDisplayName(LevelSequence, BindingGuid, FText::FromString(FinalLabel));
        }

        AppendAdded(AddedArray, Actor->GetPathName(), BindingGuid, FinalLabel);
        AppendAuditAction(AuditActions, TEXT("bind"), {{TEXT("actor"), Actor->GetPathName()}});
        bModified = true;

        if (bRemoveExistingAfterAdd)
        {
            for (const FGuid& BindingId : ExistingBindings)
            {
                if (RemoveBindingByGuid(*LevelSequence, BindingId))
                {
                    AppendAuditAction(AuditActions, TEXT("unbind"), {{TEXT("bindingId"), MakeGuidString(BindingId)}});
                }
            }
        }
    }

    if (!bModified)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetArrayField(TEXT("added"), AddedArray);
        Data->SetArrayField(TEXT("skipped"), SkippedArray);

        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("dryRun"), false);
        Audit->SetArrayField(TEXT("actions"), AuditActions);
        Data->SetObjectField(TEXT("audit"), Audit);

        return MakeSuccessResponse(Data);
    }

    LevelSequence->MarkPackageDirty();

    if (!SaveSequenceIfRequested(*LevelSequence, bSave))
    {
        return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save sequence"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetArrayField(TEXT("added"), AddedArray);
    Data->SetArrayField(TEXT("skipped"), SkippedArray);

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), false);
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FSequenceBindings::Unbind(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing parameters"));
    }

    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequencePath"), SequencePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing sequencePath"));
    }

    const FString SequenceObjectPath = ResolveSequenceObjectPath(SequencePath);
    if (SequenceObjectPath.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Invalid sequencePath"));
    }

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
    if (!LevelSequence)
    {
        return MakeErrorResponse(ErrorCodeSequenceNotFound, FString::Printf(TEXT("Sequence not found: %s"), *SequenceObjectPath));
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        return MakeErrorResponse(ErrorCodeSequenceNotFound, TEXT("Sequence is missing MovieScene"));
    }

    const TArray<TSharedPtr<FJsonValue>>* BindingIdArray = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* ActorArray = nullptr;

    const bool bHasBindingIds = Params->TryGetArrayField(TEXT("bindingIds"), BindingIdArray) && BindingIdArray && BindingIdArray->Num() > 0;
    const bool bHasActorPaths = Params->TryGetArrayField(TEXT("actorPaths"), ActorArray) && ActorArray && ActorArray->Num() > 0;

    if (!bHasBindingIds && !bHasActorPaths)
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Must supply bindingIds or actorPaths"));
    }

    const bool bSave = Params->HasTypedField<EJson::Boolean>(TEXT("save")) && Params->GetBoolField(TEXT("save"));

    TArray<FGuid> BindingIdsToRemove;
    TArray<TSharedPtr<FJsonValue>> NotFoundArray;

    if (bHasBindingIds)
    {
        for (const TSharedPtr<FJsonValue>& Value : *BindingIdArray)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                continue;
            }

            const FString BindingIdString = Value->AsString();
            FGuid BindingGuid;
            if (FGuid::ParseExact(BindingIdString, EGuidFormats::DigitsWithHyphens, BindingGuid) || FGuid::Parse(BindingIdString, BindingGuid))
            {
                BindingIdsToRemove.AddUnique(BindingGuid);
            }
            else
            {
                AppendNotFoundBinding(NotFoundArray, BindingIdString, true);
            }
        }
    }

    if (bHasActorPaths)
    {
        for (const TSharedPtr<FJsonValue>& Value : *ActorArray)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                continue;
            }

            const FString ActorPath = Value->AsString();
            AActor* Actor = ResolveActor(ActorPath);
            if (!Actor)
            {
                AppendNotFoundBinding(NotFoundArray, ActorPath, false);
                continue;
            }

            TArray<FGuid> ActorBindings = FindBindingsForActor(*LevelSequence, *Actor);
            if (ActorBindings.Num() == 0)
            {
                AppendNotFoundBinding(NotFoundArray, Actor->GetPathName(), false);
                continue;
            }

            for (const FGuid& Guid : ActorBindings)
            {
                BindingIdsToRemove.AddUnique(Guid);
            }
        }
    }

    if (BindingIdsToRemove.Num() == 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetArrayField(TEXT("removed"), {});
        Data->SetArrayField(TEXT("notFound"), NotFoundArray);

        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("dryRun"), false);
        Audit->SetArrayField(TEXT("actions"), {});
        Data->SetObjectField(TEXT("audit"), Audit);

        return MakeSuccessResponse(Data);
    }

    TArray<TSharedPtr<FJsonValue>> RemovedArray;
    TArray<TSharedPtr<FJsonValue>> AuditActions;
    bool bAnyRemoved = false;

    MovieScene->Modify();
    LevelSequence->Modify();

    for (const FGuid& BindingId : BindingIdsToRemove)
    {
        if (RemoveBindingByGuid(*LevelSequence, BindingId))
        {
            AppendRemoved(RemovedArray, BindingId);
            AppendAuditAction(AuditActions, TEXT("unbind"), {{TEXT("bindingId"), MakeGuidString(BindingId)}});
            bAnyRemoved = true;
        }
        else
        {
            AppendNotFoundBinding(NotFoundArray, MakeGuidString(BindingId), true);
        }
    }

    if (bAnyRemoved)
    {
        LevelSequence->MarkPackageDirty();

        if (!SaveSequenceIfRequested(*LevelSequence, bSave))
        {
            return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save sequence"));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetArrayField(TEXT("removed"), RemovedArray);
    Data->SetArrayField(TEXT("notFound"), NotFoundArray);

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), false);
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FSequenceBindings::List(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing parameters"));
    }

    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequencePath"), SequencePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing sequencePath"));
    }

    const FString SequenceObjectPath = ResolveSequenceObjectPath(SequencePath);
    if (SequenceObjectPath.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Invalid sequencePath"));
    }

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
    if (!LevelSequence)
    {
        return MakeErrorResponse(ErrorCodeSequenceNotFound, FString::Printf(TEXT("Sequence not found: %s"), *SequenceObjectPath));
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        return MakeErrorResponse(ErrorCodeSequenceNotFound, TEXT("Sequence is missing MovieScene"));
    }

    UWorld* World = GetEditorWorld();

    TArray<TSharedPtr<FJsonValue>> BindingsArray;

    for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
    {
        TSharedPtr<FJsonObject> BindingJson = MakeShared<FJsonObject>();
        const FGuid& Guid = Binding.GetObjectGuid();
        BindingJson->SetStringField(TEXT("bindingId"), MakeGuidString(Guid));

        const FText DisplayName = MovieScene->GetObjectDisplayName(Guid);
        BindingJson->SetStringField(TEXT("label"), DisplayName.ToString());

        if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
        {
            BindingJson->SetStringField(TEXT("possessedObjectClass"), Possessable->GetPossessedObjectClassName());
        }

        if (World)
        {
            TArray<UObject*, TInlineAllocator<1>> LocatedObjects;
            LevelSequence->LocateBoundObjects(Guid, World, LocatedObjects);
            for (UObject* Object : LocatedObjects)
            {
                if (AActor* Actor = Cast<AActor>(Object))
                {
                    BindingJson->SetStringField(TEXT("boundActorPath"), Actor->GetPathName());
                    break;
                }
            }
        }

        if (!BindingJson->HasField(TEXT("boundActorPath")))
        {
            BindingJson->SetField(TEXT("boundActorPath"), MakeShared<FJsonValueNull>());
        }

        BindingsArray.Add(MakeShared<FJsonValueObject>(BindingJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetArrayField(TEXT("bindings"), BindingsArray);

    return MakeSuccessResponse(Data);
}
