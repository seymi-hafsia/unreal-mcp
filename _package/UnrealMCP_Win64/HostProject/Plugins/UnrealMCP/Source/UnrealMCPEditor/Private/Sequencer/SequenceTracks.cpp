#include "Sequencer/SequenceTracks.h"
#include "CoreMinimal.h"

#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelSequence.h"
#include "String/LexFromString.h"
#include "Misc/LexToString.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "Permissions/WriteGate.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneIntegerSection.h"
#include "Sections/MovieSceneSection.h"
#include "SourceControlService.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParameters = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodeSequenceNotFound = TEXT("SEQUENCE_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeActorNotBound = TEXT("ACTOR_NOT_BOUND");
    constexpr const TCHAR* ErrorCodeTrackExists = TEXT("TRACK_EXISTS");
    constexpr const TCHAR* ErrorCodeTrackAddFailed = TEXT("TRACK_ADD_FAILED");
    constexpr const TCHAR* ErrorCodePropertyNotFound = TEXT("PROPERTY_NOT_FOUND");
    constexpr const TCHAR* ErrorCodePropertyUnsupported = TEXT("PROPERTY_NOT_SUPPORTED");
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

    void AppendAuditAction(TArray<TSharedPtr<FJsonValue>>& AuditActions, const FString& Op, const TMap<FString, FString>& Args)
    {
        TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
        Action->SetStringField(TEXT("op"), Op);
        for (const TPair<FString, FString>& Pair : Args)
        {
            Action->SetStringField(Pair.Key, Pair.Value);
        }

        AuditActions.Add(MakeShared<FJsonValueObject>(Action));
    }

    bool ParseFrameNumber(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, int32& OutFrame)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        if (Object->HasTypedField<EJson::Number>(FieldName))
        {
            OutFrame = static_cast<int32>(Object->GetNumberField(FieldName));
            return true;
        }

        if (Object->HasTypedField<EJson::String>(FieldName))
        {
            return LexTryParseString(OutFrame, *Object->GetStringField(FieldName));
        }

        return false;
    }

    bool ParseFloatArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, int32 ExpectedNum, TArray<double>& OutValues, bool& bOutProvided)
    {
        bOutProvided = false;
        OutValues.Reset();

        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        if (!Object->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr)
        {
            return true;
        }

        if (ExpectedNum > 0 && ArrayPtr->Num() != ExpectedNum)
        {
            return false;
        }

        OutValues.Reserve(ArrayPtr->Num());
        for (const TSharedPtr<FJsonValue>& Value : *ArrayPtr)
        {
            if (!Value.IsValid() || Value->Type != EJson::Number)
            {
                return false;
            }

            OutValues.Add(Value->AsNumber());
        }

        bOutProvided = true;
        return true;
    }

    bool ParseBoolField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, bool& OutBool)
    {
        if (Object->HasTypedField<EJson::Boolean>(FieldName))
        {
            OutBool = Object->GetBoolField(FieldName);
            return true;
        }

        if (Object->HasTypedField<EJson::Number>(FieldName))
        {
            OutBool = !FMath::IsNearlyZero(Object->GetNumberField(FieldName));
            return true;
        }

        if (Object->HasTypedField<EJson::String>(FieldName))
        {
            const FString Value = Object->GetStringField(FieldName);
            if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1"))
            {
                OutBool = true;
                return true;
            }

            if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value == TEXT("0"))
            {
                OutBool = false;
                return true;
            }
        }

        return false;
    }

    struct FBindingLookupResult
    {
        FGuid BindingId;
        FString ActorPath;
        UObject* TemplateObject = nullptr;
        UClass* BoundClass = nullptr;
    };

    bool FindBindingForActorPath(ULevelSequence& Sequence, UMovieScene& MovieScene, const FString& ActorPath, FBindingLookupResult& OutResult)
    {
        FString TrimmedPath = ActorPath;
        TrimmedPath.TrimStartAndEndInline();
        if (TrimmedPath.IsEmpty())
        {
            return false;
        }

        UWorld* World = GetEditorWorld();

        for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
        {
            const FGuid& Guid = Binding.GetObjectGuid();

            if (World)
            {
                TArray<UObject*, TInlineAllocator<1>> LocatedObjects;
                Sequence.LocateBoundObjects(Guid, World, LocatedObjects);
                for (UObject* Object : LocatedObjects)
                {
                    if (!Object)
                    {
                        continue;
                    }

                    if (Object->GetPathName() == TrimmedPath)
                    {
                        OutResult.BindingId = Guid;
                        OutResult.ActorPath = TrimmedPath;

                        if (const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid))
                        {
                            OutResult.BoundClass = Possessable->GetPossessedObjectClass();
                        }
                        else if (const FMovieSceneSpawnable* Spawnable = MovieScene.FindSpawnable(Guid))
                        {
                            OutResult.TemplateObject = Spawnable->GetObjectTemplate();
                            OutResult.BoundClass = OutResult.TemplateObject ? OutResult.TemplateObject->GetClass() : nullptr;
                        }

                        if (!OutResult.TemplateObject && Object)
                        {
                            OutResult.TemplateObject = Object;
                        }

                        return true;
                    }
                }
            }

            if (!World)
            {
                if (const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid))
                {
                    if (Possessable->GetName() == TrimmedPath)
                    {
                        OutResult.BindingId = Guid;
                        OutResult.ActorPath = TrimmedPath;
                        OutResult.BoundClass = Possessable->GetPossessedObjectClass();
                        return true;
                    }
                }
            }
        }

        return false;
    }

    FFrameNumber ConvertDisplayFrameToTick(const UMovieScene& MovieScene, int32 Frame)
    {
        const FFrameRate DisplayRate = MovieScene.GetDisplayRate();
        const FFrameRate TickResolution = MovieScene.GetTickResolution();

        const FFrameTime SourceTime(Frame);
        const FFrameTime Converted = FFrameRate::TransformTime(SourceTime, DisplayRate, TickResolution);
        return Converted.FloorToFrame();
    }

    void EnsureTrackForOverwrite(UMovieSceneTrack& Track, bool bOverwrite)
    {
        Track.Modify();
        if (bOverwrite)
        {
            Track.RemoveAllAnimationData();
        }
    }

    template <typename TrackType>
    TrackType* FindExistingTrack(const FMovieSceneBinding& Binding)
    {
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (Track && Track->IsA<TrackType>())
            {
                return Cast<TrackType>(Track);
            }
        }

        return nullptr;
    }

    UMovieSceneTrack* FindPropertyTrack(const FMovieSceneBinding& Binding, const FString& PropertyPath, TSubclassOf<UMovieScenePropertyTrack> TrackClass)
    {
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (Track && Track->IsA(TrackClass))
            {
                if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
                {
                    if (PropertyTrack->GetPropertyPath() == PropertyPath)
                    {
                        return Track;
                    }
                }
            }
        }

        return nullptr;
    }

    struct FTransformKey
    {
        FFrameNumber Frame;
        bool bHasLocation = false;
        FVector Location = FVector::ZeroVector;
        bool bHasRotation = false;
        FRotator Rotation = FRotator::ZeroRotator;
        bool bHasScale = false;
        FVector Scale = FVector::OneVector;
    };

    bool ParseTransformKeys(const UMovieScene& MovieScene, const TArray<TSharedPtr<FJsonValue>>& KeysJson, TArray<FTransformKey>& OutKeys)
    {
        OutKeys.Reset();
        for (const TSharedPtr<FJsonValue>& Value : KeysJson)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                return false;
            }

            TSharedPtr<FJsonObject> KeyObject = Value->AsObject();
            int32 Frame = 0;
            if (!ParseFrameNumber(KeyObject, TEXT("frame"), Frame))
            {
                return false;
            }

            FTransformKey Key;
            Key.Frame = ConvertDisplayFrameToTick(MovieScene, Frame);

            TArray<double> LocationValues;
            bool bHasLocationValues = false;
            if (!ParseFloatArrayField(KeyObject, TEXT("location"), 3, LocationValues, bHasLocationValues))
            {
                return false;
            }
            if (bHasLocationValues)
            {
                Key.bHasLocation = true;
                Key.Location = FVector(LocationValues[0], LocationValues[1], LocationValues[2]);
            }

            TArray<double> RotationValues;
            bool bHasRotationValues = false;
            if (!ParseFloatArrayField(KeyObject, TEXT("rotation"), 3, RotationValues, bHasRotationValues))
            {
                return false;
            }
            if (bHasRotationValues)
            {
                Key.bHasRotation = true;
                Key.Rotation = FRotator(RotationValues[0], RotationValues[1], RotationValues[2]);
            }

            TArray<double> ScaleValues;
            bool bHasScaleValues = false;
            if (!ParseFloatArrayField(KeyObject, TEXT("scale"), 3, ScaleValues, bHasScaleValues))
            {
                return false;
            }
            if (bHasScaleValues)
            {
                Key.bHasScale = true;
                Key.Scale = FVector(ScaleValues[0], ScaleValues[1], ScaleValues[2]);
            }

            OutKeys.Add(Key);
        }

        return true;
    }

    bool ApplyTransformTrack(ULevelSequence& Sequence, UMovieScene& MovieScene, FMovieSceneBinding& Binding, const FBindingLookupResult& BindingInfo, const TArray<TSharedPtr<FJsonValue>>& KeysArray, bool bOverwrite, FString& OutError, FString& OutErrorCode, TArray<TSharedPtr<FJsonValue>>& AuditActions)
    {
        if (KeysArray.Num() == 0)
        {
            OutError = TEXT("Transform track requires keys");
            OutErrorCode = ErrorCodeInvalidParameters;
            return false;
        }

        TArray<FTransformKey> Keys;
        if (!ParseTransformKeys(MovieScene, KeysArray, Keys))
        {
            OutError = TEXT("Invalid transform keys");
            OutErrorCode = ErrorCodeInvalidParameters;
            return false;
        }

        UMovieScene3DTransformTrack* Track = FindExistingTrack<UMovieScene3DTransformTrack>(Binding);
        if (Track && !bOverwrite)
        {
            OutError = TEXT("Transform track already exists");
            OutErrorCode = ErrorCodeTrackExists;
            return false;
        }

        if (!Track)
        {
            Track = MovieScene.AddTrack<UMovieScene3DTransformTrack>(Binding.GetObjectGuid());
        }

        if (!Track)
        {
            OutError = TEXT("Failed to create transform track");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        EnsureTrackForOverwrite(*Track, bOverwrite);

        UMovieSceneSection* NewSection = Track->CreateNewSection();
        if (!NewSection)
        {
            OutError = TEXT("Failed to create transform section");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(NewSection);
        if (!TransformSection)
        {
            OutError = TEXT("Invalid transform section");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        TransformSection->SetMask(EMovieSceneTransformChannel::All);

        FFrameNumber MinFrame = Keys[0].Frame;
        FFrameNumber MaxFrame = Keys[0].Frame;

        FMovieSceneFloatChannel& TranslationX = TransformSection->GetTranslationChannel(EAxis::X);
        FMovieSceneFloatChannel& TranslationY = TransformSection->GetTranslationChannel(EAxis::Y);
        FMovieSceneFloatChannel& TranslationZ = TransformSection->GetTranslationChannel(EAxis::Z);
        FMovieSceneFloatChannel& RotationX = TransformSection->GetRotationChannel(EAxis::X);
        FMovieSceneFloatChannel& RotationY = TransformSection->GetRotationChannel(EAxis::Y);
        FMovieSceneFloatChannel& RotationZ = TransformSection->GetRotationChannel(EAxis::Z);
        FMovieSceneFloatChannel& ScaleX = TransformSection->GetScaleChannel(EAxis::X);
        FMovieSceneFloatChannel& ScaleY = TransformSection->GetScaleChannel(EAxis::Y);
        FMovieSceneFloatChannel& ScaleZ = TransformSection->GetScaleChannel(EAxis::Z);

        for (const FTransformKey& Key : Keys)
        {
            MinFrame = FMath::Min(MinFrame, Key.Frame);
            MaxFrame = FMath::Max(MaxFrame, Key.Frame);

            if (Key.bHasLocation)
            {
                TranslationX.AddCubicKey(Key.Frame, Key.Location.X);
                TranslationY.AddCubicKey(Key.Frame, Key.Location.Y);
                TranslationZ.AddCubicKey(Key.Frame, Key.Location.Z);
            }

            if (Key.bHasRotation)
            {
                RotationX.AddCubicKey(Key.Frame, Key.Rotation.Roll);
                RotationY.AddCubicKey(Key.Frame, Key.Rotation.Pitch);
                RotationZ.AddCubicKey(Key.Frame, Key.Rotation.Yaw);
            }

            if (Key.bHasScale)
            {
                ScaleX.AddCubicKey(Key.Frame, Key.Scale.X);
                ScaleY.AddCubicKey(Key.Frame, Key.Scale.Y);
                ScaleZ.AddCubicKey(Key.Frame, Key.Scale.Z);
            }
        }

        const TRange<FFrameNumber> Range = TRange<FFrameNumber>::Inclusive(MinFrame, MaxFrame);
        TransformSection->SetRange(Range);
        Track->AddSection(*TransformSection);

        AppendAuditAction(AuditActions, TEXT("add_track"), {
            {TEXT("type"), TEXT("Transform")},
            {TEXT("actor"), BindingInfo.ActorPath}
        });

        return true;
    }

    bool ApplyVisibilityTrack(ULevelSequence& Sequence, UMovieScene& MovieScene, FMovieSceneBinding& Binding, const FBindingLookupResult& BindingInfo, const TArray<TSharedPtr<FJsonValue>>& KeysArray, bool bOverwrite, FString& OutError, FString& OutErrorCode, TArray<TSharedPtr<FJsonValue>>& AuditActions)
    {
        if (KeysArray.Num() == 0)
        {
            OutError = TEXT("Visibility track requires keys");
            OutErrorCode = ErrorCodeInvalidParameters;
            return false;
        }

        UMovieSceneVisibilityTrack* Track = FindExistingTrack<UMovieSceneVisibilityTrack>(Binding);
        if (Track && !bOverwrite)
        {
            OutError = TEXT("Visibility track already exists");
            OutErrorCode = ErrorCodeTrackExists;
            return false;
        }

        if (!Track)
        {
            Track = MovieScene.AddTrack<UMovieSceneVisibilityTrack>(Binding.GetObjectGuid());
        }

        if (!Track)
        {
            OutError = TEXT("Failed to create visibility track");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        EnsureTrackForOverwrite(*Track, bOverwrite);

        UMovieSceneSection* Section = Track->CreateNewSection();
        if (!Section)
        {
            OutError = TEXT("Failed to create visibility section");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section);
        if (!BoolSection)
        {
            OutError = TEXT("Invalid visibility section");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        FMovieSceneBoolChannel& Channel = BoolSection->GetChannel();

        FFrameNumber MinFrame = TNumericLimits<int32>::Max();
        FFrameNumber MaxFrame = TNumericLimits<int32>::Lowest();

        for (const TSharedPtr<FJsonValue>& Value : KeysArray)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                OutError = TEXT("Invalid visibility key");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            TSharedPtr<FJsonObject> KeyObject = Value->AsObject();
            int32 Frame = 0;
            if (!ParseFrameNumber(KeyObject, TEXT("frame"), Frame))
            {
                OutError = TEXT("Visibility key missing frame");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            bool bVisible = false;
            if (!ParseBoolField(KeyObject, TEXT("visible"), bVisible))
            {
                OutError = TEXT("Visibility key missing visible value");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            const FFrameNumber KeyFrame = ConvertDisplayFrameToTick(MovieScene, Frame);
            MinFrame = FMath::Min(MinFrame, KeyFrame);
            MaxFrame = FMath::Max(MaxFrame, KeyFrame);
            Channel.AddKey(KeyFrame, bVisible);
        }

        const TRange<FFrameNumber> Range = TRange<FFrameNumber>::Inclusive(MinFrame, MaxFrame);
        BoolSection->SetRange(Range);
        Track->AddSection(*BoolSection);

        AppendAuditAction(AuditActions, TEXT("add_track"), {
            {TEXT("type"), TEXT("Visibility")},
            {TEXT("actor"), BindingInfo.ActorPath}
        });

        return true;
    }

    struct FPropertyKey
    {
        FFrameNumber Frame;
        bool bBoolValue = false;
        bool bHasBool = false;
        double NumericValue = 0.0;
        bool bHasNumeric = false;
        TOptional<FLinearColor> ColorValue;
        TOptional<int32> IntegerValue;
        TOptional<uint8> ByteValue;
    };

    enum class EPropertyTrackKind
    {
        Bool,
        Float,
        Integer,
        Byte,
        Color
    };

    struct FResolvedProperty
    {
        FString PropertyPath;
        FName PropertyName;
        EPropertyTrackKind Kind;
        FProperty* Property = nullptr;
    };

    bool ParsePropertyKeys(const UMovieScene& MovieScene, const EPropertyTrackKind Kind, const TArray<TSharedPtr<FJsonValue>>& KeysJson, TArray<FPropertyKey>& OutKeys, FString& OutError, FString& OutErrorCode)
    {
        OutKeys.Reset();
        for (const TSharedPtr<FJsonValue>& Value : KeysJson)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                OutError = TEXT("Invalid property key");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            TSharedPtr<FJsonObject> KeyObject = Value->AsObject();
            int32 Frame = 0;
            if (!ParseFrameNumber(KeyObject, TEXT("frame"), Frame))
            {
                OutError = TEXT("Property key missing frame");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            FPropertyKey Key;
            Key.Frame = ConvertDisplayFrameToTick(MovieScene, Frame);

            switch (Kind)
            {
                case EPropertyTrackKind::Bool:
                {
                    bool bValue = false;
                    if (!ParseBoolField(KeyObject, TEXT("value"), bValue))
                    {
                        OutError = TEXT("Property key missing bool value");
                        OutErrorCode = ErrorCodeInvalidParameters;
                        return false;
                    }
                    Key.bHasBool = true;
                    Key.bBoolValue = bValue;
                    break;
                }
                case EPropertyTrackKind::Float:
                {
                    if (!KeyObject->HasTypedField<EJson::Number>(TEXT("value")))
                    {
                        OutError = TEXT("Property key missing numeric value");
                        OutErrorCode = ErrorCodeInvalidParameters;
                        return false;
                    }
                    Key.bHasNumeric = true;
                    Key.NumericValue = KeyObject->GetNumberField(TEXT("value"));
                    break;
                }
                case EPropertyTrackKind::Integer:
                {
                    if (KeyObject->HasTypedField<EJson::Number>(TEXT("value")))
                    {
                        Key.IntegerValue = static_cast<int32>(KeyObject->GetNumberField(TEXT("value")));
                    }
                    else if (KeyObject->HasTypedField<EJson::String>(TEXT("value")))
                    {
                        int32 ParsedInt = 0;
                        if (!LexTryParseString(ParsedInt, *KeyObject->GetStringField(TEXT("value"))))
                        {
                            OutError = TEXT("Invalid integer value");
                            OutErrorCode = ErrorCodeInvalidParameters;
                            return false;
                        }
                        Key.IntegerValue = ParsedInt;
                    }
                    else
                    {
                        OutError = TEXT("Property key missing integer value");
                        OutErrorCode = ErrorCodeInvalidParameters;
                        return false;
                    }
                    break;
                }
                case EPropertyTrackKind::Byte:
                {
                    if (KeyObject->HasTypedField<EJson::Number>(TEXT("value")))
                    {
                        Key.ByteValue = static_cast<uint8>(FMath::Clamp<int32>(static_cast<int32>(KeyObject->GetNumberField(TEXT("value"))), 0, 255));
                    }
                    else if (KeyObject->HasTypedField<EJson::String>(TEXT("value")))
                    {
                        int32 ParsedInt = 0;
                        if (!LexTryParseString(ParsedInt, *KeyObject->GetStringField(TEXT("value"))))
                        {
                            OutError = TEXT("Invalid byte value");
                            OutErrorCode = ErrorCodeInvalidParameters;
                            return false;
                        }
                        Key.ByteValue = static_cast<uint8>(FMath::Clamp<int32>(ParsedInt, 0, 255));
                    }
                    else
                    {
                        OutError = TEXT("Property key missing byte value");
                        OutErrorCode = ErrorCodeInvalidParameters;
                        return false;
                    }
                    break;
                }
                case EPropertyTrackKind::Color:
                {
                    const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
                    if (!KeyObject->TryGetArrayField(TEXT("value"), ColorArray) || !ColorArray || (ColorArray->Num() != 3 && ColorArray->Num() != 4))
                    {
                        OutError = TEXT("Color value must be an array of 3 or 4 numbers");
                        OutErrorCode = ErrorCodeInvalidParameters;
                        return false;
                    }

                    FLinearColor Color(0.f, 0.f, 0.f, 1.f);
                    for (int32 Index = 0; Index < ColorArray->Num(); ++Index)
                    {
                        const TSharedPtr<FJsonValue>& ComponentValue = (*ColorArray)[Index];
                        if (!ComponentValue.IsValid() || ComponentValue->Type != EJson::Number)
                        {
                            OutError = TEXT("Color components must be numeric");
                            OutErrorCode = ErrorCodeInvalidParameters;
                            return false;
                        }

                        const double Component = ComponentValue->AsNumber();
                        switch (Index)
                        {
                            case 0: Color.R = Component; break;
                            case 1: Color.G = Component; break;
                            case 2: Color.B = Component; break;
                            case 3: Color.A = Component; break;
                            default: break;
                        }
                    }

                    Key.ColorValue = Color;
                    break;
                }
            }

            OutKeys.Add(Key);
        }

        return true;
    }

    FString ExtractPropertyName(const FString& PropertyPath)
    {
        FString Name = PropertyPath;
        int32 DotIndex = INDEX_NONE;
        if (Name.FindLastChar(TEXT('.'), DotIndex))
        {
            Name = Name.Mid(DotIndex + 1);
        }

        int32 BracketIndex = INDEX_NONE;
        if (Name.FindChar(TEXT('['), BracketIndex))
        {
            Name = Name.Left(BracketIndex);
        }

        return Name;
    }

    UObject* ResolveSampleObject(ULevelSequence& Sequence, const FBindingLookupResult& BindingInfo, UMovieScene& MovieScene)
    {
        if (BindingInfo.TemplateObject)
        {
            return BindingInfo.TemplateObject;
        }

        UWorld* World = GetEditorWorld();
        if (!World)
        {
            return nullptr;
        }

        TArray<UObject*, TInlineAllocator<1>> LocatedObjects;
        Sequence.LocateBoundObjects(BindingInfo.BindingId, World, LocatedObjects);
        if (LocatedObjects.Num() > 0)
        {
            return LocatedObjects[0];
        }

        if (const FMovieSceneSpawnable* Spawnable = MovieScene.FindSpawnable(BindingInfo.BindingId))
        {
            return Spawnable->GetObjectTemplate();
        }

        return nullptr;
    }

    bool ResolvePropertyInfo(ULevelSequence& Sequence, UMovieScene& MovieScene, const FBindingLookupResult& BindingInfo, const FString& PropertyPath, FResolvedProperty& OutProperty, FString& OutError)
    {
        UObject* SampleObject = ResolveSampleObject(Sequence, BindingInfo, MovieScene);
        if (!SampleObject)
        {
            OutError = TEXT("Unable to resolve bound object for property path");
            return false;
        }

        const FString PropertyNameString = ExtractPropertyName(PropertyPath);
        if (PropertyNameString.IsEmpty())
        {
            OutError = TEXT("Invalid property path");
            return false;
        }

        FTrackInstancePropertyBindings PropertyBindings(*PropertyNameString, PropertyPath);
        if (FProperty* ResolvedProperty = PropertyBindings.ResolveProperty(SampleObject))
        {
            OutProperty.PropertyPath = PropertyPath;
            OutProperty.PropertyName = FName(*PropertyNameString);
            OutProperty.Property = ResolvedProperty;

            if (CastField<FBoolProperty>(ResolvedProperty))
            {
                OutProperty.Kind = EPropertyTrackKind::Bool;
                return true;
            }

            if (const FByteProperty* ByteProp = CastField<FByteProperty>(ResolvedProperty))
            {
                if (ByteProp->IsEnum() || ByteProp->Enum)
                {
                    OutProperty.Kind = EPropertyTrackKind::Byte;
                }
                else
                {
                    OutProperty.Kind = EPropertyTrackKind::Integer;
                }
                return true;
            }

            if (CastField<FEnumProperty>(ResolvedProperty))
            {
                OutProperty.Kind = EPropertyTrackKind::Byte;
                return true;
            }

            if (CastField<FIntProperty>(ResolvedProperty) || CastField<FInt64Property>(ResolvedProperty) || CastField<FUInt32Property>(ResolvedProperty) || CastField<FUInt64Property>(ResolvedProperty))
            {
                OutProperty.Kind = EPropertyTrackKind::Integer;
                return true;
            }

            if (CastField<FFloatProperty>(ResolvedProperty) || CastField<FDoubleProperty>(ResolvedProperty))
            {
                OutProperty.Kind = EPropertyTrackKind::Float;
                return true;
            }

            if (const FStructProperty* StructProp = CastField<FStructProperty>(ResolvedProperty))
            {
                const UScriptStruct* Struct = StructProp->Struct;
                if (Struct && (Struct->GetFName() == NAME_Color || Struct->GetFName() == NAME_LinearColor))
                {
                    OutProperty.Kind = EPropertyTrackKind::Color;
                    return true;
                }
            }

            OutError = TEXT("Unsupported property type for track");
            return false;
        }

        OutError = TEXT("Property path could not be resolved");
        return false;
    }

    bool ApplyPropertyTrack(ULevelSequence& Sequence, UMovieScene& MovieScene, FMovieSceneBinding& Binding, const FBindingLookupResult& BindingInfo, const FString& PropertyPath, const TArray<TSharedPtr<FJsonValue>>& KeysArray, bool bOverwrite, FString& OutError, FString& OutErrorCode, TArray<TSharedPtr<FJsonValue>>& AuditActions)
    {
        if (KeysArray.Num() == 0)
        {
            OutError = TEXT("Property track requires keys");
            OutErrorCode = ErrorCodeInvalidParameters;
            return false;
        }

        FResolvedProperty ResolvedProperty;
        if (!ResolvePropertyInfo(Sequence, MovieScene, BindingInfo, PropertyPath, ResolvedProperty, OutError))
        {
            OutErrorCode = ErrorCodePropertyNotFound;
            return false;
        }

        TSubclassOf<UMovieScenePropertyTrack> TrackClass;
        switch (ResolvedProperty.Kind)
        {
            case EPropertyTrackKind::Bool: TrackClass = UMovieSceneBoolTrack::StaticClass(); break;
            case EPropertyTrackKind::Float: TrackClass = UMovieSceneFloatTrack::StaticClass(); break;
            case EPropertyTrackKind::Integer: TrackClass = UMovieSceneIntegerTrack::StaticClass(); break;
            case EPropertyTrackKind::Byte: TrackClass = UMovieSceneByteTrack::StaticClass(); break;
            case EPropertyTrackKind::Color: TrackClass = UMovieSceneColorTrack::StaticClass(); break;
            default:
                OutError = TEXT("Unsupported property type for track");
                OutErrorCode = ErrorCodePropertyUnsupported;
                return false;
        }

        FString ParseError;
        TArray<FPropertyKey> Keys;
        if (!ParsePropertyKeys(MovieScene, ResolvedProperty.Kind, KeysArray, Keys, ParseError, OutErrorCode))
        {
            OutError = ParseError;
            return false;
        }

        UMovieSceneTrack* ExistingTrack = FindPropertyTrack(Binding, ResolvedProperty.PropertyPath, TrackClass);
        if (ExistingTrack && !bOverwrite)
        {
            OutError = TEXT("Property track already exists");
            OutErrorCode = ErrorCodeTrackExists;
            return false;
        }

        UMovieScenePropertyTrack* PropertyTrack = nullptr;
        if (ExistingTrack)
        {
            PropertyTrack = Cast<UMovieScenePropertyTrack>(ExistingTrack);
        }
        else
        {
            PropertyTrack = Cast<UMovieScenePropertyTrack>(MovieScene.AddTrack(TrackClass, Binding.GetObjectGuid()));
        }

        if (!PropertyTrack)
        {
            OutError = TEXT("Failed to create property track");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        EnsureTrackForOverwrite(*PropertyTrack, bOverwrite);
        PropertyTrack->SetPropertyNameAndPath(ResolvedProperty.PropertyName, ResolvedProperty.PropertyPath);

        UMovieSceneSection* Section = PropertyTrack->CreateNewSection();
        if (!Section)
        {
            OutError = TEXT("Failed to create property section");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        FFrameNumber MinFrame = Keys[0].Frame;
        FFrameNumber MaxFrame = Keys[0].Frame;

        if (ResolvedProperty.Kind == EPropertyTrackKind::Bool)
        {
            UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section);
            if (!BoolSection)
            {
                OutError = TEXT("Invalid bool section");
                OutErrorCode = ErrorCodeTrackAddFailed;
                return false;
            }

            FMovieSceneBoolChannel& Channel = BoolSection->GetChannel();
            for (const FPropertyKey& Key : Keys)
            {
                MinFrame = FMath::Min(MinFrame, Key.Frame);
                MaxFrame = FMath::Max(MaxFrame, Key.Frame);
                Channel.AddKey(Key.Frame, Key.bBoolValue);
            }

            BoolSection->SetRange(TRange<FFrameNumber>::Inclusive(MinFrame, MaxFrame));
            PropertyTrack->AddSection(*BoolSection);
        }
        else if (ResolvedProperty.Kind == EPropertyTrackKind::Float)
        {
            UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
            if (!FloatSection)
            {
                OutError = TEXT("Invalid float section");
                OutErrorCode = ErrorCodeTrackAddFailed;
                return false;
            }

            FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
            for (const FPropertyKey& Key : Keys)
            {
                MinFrame = FMath::Min(MinFrame, Key.Frame);
                MaxFrame = FMath::Max(MaxFrame, Key.Frame);
                Channel.AddCubicKey(Key.Frame, static_cast<float>(Key.NumericValue));
            }

            FloatSection->SetRange(TRange<FFrameNumber>::Inclusive(MinFrame, MaxFrame));
            PropertyTrack->AddSection(*FloatSection);
        }
        else if (ResolvedProperty.Kind == EPropertyTrackKind::Integer)
        {
            UMovieSceneIntegerSection* IntegerSection = Cast<UMovieSceneIntegerSection>(Section);
            if (!IntegerSection)
            {
                OutError = TEXT("Invalid integer section");
                OutErrorCode = ErrorCodeTrackAddFailed;
                return false;
            }

            FMovieSceneIntegerChannel& Channel = IntegerSection->GetChannel();
            for (const FPropertyKey& Key : Keys)
            {
                if (!Key.IntegerValue.IsSet())
                {
                    OutError = TEXT("Missing integer value");
                    OutErrorCode = ErrorCodeInvalidParameters;
                    return false;
                }

                MinFrame = FMath::Min(MinFrame, Key.Frame);
                MaxFrame = FMath::Max(MaxFrame, Key.Frame);
                Channel.AddKey(Key.Frame, Key.IntegerValue.GetValue());
            }

            IntegerSection->SetRange(TRange<FFrameNumber>::Inclusive(MinFrame, MaxFrame));
            PropertyTrack->AddSection(*IntegerSection);
        }
        else if (ResolvedProperty.Kind == EPropertyTrackKind::Byte)
        {
            UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(Section);
            if (!ByteSection)
            {
                OutError = TEXT("Invalid byte section");
                OutErrorCode = ErrorCodeTrackAddFailed;
                return false;
            }

            FMovieSceneByteChannel& Channel = ByteSection->GetChannel();
            for (const FPropertyKey& Key : Keys)
            {
                if (!Key.ByteValue.IsSet())
                {
                    OutError = TEXT("Missing byte value");
                    OutErrorCode = ErrorCodeInvalidParameters;
                    return false;
                }

                MinFrame = FMath::Min(MinFrame, Key.Frame);
                MaxFrame = FMath::Max(MaxFrame, Key.Frame);
                Channel.AddKey(Key.Frame, Key.ByteValue.GetValue());
            }

            ByteSection->SetRange(TRange<FFrameNumber>::Inclusive(MinFrame, MaxFrame));
            PropertyTrack->AddSection(*ByteSection);
        }
        else if (ResolvedProperty.Kind == EPropertyTrackKind::Color)
        {
            UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section);
            if (!ColorSection)
            {
                OutError = TEXT("Invalid color section");
                OutErrorCode = ErrorCodeTrackAddFailed;
                return false;
            }

            FMovieSceneFloatChannel& R = ColorSection->GetRedChannel();
            FMovieSceneFloatChannel& G = ColorSection->GetGreenChannel();
            FMovieSceneFloatChannel& B = ColorSection->GetBlueChannel();
            FMovieSceneFloatChannel& A = ColorSection->GetAlphaChannel();

            for (const FPropertyKey& Key : Keys)
            {
                if (!Key.ColorValue.IsSet())
                {
                    OutError = TEXT("Missing color value");
                    OutErrorCode = ErrorCodeInvalidParameters;
                    return false;
                }

                const FLinearColor Color = Key.ColorValue.GetValue();
                MinFrame = FMath::Min(MinFrame, Key.Frame);
                MaxFrame = FMath::Max(MaxFrame, Key.Frame);
                R.AddCubicKey(Key.Frame, Color.R);
                G.AddCubicKey(Key.Frame, Color.G);
                B.AddCubicKey(Key.Frame, Color.B);
                A.AddCubicKey(Key.Frame, Color.A);
            }

            ColorSection->SetRange(TRange<FFrameNumber>::Inclusive(MinFrame, MaxFrame));
            PropertyTrack->AddSection(*ColorSection);
        }

        AppendAuditAction(AuditActions, TEXT("add_track"), {
            {TEXT("type"), TEXT("Property")},
            {TEXT("actor"), BindingInfo.ActorPath},
            {TEXT("property"), PropertyPath}
        });

        return true;
    }

    bool ApplyCameraCuts(UMovieScene& MovieScene, const TArray<TSharedPtr<FJsonValue>>& CameraCutsArray, bool bOverwrite, FString& OutError, FString& OutErrorCode, int32& OutAddedCuts, TArray<TSharedPtr<FJsonValue>>& AuditActions)
    {
        if (CameraCutsArray.Num() == 0)
        {
            return true;
        }

        UMovieSceneCameraCutTrack* CameraCutTrack = MovieScene.FindTrack<UMovieSceneCameraCutTrack>();
        if (!CameraCutTrack)
        {
            CameraCutTrack = MovieScene.AddTrack<UMovieSceneCameraCutTrack>();
        }

        if (!CameraCutTrack)
        {
            OutError = TEXT("Failed to create camera cut track");
            OutErrorCode = ErrorCodeTrackAddFailed;
            return false;
        }

        EnsureTrackForOverwrite(*CameraCutTrack, bOverwrite);

        if (bOverwrite)
        {
            const TArray<UMovieSceneSection*>& Sections = CameraCutTrack->GetAllSections();
            for (UMovieSceneSection* Section : Sections)
            {
                CameraCutTrack->RemoveSection(*Section);
            }
        }

        for (const TSharedPtr<FJsonValue>& Value : CameraCutsArray)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                OutError = TEXT("Invalid camera cut entry");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            TSharedPtr<FJsonObject> CutObject = Value->AsObject();

            int32 FrameStart = 0;
            if (!ParseFrameNumber(CutObject, TEXT("frameStart"), FrameStart))
            {
                OutError = TEXT("Camera cut missing frameStart");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            int32 FrameEnd = 0;
            if (!ParseFrameNumber(CutObject, TEXT("frameEnd"), FrameEnd))
            {
                OutError = TEXT("Camera cut missing frameEnd");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            FString CameraBindingIdString;
            if (!CutObject->TryGetStringField(TEXT("cameraBindingId"), CameraBindingIdString))
            {
                OutError = TEXT("Camera cut missing cameraBindingId");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            FGuid CameraBindingId;
            if (!FGuid::Parse(CameraBindingIdString, CameraBindingId))
            {
                OutError = TEXT("Invalid cameraBindingId GUID");
                OutErrorCode = ErrorCodeInvalidParameters;
                return false;
            }

            const FFrameNumber StartFrame = ConvertDisplayFrameToTick(MovieScene, FrameStart);
            const FFrameNumber EndFrameExclusive = ConvertDisplayFrameToTick(MovieScene, FrameEnd + 1);

            UMovieSceneCameraCutSection* CutSection = Cast<UMovieSceneCameraCutSection>(CameraCutTrack->CreateNewSection());
            if (!CutSection)
            {
                OutError = TEXT("Failed to create camera cut section");
                OutErrorCode = ErrorCodeTrackAddFailed;
                return false;
            }

            CutSection->SetRange(TRange<FFrameNumber>(StartFrame, EndFrameExclusive));
            CutSection->SetCameraBindingID(FMovieSceneObjectBindingID(CameraBindingId));
            CameraCutTrack->AddSection(*CutSection);

            AppendAuditAction(AuditActions, TEXT("add_camera_cut"), {
                {TEXT("from"), LexToString(FrameStart)},
                {TEXT("to"), LexToString(FrameEnd)},
                {TEXT("camera"), CameraBindingIdString.ToUpper()}
            });

            ++OutAddedCuts;
        }

        return true;
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

TSharedPtr<FJsonObject> FSequenceTracks::AddTracks(const TSharedPtr<FJsonObject>& Params)
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

    const FString PackagePath = FPackageName::ObjectPathToPackageName(SequenceObjectPath);

    FString PathReason;
    if (!FWriteGate::IsPathAllowed(PackagePath, PathReason))
    {
        return MakeErrorResponse(TEXT("PATH_NOT_ALLOWED"), PathReason);
    }

    if (!FWriteGate::IsWriteAllowed())
    {
        return MakeErrorResponse(TEXT("WRITE_NOT_ALLOWED"), TEXT("Write operations are currently disabled"));
    }

    TSharedPtr<FJsonObject> CheckoutError;
    if (!FWriteGate::EnsureCheckoutForContentPath(PackagePath, CheckoutError))
    {
        if (CheckoutError.IsValid())
        {
            return CheckoutError;
        }

        return MakeErrorResponse(TEXT("SOURCE_CONTROL_REQUIRED"), TEXT("Unable to checkout sequence"));
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

    const bool bOverwrite = Params->HasTypedField<EJson::Boolean>(TEXT("overwrite")) && Params->GetBoolField(TEXT("overwrite"));
    const bool bSave = Params->HasTypedField<EJson::Boolean>(TEXT("save")) && Params->GetBoolField(TEXT("save"));

    const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
    Params->TryGetArrayField(TEXT("bindings"), BindingsArray);

    const TArray<TSharedPtr<FJsonValue>>* CameraCutsArray = nullptr;
    Params->TryGetArrayField(TEXT("cameraCuts"), CameraCutsArray);

    if ((!BindingsArray || BindingsArray->Num() == 0) && (!CameraCutsArray || CameraCutsArray->Num() == 0))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Nothing to add"));
    }

    LevelSequence->Modify();
    MovieScene->Modify();

    int32 TracksAdded = 0;
    int32 CameraCutsAdded = 0;
    bool bModified = false;

    TArray<TSharedPtr<FJsonValue>> AuditActions;

    if (BindingsArray)
    {
        for (const TSharedPtr<FJsonValue>& BindingValue : *BindingsArray)
        {
            if (!BindingValue.IsValid() || BindingValue->Type != EJson::Object)
            {
                return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Invalid binding entry"));
            }

            TSharedPtr<FJsonObject> BindingObject = BindingValue->AsObject();
            FString ActorPath;
            if (!BindingObject->TryGetStringField(TEXT("actorPath"), ActorPath))
            {
                return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Binding entry missing actorPath"));
            }

            FBindingLookupResult BindingInfo;
            if (!FindBindingForActorPath(*LevelSequence, *MovieScene, ActorPath, BindingInfo))
            {
                return MakeErrorResponse(ErrorCodeActorNotBound, FString::Printf(TEXT("Actor not bound: %s"), *ActorPath));
            }

            FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingInfo.BindingId);
            if (!Binding)
            {
                return MakeErrorResponse(ErrorCodeTrackAddFailed, TEXT("Failed to resolve binding"));
            }

            const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
            if (!BindingObject->TryGetArrayField(TEXT("tracks"), TracksArray) || !TracksArray)
            {
                return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Binding entry missing tracks array"));
            }

            for (const TSharedPtr<FJsonValue>& TrackValue : *TracksArray)
            {
                if (!TrackValue.IsValid() || TrackValue->Type != EJson::Object)
                {
                    return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Invalid track descriptor"));
                }

                TSharedPtr<FJsonObject> TrackObject = TrackValue->AsObject();
                FString TrackType;
                if (!TrackObject->TryGetStringField(TEXT("type"), TrackType))
                {
                    return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Track descriptor missing type"));
                }

                const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
                TrackObject->TryGetArrayField(TEXT("keys"), KeysArray);
                if (!KeysArray)
                {
                    return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Track descriptor missing keys"));
                }

                FString TrackError;
                FString TrackErrorCode;
                const FString TrackTypeNormalized = TrackType.ToLower();

                if (TrackTypeNormalized == TEXT("transform"))
                {
                    if (!ApplyTransformTrack(*LevelSequence, *MovieScene, *Binding, BindingInfo, *KeysArray, bOverwrite, TrackError, TrackErrorCode, AuditActions))
                    {
                        return MakeErrorResponse(TrackErrorCode.IsEmpty() ? ErrorCodeTrackAddFailed : TrackErrorCode, TrackError);
                    }

                    ++TracksAdded;
                    bModified = true;
                }
                else if (TrackTypeNormalized == TEXT("visibility"))
                {
                    if (!ApplyVisibilityTrack(*LevelSequence, *MovieScene, *Binding, BindingInfo, *KeysArray, bOverwrite, TrackError, TrackErrorCode, AuditActions))
                    {
                        return MakeErrorResponse(TrackErrorCode.IsEmpty() ? ErrorCodeTrackAddFailed : TrackErrorCode, TrackError);
                    }

                    ++TracksAdded;
                    bModified = true;
                }
                else if (TrackTypeNormalized == TEXT("property"))
                {
                    FString PropertyPath;
                    if (!TrackObject->TryGetStringField(TEXT("propertyPath"), PropertyPath))
                    {
                        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Property track missing propertyPath"));
                    }

                    if (!ApplyPropertyTrack(*LevelSequence, *MovieScene, *Binding, BindingInfo, PropertyPath, *KeysArray, bOverwrite, TrackError, TrackErrorCode, AuditActions))
                    {
                        return MakeErrorResponse(TrackErrorCode.IsEmpty() ? ErrorCodeTrackAddFailed : TrackErrorCode, TrackError);
                    }

                    ++TracksAdded;
                    bModified = true;
                }
                else
                {
                    return MakeErrorResponse(ErrorCodeInvalidParameters, FString::Printf(TEXT("Unsupported track type: %s"), *TrackType));
                }
            }
        }
    }

    if (CameraCutsArray && CameraCutsArray->Num() > 0)
    {
        FString CameraCutError;
        FString CameraCutErrorCode;
        if (!ApplyCameraCuts(*MovieScene, *CameraCutsArray, bOverwrite, CameraCutError, CameraCutErrorCode, CameraCutsAdded, AuditActions))
        {
            return MakeErrorResponse(CameraCutErrorCode.IsEmpty() ? ErrorCodeTrackAddFailed : CameraCutErrorCode, CameraCutError);
        }

        if (CameraCutsAdded > 0)
        {
            bModified = true;
        }
    }

    if (bModified)
    {
        LevelSequence->MarkPackageDirty();
        if (!SaveSequenceIfRequested(*LevelSequence, bSave))
        {
            return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save sequence"));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetNumberField(TEXT("tracksAdded"), TracksAdded);
    Data->SetNumberField(TEXT("cameraCutsAdded"), CameraCutsAdded);

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), false);
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return MakeSuccessResponse(Data);
}
