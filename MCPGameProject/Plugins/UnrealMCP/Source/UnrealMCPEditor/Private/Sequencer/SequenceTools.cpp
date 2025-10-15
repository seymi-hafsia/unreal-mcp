#include "Sequencer/SequenceTools.h"
#include "CoreMinimal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "CineCameraActor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/EngineTypes.h"
#include "Factories/LevelSequenceFactoryNew.h"
#include "LevelSequence.h" // au lieu de "LevelSequence/LevelSequence.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "Permissions/WriteGate.h"
#include "Sections/MovieSceneSection.h"
#include "String/LexFromString.h"
#include "String/LexToString.h"
#include "GameFramework/Actor.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "SourceControlService.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParameters = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodePathNotAllowed = TEXT("PATH_NOT_ALLOWED");
    constexpr const TCHAR* ErrorCodeAssetExists = TEXT("ASSET_ALREADY_EXISTS");
    constexpr const TCHAR* ErrorCodeCreateAssetFailed = TEXT("CREATE_ASSET_FAILED");
    constexpr const TCHAR* ErrorCodeSequencerInitFailed = TEXT("SEQUENCER_INIT_FAILED");
    constexpr const TCHAR* ErrorCodeDeleteFailed = TEXT("DELETE_FAILED");
    constexpr const TCHAR* ErrorCodeSaveFailed = TEXT("SAVE_FAILED");
    constexpr const TCHAR* ErrorCodeCameraSpawnFailed = TEXT("CAMERA_SPAWN_FAILED");
    constexpr const TCHAR* ErrorCodeCameraCutFailed = TEXT("CAMERA_CUT_FAILED");
    constexpr const TCHAR* ErrorCodeBindFailed = TEXT("BIND_FAILED");
    constexpr const TCHAR* ErrorCodeSourceControlRequired = TEXT("SOURCE_CONTROL_REQUIRED");
    constexpr const TCHAR* ErrorCodeSourceControlOperationFailed = TEXT("SC_OPERATION_FAILED");

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

    FString NormalizeContentPath(const FString& InPath)
    {
        FString Trimmed = InPath;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return Trimmed;
        }

        FString Normalized = Trimmed;
        if (!Normalized.StartsWith(TEXT("/")))
        {
            Normalized = FString::Printf(TEXT("/Game/%s"), *Normalized);
        }

        if (Normalized.Contains(TEXT(".")))
        {
            Normalized = FPackageName::ObjectPathToPackageName(Normalized);
        }

        return Normalized;
    }

    bool ParseIntLike(const TSharedPtr<FJsonValue>& Value, int32& OutValue)
    {
        if (!Value.IsValid())
        {
            return false;
        }

        if (Value->Type == EJson::Number)
        {
            OutValue = static_cast<int32>(Value->AsNumber());
            return true;
        }

        if (Value->Type == EJson::String)
        {
            return LexTryParseString(OutValue, *Value->AsString());
        }

        return false;
    }

    bool ParseFrameRateArray(const TArray<TSharedPtr<FJsonValue>>& Values, FFrameRate& OutRate)
    {
        if (Values.Num() != 2)
        {
            return false;
        }

        int32 Numerator = 0;
        int32 Denominator = 0;
        if (!ParseIntLike(Values[0], Numerator) || !ParseIntLike(Values[1], Denominator))
        {
            return false;
        }

        if (Numerator <= 0 || Denominator <= 0)
        {
            return false;
        }

        OutRate = FFrameRate(Numerator, Denominator);
        return true;
    }

    bool ParseFrameRateField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FFrameRate& OutRate, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        if (!Params->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr)
        {
            OutError = FString::Printf(TEXT("Missing %s array"), *FieldName);
            return false;
        }

        if (!ParseFrameRateArray(*ArrayPtr, OutRate))
        {
            OutError = FString::Printf(TEXT("Invalid %s array"), *FieldName);
            return false;
        }

        return true;
    }

    bool ParseOptionalFrameRateField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FFrameRate& OutRate, bool& bOutProvided, FString& OutError)
    {
        bOutProvided = false;
        if (!Params->HasField(FieldName))
        {
            return true;
        }

        bOutProvided = true;
        return ParseFrameRateField(Params, FieldName, OutRate, OutError);
    }

    bool ParseIntField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, int32& OutValue)
    {
        if (Params->HasTypedField<EJson::Number>(FieldName))
        {
            OutValue = static_cast<int32>(Params->GetNumberField(FieldName));
            return true;
        }

        if (Params->HasTypedField<EJson::String>(FieldName))
        {
            return LexTryParseString(OutValue, *Params->GetStringField(FieldName));
        }

        return false;
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

    enum class EMarkForAddResult
    {
        Success,
        SourceControlUnavailable,
        OperationFailed
    };

    EMarkForAddResult MarkPackageForAdd(const FString& PackagePath, FString& OutError)
    {
        if (!FSourceControlService::IsEnabled())
        {
            return EMarkForAddResult::Success;
        }

        TArray<FString> Files;
        FString ConversionError;
        if (!FSourceControlService::AssetPathsToFiles({PackagePath}, Files, ConversionError))
        {
            OutError = ConversionError;
            return EMarkForAddResult::SourceControlUnavailable;
        }

        if (Files.Num() == 0)
        {
            return EMarkForAddResult::Success;
        }

        TMap<FString, bool> PerFileResult;
        FString OperationError;
        if (!FSourceControlService::MarkForAdd(Files, PerFileResult, OperationError))
        {
            OutError = OperationError;
            return EMarkForAddResult::OperationFailed;
        }

        for (const TPair<FString, bool>& Pair : PerFileResult)
        {
            if (!Pair.Value)
            {
                OutError = OperationError;
                return EMarkForAddResult::OperationFailed;
            }
        }

        return EMarkForAddResult::Success;
    }

    FString GetActorDisplayName(const AActor& Actor)
    {
#if WITH_EDITOR
        return Actor.GetActorLabel();
#else
        return Actor.GetName();
#endif
    }

    void AppendBindingsResult(const TArray<FString>& FailedBindings, TSharedPtr<FJsonObject>& OutData)
    {
        if (FailedBindings.Num() == 0 || !OutData.IsValid())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> FailedArray;
        for (const FString& Identifier : FailedBindings)
        {
            FailedArray.Add(MakeShared<FJsonValueString>(Identifier));
        }

        OutData->SetArrayField(TEXT("failedBindings"), FailedArray);
    }
}

