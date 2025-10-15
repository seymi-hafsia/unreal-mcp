#include "Sequencer/SequenceExport.h"
#include "CoreMinimal.h"

#include "Algo/Sort.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelSequence/LevelSequence.h"
#if __has_include("String/LexFromString.h")
#include "String/LexFromString.h"
#else
#include "Misc/LexFromString.h"
#endif
#if __has_include("String/LexToString.h")
#include "String/LexToString.h"
#else
#include "Misc/LexToString.h"
#endif
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSpawnable.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneIntegerSection.h"
#include "Sections/MovieSceneSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParameters = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodeSequenceNotFound = TEXT("SEQUENCE_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeUnsupportedFormat = TEXT("UNSUPPORTED_FORMAT");

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

    bool ParseIntField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, int32& OutValue)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        if (Object->HasTypedField<EJson::Number>(FieldName))
        {
            OutValue = static_cast<int32>(Object->GetNumberField(FieldName));
            return true;
        }

        if (Object->HasTypedField<EJson::String>(FieldName))
        {
            return LexTryParseString(OutValue, *Object->GetStringField(FieldName));
        }

        return false;
    }

    bool ParseBoolField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, bool& OutValue)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        if (Object->HasTypedField<EJson::Boolean>(FieldName))
        {
            OutValue = Object->GetBoolField(FieldName);
            return true;
        }

        if (Object->HasTypedField<EJson::String>(FieldName))
        {
            const FString Value = Object->GetStringField(FieldName);
            if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1"))
            {
                OutValue = true;
                return true;
            }
            if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value == TEXT("0"))
            {
                OutValue = false;
                return true;
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

    int32 ConvertTickFrameToDisplay(const UMovieScene& MovieScene, const FFrameNumber& TickFrame)
    {
        const FFrameRate DisplayRate = MovieScene.GetDisplayRate();
        const FFrameRate TickResolution = MovieScene.GetTickResolution();
        const FFrameTime Converted = FFrameRate::TransformTime(TickFrame, TickResolution, DisplayRate);
        return Converted.RoundToFrame().Value;
    }

    struct FFrameRangeFilter
    {
        TOptional<int32> DisplayStart;
        TOptional<int32> DisplayEnd;
        TOptional<FFrameNumber> TickStart;
        TOptional<FFrameNumber> TickEnd;

        bool ContainsTick(const FFrameNumber& Tick) const
        {
            if (TickStart.IsSet() && Tick < TickStart.GetValue())
            {
                return false;
            }
            if (TickEnd.IsSet() && Tick > TickEnd.GetValue())
            {
                return false;
            }
            return true;
        }

        bool ContainsDisplay(int32 Frame) const
        {
            if (DisplayStart.IsSet() && Frame < DisplayStart.GetValue())
            {
                return false;
            }
            if (DisplayEnd.IsSet() && Frame > DisplayEnd.GetValue())
            {
                return false;
            }
            return true;
        }
    };

    bool ParseFrameRange(const UMovieScene& MovieScene, const TSharedPtr<FJsonObject>& Params, FFrameRangeFilter& OutFilter, FString& OutError)
    {
        const TSharedPtr<FJsonObject>* FrameRangeObject = nullptr;
        if (!Params->TryGetObjectField(TEXT("frameRange"), FrameRangeObject) || !FrameRangeObject || !FrameRangeObject->IsValid())
        {
            return true;
        }

        bool bHasStart = false;
        bool bHasEnd = false;
        int32 StartFrame = 0;
        int32 EndFrame = 0;

        if ((*FrameRangeObject)->HasField(TEXT("start")))
        {
            if (!ParseIntField(*FrameRangeObject, TEXT("start"), StartFrame))
            {
                OutError = TEXT("frameRange.start must be an integer");
                return false;
            }
            bHasStart = true;
        }

        if ((*FrameRangeObject)->HasField(TEXT("end")))
        {
            if (!ParseIntField(*FrameRangeObject, TEXT("end"), EndFrame))
            {
                OutError = TEXT("frameRange.end must be an integer");
                return false;
            }
            bHasEnd = true;
        }

        if (bHasStart && bHasEnd && EndFrame < StartFrame)
        {
            OutError = TEXT("frameRange.end must be greater than or equal to frameRange.start");
            return false;
        }

        if (bHasStart)
        {
            OutFilter.DisplayStart = StartFrame;
            OutFilter.TickStart = ConvertDisplayFrameToTick(MovieScene, StartFrame);
        }

        if (bHasEnd)
        {
            OutFilter.DisplayEnd = EndFrame;
            OutFilter.TickEnd = ConvertDisplayFrameToTick(MovieScene, EndFrame);
        }

        return true;
    }

    FString CsvEscape(const FString& Value)
    {
        FString Result = Value;
        Result.ReplaceInline(TEXT("\""), TEXT(""""));

        const bool bNeedsQuotes = Result.Contains(TEXT(",")) || Result.Contains(TEXT("\n")) || Result.Contains(TEXT("\""));
        if (bNeedsQuotes)
        {
            return FString::Printf(TEXT("\"%s\""), *Result);
        }

        return Result;
    }

    FString OptionalIntToString(const TOptional<int32>& Value)
    {
        return Value.IsSet() ? FString::FromInt(Value.GetValue()) : FString();
    }

    enum class EExportFormat
    {
        Json,
        Csv
    };

    struct FIncludeSettings
    {
        bool bBindings = true;
        bool bIncludeKeys = true;
        bool bTransform = true;
        bool bVisibility = true;
        bool bProperty = true;
        bool bCameraCut = true;
    };

    void ApplyTrackFilters(const TSharedPtr<FJsonObject>& IncludeObject, FIncludeSettings& InOutSettings)
    {
        if (!IncludeObject.IsValid())
        {
            return;
        }

        bool bBindings = true;
        if (IncludeObject->HasField(TEXT("bindings")))
        {
            IncludeObject->TryGetBoolField(TEXT("bindings"), bBindings);
        }
        InOutSettings.bBindings = bBindings;

        bool bKeys = true;
        if (IncludeObject->HasField(TEXT("keys")))
        {
            IncludeObject->TryGetBoolField(TEXT("keys"), bKeys);
        }
        InOutSettings.bIncludeKeys = bKeys;

        const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
        if (IncludeObject->TryGetArrayField(TEXT("tracks"), TracksArray) && TracksArray && TracksArray->Num() > 0)
        {
            bool bIncludeAll = false;
            TSet<FString> Normalized;
            for (const TSharedPtr<FJsonValue>& Value : *TracksArray)
            {
                if (!Value.IsValid() || Value->Type != EJson::String)
                {
                    continue;
                }

                FString TrackName = Value->AsString();
                TrackName.TrimStartAndEndInline();
                if (TrackName.IsEmpty())
                {
                    continue;
                }

                if (TrackName.Equals(TEXT("All"), ESearchCase::IgnoreCase))
                {
                    bIncludeAll = true;
                    break;
                }

                Normalized.Add(TrackName.ToLower());
            }

            if (!bIncludeAll)
            {
                InOutSettings.bTransform = Normalized.Contains(TEXT("transform"));
                InOutSettings.bVisibility = Normalized.Contains(TEXT("visibility"));
                InOutSettings.bProperty = Normalized.Contains(TEXT("property"));
                InOutSettings.bCameraCut = Normalized.Contains(TEXT("cameracut"));
            }
        }
    }

    void AppendRangeArray(const UMovieScene& MovieScene, const TRange<FFrameNumber>& Range, const FFrameRangeFilter& Filter, TSharedPtr<FJsonObject>& SectionJson)
    {
        if (!SectionJson.IsValid())
        {
            return;
        }

        if (!Range.HasLowerBound() && !Range.HasUpperBound())
        {
            return;
        }

        FFrameNumber StartTick = Range.HasLowerBound() ? Range.GetLowerBoundValue() : FFrameNumber(0);
        FFrameNumber EndTick = Range.HasUpperBound() ? Range.GetUpperBoundValue() : StartTick;

        if (Range.GetUpperBound().IsOpen())
        {
            EndTick = StartTick;
        }
        else if (Range.GetUpperBound().IsExclusive())
        {
            EndTick = EndTick - 1;
        }

        if (Filter.TickStart.IsSet())
        {
            StartTick = FMath::Max(StartTick, Filter.TickStart.GetValue());
        }
        if (Filter.TickEnd.IsSet())
        {
            EndTick = FMath::Min(EndTick, Filter.TickEnd.GetValue());
        }

        if (EndTick < StartTick)
        {
            return;
        }

        const int32 StartDisplay = ConvertTickFrameToDisplay(MovieScene, StartTick);
        const int32 EndDisplay = ConvertTickFrameToDisplay(MovieScene, EndTick);

        TArray<TSharedPtr<FJsonValue>> RangeArray;
        RangeArray.Add(MakeShared<FJsonValueNumber>(StartDisplay));
        RangeArray.Add(MakeShared<FJsonValueNumber>(EndDisplay));
        SectionJson->SetArrayField(TEXT("range"), RangeArray);
    }

    void AppendVectorField(const FString& FieldName, const FVector& Value, TSharedPtr<FJsonObject>& JsonObject)
    {
        if (!JsonObject.IsValid())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Components;
        Components.Add(MakeShared<FJsonValueNumber>(Value.X));
        Components.Add(MakeShared<FJsonValueNumber>(Value.Y));
        Components.Add(MakeShared<FJsonValueNumber>(Value.Z));
        JsonObject->SetArrayField(FieldName, Components);
    }

    void AppendColorField(const FString& FieldName, const FLinearColor& Color, TSharedPtr<FJsonObject>& JsonObject)
    {
        if (!JsonObject.IsValid())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Components;
        Components.Add(MakeShared<FJsonValueNumber>(Color.R));
        Components.Add(MakeShared<FJsonValueNumber>(Color.G));
        Components.Add(MakeShared<FJsonValueNumber>(Color.B));
        Components.Add(MakeShared<FJsonValueNumber>(Color.A));
        JsonObject->SetArrayField(FieldName, Components);
    }

    void AddCsvRow(TArray<FString>& Lines,
                   const FString& BindingId,
                   const FString& Label,
                   const FString& TrackType,
                   const TOptional<int32>& SectionStart,
                   const TOptional<int32>& SectionEnd,
                   const TOptional<int32>& Frame,
                   const FString& Key,
                   const FString& Property,
                   const FString& Value,
                   const TOptional<FVector>& VectorValue,
                   const TOptional<FLinearColor>& ColorValue)
    {
        TArray<FString> Columns;
        Columns.Add(CsvEscape(BindingId));
        Columns.Add(CsvEscape(Label));
        Columns.Add(CsvEscape(TrackType));
        Columns.Add(CsvEscape(OptionalIntToString(SectionStart)));
        Columns.Add(CsvEscape(OptionalIntToString(SectionEnd)));
        Columns.Add(CsvEscape(OptionalIntToString(Frame)));
        Columns.Add(CsvEscape(Key));
        Columns.Add(CsvEscape(Property));
        Columns.Add(CsvEscape(Value));

        if (VectorValue.IsSet())
        {
            Columns.Add(CsvEscape(LexToString(VectorValue->X)));
            Columns.Add(CsvEscape(LexToString(VectorValue->Y)));
            Columns.Add(CsvEscape(LexToString(VectorValue->Z)));
        }
        else
        {
            Columns.Add(TEXT(""));
            Columns.Add(TEXT(""));
            Columns.Add(TEXT(""));
        }

        if (ColorValue.IsSet())
        {
            Columns.Add(CsvEscape(LexToString(ColorValue->R)));
            Columns.Add(CsvEscape(LexToString(ColorValue->G)));
            Columns.Add(CsvEscape(LexToString(ColorValue->B)));
            Columns.Add(CsvEscape(LexToString(ColorValue->A)));
        }
        else
        {
            Columns.Add(TEXT(""));
            Columns.Add(TEXT(""));
            Columns.Add(TEXT(""));
            Columns.Add(TEXT(""));
        }

        Lines.Add(FString::Join(Columns, TEXT(",")));
    }

    FString GetBindingClassName(const UMovieScene& MovieScene, const FMovieSceneBinding& Binding)
    {
        const FGuid& Guid = Binding.GetObjectGuid();
        if (const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid))
        {
            return Possessable->GetPossessedObjectClassName();
        }

        if (const FMovieSceneSpawnable* Spawnable = MovieScene.FindSpawnable(Guid))
        {
            if (UObject* Template = Spawnable->GetObjectTemplate())
            {
                if (UClass* TemplateClass = Template->GetClass())
                {
                    return TemplateClass->GetName();
                }
            }
        }

        return FString();
    }

    FString ToNormalizedBindingGuid(const FGuid& Guid)
    {
        return Guid.ToString(EGuidFormats::DigitsWithHyphens).ToUpper();
    }
}

TSharedPtr<FJsonObject> FSequenceExport::Export(const TSharedPtr<FJsonObject>& Params)
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

    FString FormatString = TEXT("json");
    if (Params->HasField(TEXT("format")))
    {
        Params->TryGetStringField(TEXT("format"), FormatString);
    }
    FormatString.TrimStartAndEndInline();

    EExportFormat ExportFormat = EExportFormat::Json;
    if (FormatString.Equals(TEXT("json"), ESearchCase::IgnoreCase))
    {
        ExportFormat = EExportFormat::Json;
    }
    else if (FormatString.Equals(TEXT("csv"), ESearchCase::IgnoreCase))
    {
        ExportFormat = EExportFormat::Csv;
    }
    else
    {
        return MakeErrorResponse(ErrorCodeUnsupportedFormat, FString::Printf(TEXT("Unsupported format: %s"), *FormatString));
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

    FIncludeSettings IncludeSettings;
    const TSharedPtr<FJsonObject>* IncludeObject = nullptr;
    if (Params->TryGetObjectField(TEXT("include"), IncludeObject))
    {
        ApplyTrackFilters(*IncludeObject, IncludeSettings);
    }

    const bool bResolveActorPaths = Params->HasTypedField<EJson::Boolean>(TEXT("worldActorPaths")) && Params->GetBoolField(TEXT("worldActorPaths"));
    const bool bFlattenProperties = Params->HasTypedField<EJson::Boolean>(TEXT("flattenProperties")) && Params->GetBoolField(TEXT("flattenProperties"));

    FFrameRangeFilter FrameFilter;
    FString FrameRangeError;
    if (!ParseFrameRange(*MovieScene, Params, FrameFilter, FrameRangeError))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, FrameRangeError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);

    TSharedPtr<FJsonObject> SequenceJson = MakeShared<FJsonObject>();
    SequenceJson->SetStringField(TEXT("assetPath"), SequenceObjectPath);

    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickResolution = MovieScene->GetTickResolution();

    {
        TArray<TSharedPtr<FJsonValue>> DisplayRateArray;
        DisplayRateArray.Add(MakeShared<FJsonValueNumber>(DisplayRate.Numerator));
        DisplayRateArray.Add(MakeShared<FJsonValueNumber>(DisplayRate.Denominator));
        SequenceJson->SetArrayField(TEXT("displayRate"), DisplayRateArray);
    }

    {
        TArray<TSharedPtr<FJsonValue>> TickArray;
        TickArray.Add(MakeShared<FJsonValueNumber>(TickResolution.Numerator));
        TickArray.Add(MakeShared<FJsonValueNumber>(TickResolution.Denominator));
        SequenceJson->SetArrayField(TEXT("tickResolution"), TickArray);
    }

    const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
    if (PlaybackRange.HasLowerBound() && PlaybackRange.HasUpperBound())
    {
        FFrameNumber RangeEnd = PlaybackRange.GetUpperBound().IsExclusive() ? PlaybackRange.GetUpperBoundValue() - 1 : PlaybackRange.GetUpperBoundValue();
        const int32 DurationDisplay = ConvertTickFrameToDisplay(*MovieScene, RangeEnd) - ConvertTickFrameToDisplay(*MovieScene, PlaybackRange.GetLowerBoundValue()) + 1;
        SequenceJson->SetNumberField(TEXT("durationFrames"), DurationDisplay);
    }
    else
    {
        SequenceJson->SetNumberField(TEXT("durationFrames"), 0);
    }

    Data->SetObjectField(TEXT("sequence"), SequenceJson);

    UWorld* World = bResolveActorPaths ? GetEditorWorld() : nullptr;

    TArray<TSharedPtr<FJsonValue>> BindingsArray;

    TArray<FString> CsvLines;
    if (ExportFormat == EExportFormat::Csv)
    {
        CsvLines.Add(TEXT("bindingId,label,trackType,sectionStart,sectionEnd,frame,key,property,value,x,y,z,r,g,b,a"));
    }

    for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
    {
        const FGuid& BindingGuid = Binding.GetObjectGuid();
        const FString BindingId = ToNormalizedBindingGuid(BindingGuid);
        const FString BindingLabel = MovieScene->GetObjectDisplayName(BindingGuid).ToString();
        const FString ClassName = GetBindingClassName(*MovieScene, Binding);

        FString ResolvedActorPath;
        if (World)
        {
            TArray<UObject*, TInlineAllocator<1>> LocatedObjects;
            LevelSequence->LocateBoundObjects(BindingGuid, World, LocatedObjects);
            for (UObject* Located : LocatedObjects)
            {
                if (AActor* Actor = Cast<AActor>(Located))
                {
                    ResolvedActorPath = Actor->GetPathName();
                    break;
                }
            }
        }

        TSharedPtr<FJsonObject> BindingJson;
        if (IncludeSettings.bBindings)
        {
            BindingJson = MakeShared<FJsonObject>();
            BindingJson->SetStringField(TEXT("bindingId"), BindingId);
            BindingJson->SetStringField(TEXT("label"), BindingLabel);
            if (!ClassName.IsEmpty())
            {
                BindingJson->SetStringField(TEXT("class"), ClassName);
            }
            if (!ResolvedActorPath.IsEmpty())
            {
                BindingJson->SetStringField(TEXT("resolvedActorPath"), ResolvedActorPath);
            }
            else if (bResolveActorPaths)
            {
                BindingJson->SetField(TEXT("resolvedActorPath"), MakeShared<FJsonValueNull>());
            }
        }

        TArray<TSharedPtr<FJsonValue>> TrackArray;

        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (!Track)
            {
                continue;
            }

            if (IncludeSettings.bTransform)
            {
                if (UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Track))
                {
                    TArray<TSharedPtr<FJsonValue>> TransformSections;

                    for (UMovieSceneSection* Section : TransformTrack->GetAllSections())
                    {
                        UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(Section);
                        if (!TransformSection)
                        {
                            continue;
                        }

                        const TRange<FFrameNumber> SectionRange = TransformSection->GetRange();

                        FFrameNumber SectionStartTick = SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : FFrameNumber(0);
                        FFrameNumber SectionEndTick = SectionRange.HasUpperBound() ? SectionRange.GetUpperBoundValue() : SectionStartTick;
                        if (SectionRange.GetUpperBound().IsExclusive())
                        {
                            SectionEndTick = SectionEndTick - 1;
                        }

                        if (FrameFilter.TickStart.IsSet())
                        {
                            SectionStartTick = FMath::Max(SectionStartTick, FrameFilter.TickStart.GetValue());
                        }
                        if (FrameFilter.TickEnd.IsSet())
                        {
                            SectionEndTick = FMath::Min(SectionEndTick, FrameFilter.TickEnd.GetValue());
                        }

                        if (SectionEndTick < SectionStartTick)
                        {
                            continue;
                        }

                        const int32 SectionStartDisplay = ConvertTickFrameToDisplay(*MovieScene, SectionStartTick);
                        const int32 SectionEndDisplay = ConvertTickFrameToDisplay(*MovieScene, SectionEndTick);

                        TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
                        AppendRangeArray(*MovieScene, SectionRange, FrameFilter, SectionJson);

                        if (IncludeSettings.bIncludeKeys)
                        {
                            TSet<int32> KeyFramesTick;

                            auto AccumulateFrames = [&KeyFramesTick, &FrameFilter](FMovieSceneFloatChannel& Channel)
                            {
                                const FMovieSceneChannelData<float> ChannelData = Channel.GetData();
                                TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
                                for (const FFrameNumber& Time : Times)
                                {
                                    const int32 TickValue = Time.Value;
                                    if (FrameFilter.TickStart.IsSet() && TickValue < FrameFilter.TickStart.GetValue().Value)
                                    {
                                        continue;
                                    }
                                    if (FrameFilter.TickEnd.IsSet() && TickValue > FrameFilter.TickEnd.GetValue().Value)
                                    {
                                        continue;
                                    }
                                    KeyFramesTick.Add(TickValue);
                                }
                            };

                            AccumulateFrames(TransformSection->GetTranslationChannel(0));
                            AccumulateFrames(TransformSection->GetTranslationChannel(1));
                            AccumulateFrames(TransformSection->GetTranslationChannel(2));
                            AccumulateFrames(TransformSection->GetRotationChannel(0));
                            AccumulateFrames(TransformSection->GetRotationChannel(1));
                            AccumulateFrames(TransformSection->GetRotationChannel(2));
                            AccumulateFrames(TransformSection->GetScaleChannel(0));
                            AccumulateFrames(TransformSection->GetScaleChannel(1));
                            AccumulateFrames(TransformSection->GetScaleChannel(2));

                            TArray<int32> SortedTicks = KeyFramesTick.Array();
                            SortedTicks.Sort();

                            TArray<TSharedPtr<FJsonValue>> KeysArray;

                            for (int32 TickValue : SortedTicks)
                            {
                                const FFrameNumber TickFrame(TickValue);
                                if (!FrameFilter.ContainsTick(TickFrame))
                                {
                                    continue;
                                }

                                const int32 DisplayFrame = ConvertTickFrameToDisplay(*MovieScene, TickFrame);
                                if (!FrameFilter.ContainsDisplay(DisplayFrame))
                                {
                                    continue;
                                }

                                TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                                KeyJson->SetNumberField(TEXT("frame"), DisplayFrame);

                                FVector Location = FVector::ZeroVector;
                                FVector Rotation = FVector::ZeroVector;
                                FVector Scale = FVector(1.0f, 1.0f, 1.0f);
                                bool bHasLocation = false;
                                bool bHasRotation = false;
                                bool bHasScale = false;

                                auto EvaluateVector = [&TickFrame](FMovieSceneFloatChannel& X, FMovieSceneFloatChannel& Y, FMovieSceneFloatChannel& Z, FVector& OutVector, bool& bOutHasValue)
                                {
                                    double VX = 0.0;
                                    double VY = 0.0;
                                    double VZ = 0.0;
                                    const bool bHasX = X.Evaluate(TickFrame, VX);
                                    const bool bHasY = Y.Evaluate(TickFrame, VY);
                                    const bool bHasZ = Z.Evaluate(TickFrame, VZ);
                                    if (bHasX || bHasY || bHasZ)
                                    {
                                        OutVector = FVector(static_cast<float>(VX), static_cast<float>(VY), static_cast<float>(VZ));
                                        bOutHasValue = true;
                                    }
                                };

                                EvaluateVector(TransformSection->GetTranslationChannel(0), TransformSection->GetTranslationChannel(1), TransformSection->GetTranslationChannel(2), Location, bHasLocation);
                                EvaluateVector(TransformSection->GetRotationChannel(0), TransformSection->GetRotationChannel(1), TransformSection->GetRotationChannel(2), Rotation, bHasRotation);
                                EvaluateVector(TransformSection->GetScaleChannel(0), TransformSection->GetScaleChannel(1), TransformSection->GetScaleChannel(2), Scale, bHasScale);

                                if (bHasLocation)
                                {
                                    AppendVectorField(TEXT("location"), Location, KeyJson);
                                }
                                if (bHasRotation)
                                {
                                    AppendVectorField(TEXT("rotation"), Rotation, KeyJson);
                                }
                                if (bHasScale)
                                {
                                    AppendVectorField(TEXT("scale"), Scale, KeyJson);
                                }

                                KeysArray.Add(MakeShared<FJsonValueObject>(KeyJson));

                                if (ExportFormat == EExportFormat::Csv)
                                {
                                    if (bHasLocation)
                                    {
                                        AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Transform"), SectionStartDisplay, SectionEndDisplay, DisplayFrame, TEXT("location"), FString(), FString(), Location, TOptional<FLinearColor>());
                                    }
                                    if (bHasRotation)
                                    {
                                        AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Transform"), SectionStartDisplay, SectionEndDisplay, DisplayFrame, TEXT("rotation"), FString(), FString(), Rotation, TOptional<FLinearColor>());
                                    }
                                    if (bHasScale)
                                    {
                                        AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Transform"), SectionStartDisplay, SectionEndDisplay, DisplayFrame, TEXT("scale"), FString(), FString(), Scale, TOptional<FLinearColor>());
                                    }
                                }
                            }

                            if (KeysArray.Num() > 0)
                            {
                                SectionJson->SetArrayField(TEXT("keys"), KeysArray);
                            }
                            else if (IncludeSettings.bIncludeKeys)
                            {
                                SectionJson->SetArrayField(TEXT("keys"), {});
                            }
                        }

                        if (IncludeSettings.bBindings)
                        {
                            if (!SectionJson->HasField(TEXT("keys")) && IncludeSettings.bIncludeKeys)
                            {
                                SectionJson->SetArrayField(TEXT("keys"), {});
                            }

                            TransformSections.Add(MakeShared<FJsonValueObject>(SectionJson));
                        }
                    }

                    if (IncludeSettings.bBindings && TransformSections.Num() > 0)
                    {
                        TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
                        TrackJson->SetStringField(TEXT("type"), TEXT("Transform"));
                        TrackJson->SetArrayField(TEXT("sections"), TransformSections);
                        TrackArray.Add(MakeShared<FJsonValueObject>(TrackJson));
                    }
                }
            }

            if (IncludeSettings.bVisibility)
            {
                if (UMovieSceneVisibilityTrack* VisibilityTrack = Cast<UMovieSceneVisibilityTrack>(Track))
                {
                    TArray<TSharedPtr<FJsonValue>> VisibilitySections;

                    for (UMovieSceneSection* Section : VisibilityTrack->GetAllSections())
                    {
                        UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section);
                        if (!BoolSection)
                        {
                            continue;
                        }

                        const TRange<FFrameNumber> SectionRange = BoolSection->GetRange();
                        TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
                        AppendRangeArray(*MovieScene, SectionRange, FrameFilter, SectionJson);

                        FMovieSceneBoolChannel& Channel = BoolSection->GetChannel();
                        const FMovieSceneChannelData<bool> ChannelData = Channel.GetData();
                        TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
                        TArrayView<const bool> Values = ChannelData.GetValues();

                        if (IncludeSettings.bIncludeKeys)
                        {
                            TArray<TSharedPtr<FJsonValue>> KeysArray;
                            for (int32 Index = 0; Index < Times.Num(); ++Index)
                            {
                                const FFrameNumber TickFrame = Times[Index];
                                if (!FrameFilter.ContainsTick(TickFrame))
                                {
                                    continue;
                                }

                                const int32 DisplayFrame = ConvertTickFrameToDisplay(*MovieScene, TickFrame);
                                if (!FrameFilter.ContainsDisplay(DisplayFrame))
                                {
                                    continue;
                                }

                                const bool bVisible = Values[Index];
                                TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                                KeyJson->SetNumberField(TEXT("frame"), DisplayFrame);
                                KeyJson->SetBoolField(TEXT("visible"), bVisible);
                                KeysArray.Add(MakeShared<FJsonValueObject>(KeyJson));

                                if (ExportFormat == EExportFormat::Csv)
                                {
                                    AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Visibility"),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : TickFrame),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasUpperBound() ? (SectionRange.GetUpperBound().IsExclusive() ? SectionRange.GetUpperBoundValue() - 1 : SectionRange.GetUpperBoundValue()) : TickFrame),
                                              DisplayFrame, TEXT("visible"), FString(), bVisible ? TEXT("true") : TEXT("false"), TOptional<FVector>(), TOptional<FLinearColor>());
                                }
                            }

                            SectionJson->SetArrayField(TEXT("keys"), KeysArray);
                        }

                        if (IncludeSettings.bBindings)
                        {
                            VisibilitySections.Add(MakeShared<FJsonValueObject>(SectionJson));
                        }
                    }

                    if (IncludeSettings.bBindings && VisibilitySections.Num() > 0)
                    {
                        TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
                        TrackJson->SetStringField(TEXT("type"), TEXT("Visibility"));
                        TrackJson->SetArrayField(TEXT("sections"), VisibilitySections);
                        TrackArray.Add(MakeShared<FJsonValueObject>(TrackJson));
                    }
                }
            }

            if (IncludeSettings.bProperty)
            {
                if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
                {
                    const FString PropertyPath = PropertyTrack->GetPropertyPath();
                    const FString PropertyName = PropertyTrack->GetPropertyName().ToString();

                    TArray<TSharedPtr<FJsonValue>> PropertySections;

                    for (UMovieSceneSection* Section : PropertyTrack->GetAllSections())
                    {
                        if (!Section)
                        {
                            continue;
                        }

                        const TRange<FFrameNumber> SectionRange = Section->GetRange();
                        TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
                        AppendRangeArray(*MovieScene, SectionRange, FrameFilter, SectionJson);

                        TArray<TSharedPtr<FJsonValue>> KeysArray;

                        if (UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section))
                        {
                            FMovieSceneBoolChannel& Channel = BoolSection->GetChannel();
                            const FMovieSceneChannelData<bool> ChannelData = Channel.GetData();
                            TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
                            TArrayView<const bool> Values = ChannelData.GetValues();

                            for (int32 Index = 0; Index < Times.Num(); ++Index)
                            {
                                const FFrameNumber TickFrame = Times[Index];
                                if (!FrameFilter.ContainsTick(TickFrame))
                                {
                                    continue;
                                }

                                const int32 DisplayFrame = ConvertTickFrameToDisplay(*MovieScene, TickFrame);
                                if (!FrameFilter.ContainsDisplay(DisplayFrame))
                                {
                                    continue;
                                }

                                const bool bValue = Values[Index];
                                TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                                KeyJson->SetNumberField(TEXT("frame"), DisplayFrame);
                                KeyJson->SetBoolField(TEXT("value"), bValue);
                                KeysArray.Add(MakeShared<FJsonValueObject>(KeyJson));

                                if (ExportFormat == EExportFormat::Csv)
                                {
                                    AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Property"),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : TickFrame),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasUpperBound() ? (SectionRange.GetUpperBound().IsExclusive() ? SectionRange.GetUpperBoundValue() - 1 : SectionRange.GetUpperBoundValue()) : TickFrame),
                                              DisplayFrame, PropertyName, PropertyPath, bValue ? TEXT("true") : TEXT("false"),
                                              TOptional<FVector>(), TOptional<FLinearColor>());
                                }
                            }
                        }
                        else if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
                        {
                            FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
                            const FMovieSceneChannelData<FMovieSceneFloatValue> ChannelData = Channel.GetData();
                            TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
                            TArrayView<const FMovieSceneFloatValue> Values = ChannelData.GetValues();

                            for (int32 Index = 0; Index < Times.Num(); ++Index)
                            {
                                const FFrameNumber TickFrame = Times[Index];
                                if (!FrameFilter.ContainsTick(TickFrame))
                                {
                                    continue;
                                }

                                const int32 DisplayFrame = ConvertTickFrameToDisplay(*MovieScene, TickFrame);
                                if (!FrameFilter.ContainsDisplay(DisplayFrame))
                                {
                                    continue;
                                }

                                const double Value = Values[Index].Value;
                                TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                                KeyJson->SetNumberField(TEXT("frame"), DisplayFrame);
                                KeyJson->SetNumberField(TEXT("value"), Value);
                                KeysArray.Add(MakeShared<FJsonValueObject>(KeyJson));

                                if (ExportFormat == EExportFormat::Csv)
                                {
                                    AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Property"),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : TickFrame),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasUpperBound() ? (SectionRange.GetUpperBound().IsExclusive() ? SectionRange.GetUpperBoundValue() - 1 : SectionRange.GetUpperBoundValue()) : TickFrame),
                                              DisplayFrame, PropertyName, PropertyPath, LexToString(Value), TOptional<FVector>(), TOptional<FLinearColor>());
                                }
                            }
                        }
                        else if (UMovieSceneIntegerSection* IntegerSection = Cast<UMovieSceneIntegerSection>(Section))
                        {
                            FMovieSceneIntegerChannel& Channel = IntegerSection->GetChannel();
                            const FMovieSceneChannelData<int32> ChannelData = Channel.GetData();
                            TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
                            TArrayView<const int32> Values = ChannelData.GetValues();

                            for (int32 Index = 0; Index < Times.Num(); ++Index)
                            {
                                const FFrameNumber TickFrame = Times[Index];
                                if (!FrameFilter.ContainsTick(TickFrame))
                                {
                                    continue;
                                }

                                const int32 DisplayFrame = ConvertTickFrameToDisplay(*MovieScene, TickFrame);
                                if (!FrameFilter.ContainsDisplay(DisplayFrame))
                                {
                                    continue;
                                }

                                const int32 Value = Values[Index];
                                TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                                KeyJson->SetNumberField(TEXT("frame"), DisplayFrame);
                                KeyJson->SetNumberField(TEXT("value"), Value);
                                KeysArray.Add(MakeShared<FJsonValueObject>(KeyJson));

                                if (ExportFormat == EExportFormat::Csv)
                                {
                                    AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Property"),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : TickFrame),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasUpperBound() ? (SectionRange.GetUpperBound().IsExclusive() ? SectionRange.GetUpperBoundValue() - 1 : SectionRange.GetUpperBoundValue()) : TickFrame),
                                              DisplayFrame, PropertyName, PropertyPath, LexToString(Value), TOptional<FVector>(), TOptional<FLinearColor>());
                                }
                            }
                        }
                        else if (UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(Section))
                        {
                            FMovieSceneByteChannel& Channel = ByteSection->GetChannel();
                            const FMovieSceneChannelData<uint8> ChannelData = Channel.GetData();
                            TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
                            TArrayView<const uint8> Values = ChannelData.GetValues();

                            for (int32 Index = 0; Index < Times.Num(); ++Index)
                            {
                                const FFrameNumber TickFrame = Times[Index];
                                if (!FrameFilter.ContainsTick(TickFrame))
                                {
                                    continue;
                                }

                                const int32 DisplayFrame = ConvertTickFrameToDisplay(*MovieScene, TickFrame);
                                if (!FrameFilter.ContainsDisplay(DisplayFrame))
                                {
                                    continue;
                                }

                                const uint8 Value = Values[Index];
                                TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                                KeyJson->SetNumberField(TEXT("frame"), DisplayFrame);
                                KeyJson->SetNumberField(TEXT("value"), Value);
                                KeysArray.Add(MakeShared<FJsonValueObject>(KeyJson));

                                if (ExportFormat == EExportFormat::Csv)
                                {
                                    AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Property"),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : TickFrame),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasUpperBound() ? (SectionRange.GetUpperBound().IsExclusive() ? SectionRange.GetUpperBoundValue() - 1 : SectionRange.GetUpperBoundValue()) : TickFrame),
                                              DisplayFrame, PropertyName, PropertyPath, LexToString(Value), TOptional<FVector>(), TOptional<FLinearColor>());
                                }
                            }
                        }
                        else if (UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section))
                        {
                            FMovieSceneFloatChannel& Red = ColorSection->GetRedChannel();
                            FMovieSceneFloatChannel& Green = ColorSection->GetGreenChannel();
                            FMovieSceneFloatChannel& Blue = ColorSection->GetBlueChannel();
                            FMovieSceneFloatChannel& Alpha = ColorSection->GetAlphaChannel();

                            TSet<int32> KeyFrames;
                            const auto GatherFrames = [&KeyFrames, &FrameFilter](FMovieSceneFloatChannel& Channel)
                            {
                                const FMovieSceneChannelData<FMovieSceneFloatValue> ChannelData = Channel.GetData();
                                TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
                                for (const FFrameNumber& Time : Times)
                                {
                                    const int32 Tick = Time.Value;
                                    if (FrameFilter.TickStart.IsSet() && Tick < FrameFilter.TickStart.GetValue().Value)
                                    {
                                        continue;
                                    }
                                    if (FrameFilter.TickEnd.IsSet() && Tick > FrameFilter.TickEnd.GetValue().Value)
                                    {
                                        continue;
                                    }
                                    KeyFrames.Add(Tick);
                                }
                            };

                            GatherFrames(Red);
                            GatherFrames(Green);
                            GatherFrames(Blue);
                            GatherFrames(Alpha);

                            TArray<int32> Sorted = KeyFrames.Array();
                            Sorted.Sort();

                            for (int32 Tick : Sorted)
                            {
                                const FFrameNumber TickFrame(Tick);
                                if (!FrameFilter.ContainsTick(TickFrame))
                                {
                                    continue;
                                }

                                const int32 DisplayFrame = ConvertTickFrameToDisplay(*MovieScene, TickFrame);
                                if (!FrameFilter.ContainsDisplay(DisplayFrame))
                                {
                                    continue;
                                }

                                double R = 0.0;
                                double G = 0.0;
                                double B = 0.0;
                                double A = 0.0;
                                const bool bHasR = Red.Evaluate(TickFrame, R);
                                const bool bHasG = Green.Evaluate(TickFrame, G);
                                const bool bHasB = Blue.Evaluate(TickFrame, B);
                                const bool bHasA = Alpha.Evaluate(TickFrame, A);
                                if (!(bHasR || bHasG || bHasB || bHasA))
                                {
                                    continue;
                                }

                                FLinearColor Color(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
                                TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                                KeyJson->SetNumberField(TEXT("frame"), DisplayFrame);
                                AppendColorField(TEXT("color"), Color, KeyJson);
                                KeysArray.Add(MakeShared<FJsonValueObject>(KeyJson));

                                if (ExportFormat == EExportFormat::Csv)
                                {
                                    FString ValueString;
                                    if (!bFlattenProperties)
                                    {
                                        ValueString = FString::Printf(TEXT("%s,%s,%s,%s"), *LexToString(Color.R), *LexToString(Color.G), *LexToString(Color.B), *LexToString(Color.A));
                                    }

                                    AddCsvRow(CsvLines, BindingId, BindingLabel, TEXT("Property"),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : TickFrame),
                                              ConvertTickFrameToDisplay(*MovieScene, SectionRange.HasUpperBound() ? (SectionRange.GetUpperBound().IsExclusive() ? SectionRange.GetUpperBoundValue() - 1 : SectionRange.GetUpperBoundValue()) : TickFrame),
                                              DisplayFrame, PropertyName, PropertyPath, ValueString, TOptional<FVector>(), bFlattenProperties ? Color : TOptional<FLinearColor>());
                                }
                            }
                        }

                        if (IncludeSettings.bIncludeKeys)
                        {
                            SectionJson->SetArrayField(TEXT("keys"), KeysArray);
                        }

                        if (IncludeSettings.bBindings)
                        {
                            PropertySections.Add(MakeShared<FJsonValueObject>(SectionJson));
                        }
                    }

                    if (IncludeSettings.bBindings && PropertySections.Num() > 0)
                    {
                        TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
                        TrackJson->SetStringField(TEXT("type"), TEXT("Property"));
                        TrackJson->SetStringField(TEXT("propertyPath"), PropertyPath);
                        if (!PropertyName.IsEmpty())
                        {
                            TrackJson->SetStringField(TEXT("propertyName"), PropertyName);
                        }
                        TrackJson->SetArrayField(TEXT("sections"), PropertySections);
                        TrackArray.Add(MakeShared<FJsonValueObject>(TrackJson));
                    }
                }
            }
        }

        if (IncludeSettings.bBindings && BindingJson.IsValid())
        {
            BindingJson->SetArrayField(TEXT("tracks"), TrackArray);
            BindingsArray.Add(MakeShared<FJsonValueObject>(BindingJson));
        }
    }

    if (IncludeSettings.bBindings)
    {
        Data->SetArrayField(TEXT("bindings"), BindingsArray);
    }

    if (IncludeSettings.bCameraCut)
    {
        if (UMovieSceneCameraCutTrack* CameraCutTrack = MovieScene->FindMasterTrack<UMovieSceneCameraCutTrack>())
        {
            TArray<TSharedPtr<FJsonValue>> CameraCutsArray;
            for (UMovieSceneSection* Section : CameraCutTrack->GetAllSections())
            {
                UMovieSceneCameraCutSection* CameraSection = Cast<UMovieSceneCameraCutSection>(Section);
                if (!CameraSection)
                {
                    continue;
                }

                const TRange<FFrameNumber> SectionRange = CameraSection->GetRange();
                const FFrameNumber StartTick = SectionRange.HasLowerBound() ? SectionRange.GetLowerBoundValue() : FFrameNumber(0);
                FFrameNumber EndTick = SectionRange.HasUpperBound() ? SectionRange.GetUpperBoundValue() : StartTick;
                if (SectionRange.GetUpperBound().IsExclusive())
                {
                    EndTick = EndTick - 1;
                }

                if (FrameFilter.TickStart.IsSet())
                {
                    if (EndTick < FrameFilter.TickStart.GetValue())
                    {
                        continue;
                    }
                }
                if (FrameFilter.TickEnd.IsSet())
                {
                    if (StartTick > FrameFilter.TickEnd.GetValue())
                    {
                        continue;
                    }
                }

                const int32 StartDisplay = ConvertTickFrameToDisplay(*MovieScene, StartTick);
                const int32 EndDisplay = ConvertTickFrameToDisplay(*MovieScene, EndTick);
                if (!FrameFilter.ContainsDisplay(StartDisplay) && !FrameFilter.ContainsDisplay(EndDisplay))
                {
                    if (FrameFilter.DisplayStart.IsSet() || FrameFilter.DisplayEnd.IsSet())
                    {
                        if ((FrameFilter.DisplayStart.IsSet() && EndDisplay < FrameFilter.DisplayStart.GetValue()) ||
                            (FrameFilter.DisplayEnd.IsSet() && StartDisplay > FrameFilter.DisplayEnd.GetValue()))
                        {
                            continue;
                        }
                    }
                }

                TSharedPtr<FJsonObject> CutJson = MakeShared<FJsonObject>();
                CutJson->SetNumberField(TEXT("start"), StartDisplay);
                CutJson->SetNumberField(TEXT("end"), EndDisplay);

                const FMovieSceneObjectBindingID BindingId = CameraSection->GetCameraBindingID();
                CutJson->SetStringField(TEXT("cameraBindingId"), ToNormalizedBindingGuid(BindingId.GetGuid()));
                CameraCutsArray.Add(MakeShared<FJsonValueObject>(CutJson));

                if (ExportFormat == EExportFormat::Csv)
                {
                    AddCsvRow(CsvLines, FString(), TEXT(""), TEXT("CameraCut"), StartDisplay, EndDisplay, StartDisplay, TEXT("camera"), TEXT("cameraBindingId"), ToNormalizedBindingGuid(BindingId.GetGuid()), TOptional<FVector>(), TOptional<FLinearColor>());
                }
            }

            if (CameraCutsArray.Num() > 0)
            {
                Data->SetArrayField(TEXT("cameraCuts"), CameraCutsArray);
            }
            else if (IncludeSettings.bBindings)
            {
                Data->SetArrayField(TEXT("cameraCuts"), {});
            }
        }
    }

    if (ExportFormat == EExportFormat::Csv)
    {
        const FString CsvString = FString::Join(CsvLines, TEXT("\n"));
        Data->SetStringField(TEXT("csv"), CsvString);
    }

    return MakeSuccessResponse(Data);
}