TSharedPtr<FJsonObject> FSequenceTools::Create(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing parameters"));
    }

    FString RawSequencePath;
    if (!Params->TryGetStringField(TEXT("sequencePath"), RawSequencePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing sequencePath"));
    }

    const FString SequencePackagePath = NormalizeContentPath(RawSequencePath);
    if (!SequencePackagePath.StartsWith(TEXT("/Game/")))
    {
        return MakeErrorResponse(ErrorCodePathNotAllowed, TEXT("Sequence path must be under /Game"));
    }

    if (!FPackageName::IsValidLongPackageName(SequencePackagePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, FString::Printf(TEXT("Invalid package path: %s"), *SequencePackagePath));
    }

    FString PathReason;
    if (!FWriteGate::IsPathAllowed(SequencePackagePath, PathReason))
    {
        return MakeErrorResponse(ErrorCodePathNotAllowed, PathReason);
    }

    const FString AssetName = FPackageName::GetLongPackageAssetName(SequencePackagePath);
    if (AssetName.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Sequence path must include asset name"));
    }

    const FString PackageFolder = FPackageName::GetLongPackagePath(SequencePackagePath);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *SequencePackagePath, *AssetName);

    FFrameRate DisplayRate;
    FString FrameRateError;
    if (!ParseFrameRateField(Params, TEXT("displayRate"), DisplayRate, FrameRateError))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, FrameRateError);
    }

    FFrameRate TickResolution;
    bool bCustomTickResolution = false;
    if (!ParseOptionalFrameRateField(Params, TEXT("tickResolution"), TickResolution, bCustomTickResolution, FrameRateError))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, FrameRateError);
    }

    int32 DurationFrames = 0;
    if (!ParseIntField(Params, TEXT("durationFrames"), DurationFrames) || DurationFrames <= 0)
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Invalid durationFrames"));
    }

    FString EvaluationTypeString;
    Params->TryGetStringField(TEXT("evaluationType"), EvaluationTypeString);

    EMovieSceneEvaluationType EvaluationType = EMovieSceneEvaluationType::WithSubFrames;
    if (!EvaluationTypeString.IsEmpty())
    {
        if (EvaluationTypeString.Equals(TEXT("WithSubFrames"), ESearchCase::IgnoreCase))
        {
            EvaluationType = EMovieSceneEvaluationType::WithSubFrames;
        }
        else if (EvaluationTypeString.Equals(TEXT("FrameLocked"), ESearchCase::IgnoreCase))
        {
            EvaluationType = EMovieSceneEvaluationType::FrameLocked;
        }
        else
        {
            return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Unsupported evaluationType"));
        }
    }

    const bool bCreateCamera = Params->HasTypedField<EJson::Boolean>(TEXT("createCamera")) && Params->GetBoolField(TEXT("createCamera"));
    const bool bAddCameraCut = Params->HasTypedField<EJson::Boolean>(TEXT("addCameraCut")) && Params->GetBoolField(TEXT("addCameraCut"));
    const bool bOverwriteIfExists = Params->HasTypedField<EJson::Boolean>(TEXT("overwriteIfExists")) && Params->GetBoolField(TEXT("overwriteIfExists"));

    FString CameraName;
    Params->TryGetStringField(TEXT("cameraName"), CameraName);

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
    const bool bAssetExists = ExistingAsset.IsValid();

    if (bAssetExists && !bOverwriteIfExists)
    {
        return MakeErrorResponse(ErrorCodeAssetExists, TEXT("Sequence asset already exists"));
    }

    if (bAssetExists && bOverwriteIfExists)
    {
        if (!UEditorAssetLibrary::DeleteAsset(ObjectPath))
        {
            return MakeErrorResponse(ErrorCodeDeleteFailed, TEXT("Failed to delete existing sequence asset"));
        }
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    ULevelSequenceFactoryNew* Factory = NewObject<ULevelSequenceFactoryNew>();
    UObject* NewAsset = AssetToolsModule.Get().CreateAsset(AssetName, PackageFolder, ULevelSequence::StaticClass(), Factory);
    ULevelSequence* LevelSequence = Cast<ULevelSequence>(NewAsset);
    if (!LevelSequence)
    {
        return MakeErrorResponse(ErrorCodeCreateAssetFailed, TEXT("Failed to create Level Sequence asset"));
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        return MakeErrorResponse(ErrorCodeSequencerInitFailed, TEXT("Failed to initialize Movie Scene"));
    }

    MovieScene->Modify();
    MovieScene->SetDisplayRate(DisplayRate);
    if (bCustomTickResolution)
    {
        MovieScene->SetTickResolution(TickResolution);
    }
    MovieScene->SetEvaluationType(EvaluationType);

    const TRange<FFrameNumber> PlaybackRange(FFrameNumber(0), FFrameNumber(DurationFrames));
    MovieScene->SetPlaybackRange(PlaybackRange);

    const double DurationSeconds = static_cast<double>(DurationFrames) / DisplayRate.AsDecimal();
    MovieScene->SetWorkingRange(0.0, DurationSeconds);
    MovieScene->SetViewRange(0.0, DurationSeconds);

    FString CreatedCameraPath;
    FGuid CameraBindingGuid;
    bool bAddedCameraCut = false;
    TArray<FString> FailedBindings;

    ACineCameraActor* SpawnedCamera = nullptr;
    if (bCreateCamera)
    {
        UWorld* World = GetEditorWorld();
        if (!World)
        {
            return MakeErrorResponse(ErrorCodeCameraSpawnFailed, TEXT("Editor world unavailable"));
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.Name = CameraName.IsEmpty() ? NAME_None : FName(*CameraName);
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

        SpawnedCamera = World->SpawnActor<ACineCameraActor>(ACineCameraActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
        if (!SpawnedCamera)
        {
            return MakeErrorResponse(ErrorCodeCameraSpawnFailed, TEXT("Failed to spawn CineCameraActor"));
        }

        if (!CameraName.IsEmpty())
        {
            SpawnedCamera->Rename(*CameraName);
#if WITH_EDITOR
            SpawnedCamera->SetActorLabel(CameraName);
#endif
        }

        CreatedCameraPath = SpawnedCamera->GetPathName();

        MovieScene->Modify();
        FMovieScenePossessable& Possessable = MovieScene->AddPossessable(GetActorDisplayName(*SpawnedCamera), SpawnedCamera->GetClass());
        CameraBindingGuid = Possessable.GetGuid();
        const bool bBound = LevelSequence->BindPossessableObject(CameraBindingGuid, *SpawnedCamera, SpawnedCamera->GetWorld());
        if (!bBound)
        {
            return MakeErrorResponse(ErrorCodeBindFailed, TEXT("Failed to bind spawned camera"));
        }
    }

    if (bAddCameraCut)
    {
        if (!CameraBindingGuid.IsValid())
        {
            return MakeErrorResponse(ErrorCodeCameraCutFailed, TEXT("Camera cut requested but no camera was created"));
        }

        UMovieSceneCameraCutTrack* CameraCutTrack = MovieScene->FindTrack<UMovieSceneCameraCutTrack>();
        if (!CameraCutTrack)
        {
            CameraCutTrack = MovieScene->AddTrack<UMovieSceneCameraCutTrack>();
        }

        if (!CameraCutTrack)
        {
            return MakeErrorResponse(ErrorCodeCameraCutFailed, TEXT("Failed to create camera cut track"));
        }

        CameraCutTrack->Modify();
        UMovieSceneCameraCutSection* CutSection = Cast<UMovieSceneCameraCutSection>(CameraCutTrack->CreateNewSection());
        if (!CutSection)
        {
            return MakeErrorResponse(ErrorCodeCameraCutFailed, TEXT("Failed to create camera cut section"));
        }

        CutSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(DurationFrames)));
        CutSection->SetCameraBindingID(FMovieSceneObjectBindingID(CameraBindingGuid));
        CameraCutTrack->AddSection(*CutSection);
        bAddedCameraCut = true;
    }

    const TArray<TSharedPtr<FJsonValue>>* BindArray = nullptr;
    int32 BoundActors = 0;
    if (Params->TryGetArrayField(TEXT("bindActors"), BindArray) && BindArray)
    {
        for (const TSharedPtr<FJsonValue>& Value : *BindArray)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                continue;
            }

            const FString ActorIdentifier = Value->AsString();
            AActor* TargetActor = ResolveActor(ActorIdentifier);
            if (!TargetActor)
            {
                FailedBindings.Add(ActorIdentifier);
                continue;
            }

            MovieScene->Modify();
            FMovieScenePossessable& Possessable = MovieScene->AddPossessable(GetActorDisplayName(*TargetActor), TargetActor->GetClass());
            const FGuid BindingGuid = Possessable.GetGuid();
            const bool bBound = LevelSequence->BindPossessableObject(BindingGuid, *TargetActor, TargetActor->GetWorld());
            if (bBound)
            {
                ++BoundActors;
            }
            else
            {
                FailedBindings.Add(ActorIdentifier);
            }
        }
    }

    LevelSequence->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(ObjectPath))
    {
        return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save Level Sequence"));
    }

    if (!bAssetExists)
    {
        FString MarkForAddError;
        const EMarkForAddResult MarkResult = MarkPackageForAdd(SequencePackagePath, MarkForAddError);
        if (MarkResult == EMarkForAddResult::SourceControlUnavailable)
        {
            return MakeErrorResponse(ErrorCodeSourceControlRequired, MarkForAddError.IsEmpty() ? TEXT("Source control provider unavailable") : MarkForAddError);
        }
        if (MarkResult == EMarkForAddResult::OperationFailed)
        {
            return MakeErrorResponse(ErrorCodeSourceControlOperationFailed, MarkForAddError.IsEmpty() ? TEXT("Failed to mark asset for add") : MarkForAddError);
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);

    TSharedPtr<FJsonObject> SequenceObject = MakeShared<FJsonObject>();
    SequenceObject->SetStringField(TEXT("assetPath"), ObjectPath);

    TArray<TSharedPtr<FJsonValue>> DisplayRateArray;
    DisplayRateArray.Add(MakeShared<FJsonValueNumber>(DisplayRate.Numerator));
    DisplayRateArray.Add(MakeShared<FJsonValueNumber>(DisplayRate.Denominator));
    SequenceObject->SetArrayField(TEXT("fps"), DisplayRateArray);
    SequenceObject->SetNumberField(TEXT("durationFrames"), DurationFrames);

    Data->SetObjectField(TEXT("sequence"), SequenceObject);

    if (!CreatedCameraPath.IsEmpty())
    {
        Data->SetStringField(TEXT("createdCamera"), CreatedCameraPath);
    }

    Data->SetBoolField(TEXT("addedCameraCut"), bAddedCameraCut);
    Data->SetNumberField(TEXT("boundActors"), BoundActors);

    AppendBindingsResult(FailedBindings, Data);

    return MakeSuccessResponse(Data);
}
