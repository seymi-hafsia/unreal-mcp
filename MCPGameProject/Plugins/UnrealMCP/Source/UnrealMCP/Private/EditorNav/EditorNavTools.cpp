#include "EditorNav/EditorNavTools.h"

#include "Commands/UnrealMCPCommonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorLevelLibrary.h"
#include "EditorViewportClient.h"
#include "EditorViewportLibrary.h"
#include "Engine/BookMark.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Math/Box.h"
#include "Templates/Optional.h"
#include "Misc/LexFromString.h"
#include "Misc/LexToString.h"

namespace
{
        constexpr int32 MaxBookmarkSlots = 10;

        struct FCameraBookmarkPose
        {
                FVector Location = FVector::ZeroVector;
                FRotator Rotation = FRotator::ZeroRotator;
        };

        TArray<TOptional<FCameraBookmarkPose>> GSessionBookmarks;

        UWorld* GetEditorWorld()
        {
                if (!GEditor)
                {
                        return nullptr;
                }

                if (FWorldContext* PIEContext = GEditor->GetPIEWorldContext())
                {
                        if (PIEContext->World())
                        {
                                return PIEContext->World();
                        }
                }

                return GEditor->GetEditorWorldContext().World();
        }

        FEditorViewportClient* GetActiveLevelViewportClient()
        {
                if (!GEditor)
                {
                        return nullptr;
                }

                if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
                {
                        if (FViewportClient* RawClient = ActiveViewport->GetClient())
                        {
                                return static_cast<FEditorViewportClient*>(RawClient);
                        }
                }

                return nullptr;
        }

        void EnsureBookmarkBuffer()
        {
                if (GSessionBookmarks.Num() < MaxBookmarkSlots)
                {
                        GSessionBookmarks.SetNum(MaxBookmarkSlots);
                }
        }

        bool ParseVector(const TArray<TSharedPtr<FJsonValue>>& JsonArray, FVector& OutVector)
        {
                if (JsonArray.Num() != 3)
                {
                        return false;
                }

                double Components[3];
                for (int32 Index = 0; Index < 3; ++Index)
                {
                        const TSharedPtr<FJsonValue>& Value = JsonArray[Index];
                        if (!Value.IsValid())
                        {
                                return false;
                        }

                        if (Value->Type == EJson::Number)
                        {
                                Components[Index] = Value->AsNumber();
                        }
                        else if (Value->Type == EJson::String)
                        {
                                if (!LexTryParseString(Components[Index], *Value->AsString()))
                                {
                                        return false;
                                }
                        }
                        else
                        {
                                return false;
                        }
                }

                OutVector = FVector(Components[0], Components[1], Components[2]);
                return true;
        }

        bool ParseRotator(const TArray<TSharedPtr<FJsonValue>>& JsonArray, FRotator& OutRotator)
        {
                FVector Vector;
                if (!ParseVector(JsonArray, Vector))
                {
                        return false;
                }

                OutRotator = FRotator(Vector.X, Vector.Y, Vector.Z);
                return true;
        }

        AActor* ResolveActor(const FString& Identifier)
        {
                FString Trimmed = Identifier;
                Trimmed.TrimStartAndEndInline();
                if (Trimmed.IsEmpty())
                {
                        return nullptr;
                }

                if (AActor* Existing = FindObject<AActor>(nullptr, *Trimmed))
                {
                        return Existing;
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

        void CollectAttachedActors(AActor& Actor, TSet<AActor*>& OutActors)
        {
                TArray<AActor*> Attached;
                Actor.GetAttachedActors(Attached);
                for (AActor* AttachedActor : Attached)
                {
                        if (AttachedActor && !OutActors.Contains(AttachedActor))
                        {
                                OutActors.Add(AttachedActor);
                                CollectAttachedActors(*AttachedActor, OutActors);
                        }
                }
        }

        bool CaptureCameraPose(FCameraBookmarkPose& OutPose, FString& OutError)
        {
                if (!GEditor)
                {
                        OutError = TEXT("Editor instance unavailable");
                        return false;
                }

                if (FEditorViewportClient* ViewportClient = GetActiveLevelViewportClient())
                {
                        OutPose.Location = ViewportClient->GetViewLocation();
                        OutPose.Rotation = ViewportClient->GetViewRotation();
                        return true;
                }

                OutError = TEXT("No active level viewport to capture camera");
                return false;
        }

        bool ApplyCameraPose(const FCameraBookmarkPose& Pose, FString& OutError)
        {
                if (!GEditor)
                {
                        OutError = TEXT("Editor instance unavailable");
                        return false;
                }

                if (FEditorViewportClient* ViewportClient = GetActiveLevelViewportClient())
                {
                        ViewportClient->SetViewLocation(Pose.Location);
                        ViewportClient->SetViewRotation(Pose.Rotation);
                        ViewportClient->Invalidate();
                        return true;
                }

                OutError = TEXT("No active level viewport to move camera");
                return false;
        }

        bool PersistBookmark(int32 Index, FString& OutError)
        {
                UEditorLevelLibrary::SetLevelBookmark(Index);

                if (!HasPersistentBookmark(Index))
                {
                        OutError = TEXT("Failed to store persistent bookmark");
                        return false;
                }

                return true;
        }

        bool LoadPersistentBookmark(int32 Index)
        {
                return UEditorLevelLibrary::LoadLevelBookmark(Index);
        }

        bool HasPersistentBookmark(int32 Index)
        {
                if (UWorld* World = GetEditorWorld())
                {
                        if (ULevel* Level = World->GetCurrentLevel())
                        {
                                if (Level->Bookmarks.Num() > Index && Level->Bookmarks[Index] != nullptr)
                                {
                                        return true;
                                }
                        }
                }

                return false;
        }

        void AppendActorPathList(USelection& Selection, TArray<TSharedPtr<FJsonValue>>& OutArray)
        {
                for (FSelectionIterator It(Selection); It; ++It)
                {
                        if (AActor* Actor = Cast<AActor>(*It))
                        {
                                OutArray.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
                        }
                }
        }

        void AppendActorArray(const TArray<AActor*>& Actors, TArray<TSharedPtr<FJsonValue>>& OutArray)
        {
                for (AActor* Actor : Actors)
                {
                        if (Actor)
                        {
                                OutArray.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
                        }
                }
        }

        FString NormalizeMode(const FString& Mode)
        {
                FString Normalized = Mode;
                Normalized.TrimStartAndEndInline();
                Normalized.ToLowerInline();
                return Normalized;
        }

        FString NormalizeOp(const FString& Op)
        {
                FString Normalized = Op;
                Normalized.TrimStartAndEndInline();
                Normalized.ToLowerInline();
                return Normalized;
        }

        bool ParseIndex(const TSharedPtr<FJsonObject>& Params, int32& OutIndex)
        {
                OutIndex = 0;
                if (!Params.IsValid())
                {
                        return true;
                }

                if (Params->HasTypedField<EJson::Number>(TEXT("index")))
                {
                        OutIndex = static_cast<int32>(Params->GetNumberField(TEXT("index")));
                        return true;
                }

                if (Params->HasTypedField<EJson::String>(TEXT("index")))
                {
                        const FString IndexString = Params->GetStringField(TEXT("index"));
                        return LexTryParseString(OutIndex, *IndexString);
                }

                return true;
        }
}

TSharedPtr<FJsonObject> FEditorNavTools::LevelSelect(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
                Error->SetStringField(TEXT("errorCode"), TEXT("LEVEL_SELECT_INVALID_PARAMS"));
                return Error;
        }

        if (!GEditor)
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Editor context unavailable"));
                Error->SetStringField(TEXT("errorCode"), TEXT("LEVEL_SELECT_EDITOR_MISSING"));
                return Error;
        }

        const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
        if (!Params->TryGetObjectField(TEXT("filters"), FiltersObject))
        {
                FiltersObject = nullptr;
        }

        TArray<FString> PathFilters;
        TOptional<FString> NameContains;
        TArray<FString> ClassNames;
        TArray<FName> Tags;

        if (FiltersObject && FiltersObject->IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
                if ((*FiltersObject)->TryGetArrayField(TEXT("paths"), PathsArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *PathsArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FString PathValue = Value->AsString();
                                        PathValue.TrimStartAndEndInline();
                                        if (!PathValue.IsEmpty())
                                        {
                                                PathFilters.Add(PathValue);
                                        }
                                }
                        }
                }

                FString NameValue;
                if ((*FiltersObject)->TryGetStringField(TEXT("nameContains"), NameValue))
                {
                        NameValue.TrimStartAndEndInline();
                        if (!NameValue.IsEmpty())
                        {
                                NameContains = NameValue;
                        }
                }

                const TArray<TSharedPtr<FJsonValue>>* ClassArray = nullptr;
                if ((*FiltersObject)->TryGetArrayField(TEXT("classNames"), ClassArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *ClassArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FString ClassValue = Value->AsString();
                                        ClassValue.TrimStartAndEndInline();
                                        if (!ClassValue.IsEmpty())
                                        {
                                                ClassNames.Add(ClassValue);
                                        }
                                }
                        }
                }

                const TArray<TSharedPtr<FJsonValue>>* TagArray = nullptr;
                if ((*FiltersObject)->TryGetArrayField(TEXT("tags"), TagArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *TagArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FString TagString = Value->AsString();
                                        TagString.TrimStartAndEndInline();
                                        if (!TagString.IsEmpty())
                                        {
                                                Tags.Add(FName(*TagString));
                                        }
                                }
                        }
                }
        }

        FString ModeString = TEXT("replace");
        if (Params->HasTypedField<EJson::String>(TEXT("mode")))
        {
                ModeString = Params->GetStringField(TEXT("mode"));
        }
        const FString NormalizedMode = NormalizeMode(ModeString);

        const bool bSelectChildren = Params->HasTypedField<EJson::Boolean>(TEXT("selectChildren")) && Params->GetBoolField(TEXT("selectChildren"));

        UWorld* World = GetEditorWorld();
        if (!World)
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Unable to resolve editor world"));
                Error->SetStringField(TEXT("errorCode"), TEXT("LEVEL_SELECT_WORLD_MISSING"));
                return Error;
        }

        auto MatchesFilters = [&](AActor* Actor)
        {
                if (!Actor)
                {
                        return false;
                }

                if (NameContains.IsSet() && !Actor->GetName().Contains(NameContains.GetValue(), ESearchCase::IgnoreCase))
                {
                        return false;
                }

                if (PathFilters.Num() > 0)
                {
                        bool bMatchesPath = false;
                        const FString ActorPath = Actor->GetPathName();
                        const FString ClassPath = Actor->GetClass()->GetPathName();
                        for (const FString& Path : PathFilters)
                        {
                                if (ActorPath.StartsWith(Path, ESearchCase::IgnoreCase) || ClassPath.StartsWith(Path, ESearchCase::IgnoreCase))
                                {
                                        bMatchesPath = true;
                                        break;
                                }
                        }

                        if (!bMatchesPath)
                        {
                                return false;
                        }
                }

                if (ClassNames.Num() > 0)
                {
                        const FString ClassName = Actor->GetClass()->GetName();
                        const FString GeneratedClassName = ClassName + TEXT("_C");
                        const FString ClassPath = Actor->GetClass()->GetPathName();
                        bool bMatchesClass = false;
                        for (const FString& Candidate : ClassNames)
                        {
                                if (ClassName.Equals(Candidate, ESearchCase::IgnoreCase) ||
                                        GeneratedClassName.Equals(Candidate, ESearchCase::IgnoreCase) ||
                                        ClassPath.Equals(Candidate, ESearchCase::IgnoreCase))
                                {
                                        bMatchesClass = true;
                                        break;
                                }
                        }

                        if (!bMatchesClass)
                        {
                                return false;
                        }
                }

                if (Tags.Num() > 0)
                {
                        for (const FName& Tag : Tags)
                        {
                                if (!Actor->ActorHasTag(Tag))
                                {
                                        return false;
                                }
                        }
                }

                return true;
        };

        TArray<AActor*> MatchedActors;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
                if (MatchesFilters(*It))
                {
                        MatchedActors.Add(*It);
                }
        }

        if (NormalizedMode == TEXT("replace"))
        {
                GEditor->SelectNone(false, true);
        }

        TSet<AActor*> SelectionSet;
        if (NormalizedMode == TEXT("remove"))
        {
                for (AActor* Actor : MatchedActors)
                {
                        if (Actor)
                        {
                                UEditorLevelLibrary::SetActorSelectionState(Actor, false);
                                if (bSelectChildren)
                                {
                                        TSet<AActor*> Children;
                                        CollectAttachedActors(*Actor, Children);
                                        for (AActor* Child : Children)
                                        {
                                                UEditorLevelLibrary::SetActorSelectionState(Child, false);
                                        }
                                }
                        }
                }
        }
        else
        {
                for (AActor* Actor : MatchedActors)
                {
                        if (Actor)
                        {
                                SelectionSet.Add(Actor);
                                if (bSelectChildren)
                                {
                                        CollectAttachedActors(*Actor, SelectionSet);
                                }
                        }
                }

                for (AActor* Actor : SelectionSet)
                {
                        UEditorLevelLibrary::SetActorSelectionState(Actor, true);
                }
        }

        USelection* Selection = GEditor->GetSelectedActors();
        TArray<TSharedPtr<FJsonValue>> SelectedActorsJson;
        if (Selection)
        {
                AppendActorPathList(*Selection, SelectedActorsJson);
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetNumberField(TEXT("selectedCount"), Selection ? Selection->Num() : 0);
        Data->SetArrayField(TEXT("selectedActors"), SelectedActorsJson);

        if (NormalizedMode == TEXT("remove"))
        {
                Data->SetStringField(TEXT("mode"), TEXT("remove"));
        }
        else if (NormalizedMode == TEXT("add"))
        {
                Data->SetStringField(TEXT("mode"), TEXT("add"));
        }
        else
        {
                Data->SetStringField(TEXT("mode"), TEXT("replace"));
        }

        Data->SetBoolField(TEXT("selectChildren"), bSelectChildren);
        if (MatchedActors.Num() > 0)
        {
                TArray<TSharedPtr<FJsonValue>> MatchedJson;
                AppendActorArray(MatchedActors, MatchedJson);
                Data->SetArrayField(TEXT("matchedActors"), MatchedJson);
        }

        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FEditorNavTools::ViewportFocus(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
                Error->SetStringField(TEXT("errorCode"), TEXT("VIEWPORT_FOCUS_INVALID_PARAMS"));
                return Error;
        }

        if (!GEditor)
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Editor context unavailable"));
                Error->SetStringField(TEXT("errorCode"), TEXT("VIEWPORT_FOCUS_EDITOR_MISSING"));
                return Error;
        }

        TArray<AActor*> FocusActors;
        bool bHasActors = false;
        const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
        if (Params->TryGetArrayField(TEXT("actors"), ActorsArray))
        {
                for (const TSharedPtr<FJsonValue>& Value : *ActorsArray)
                {
                        if (Value.IsValid() && Value->Type == EJson::String)
                        {
                                if (AActor* Actor = ResolveActor(Value->AsString()))
                                {
                                        FocusActors.Add(Actor);
                                }
                        }
                }
                bHasActors = FocusActors.Num() > 0;
        }

        FVector LocationTarget = FVector::ZeroVector;
        bool bHasLocation = false;
        if (Params->HasField(TEXT("location")))
        {
                const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
                if (Params->TryGetArrayField(TEXT("location"), LocationArray) && LocationArray)
                {
                        bHasLocation = ParseVector(*LocationArray, LocationTarget);
                }
        }

        FVector BoxOrigin = FVector::ZeroVector;
        FVector BoxExtent = FVector::ZeroVector;
        bool bHasBox = false;
        if (Params->HasField(TEXT("box")))
        {
                const TSharedPtr<FJsonObject>* BoxObject = nullptr;
                if (Params->TryGetObjectField(TEXT("box"), BoxObject) && BoxObject->IsValid())
                {
                        const TArray<TSharedPtr<FJsonValue>>* OriginArray = nullptr;
                        const TArray<TSharedPtr<FJsonValue>>* ExtentArray = nullptr;
                        if ((*BoxObject)->TryGetArrayField(TEXT("origin"), OriginArray) && OriginArray)
                        {
                                bHasBox = ParseVector(*OriginArray, BoxOrigin);
                        }

                        if (bHasBox && (*BoxObject)->TryGetArrayField(TEXT("extent"), ExtentArray) && ExtentArray)
                        {
                                bHasBox = ParseVector(*ExtentArray, BoxExtent);
                        }
                }
        }

        const bool bOrbit = Params->HasTypedField<EJson::Boolean>(TEXT("orbit")) && Params->GetBoolField(TEXT("orbit"));
        const double TransitionSeconds = Params->HasTypedField<EJson::Number>(TEXT("transitionSec")) ? Params->GetNumberField(TEXT("transitionSec")) : 0.0;

        FString FocusMode;
        int32 Count = 0;

        if (bHasActors)
        {
                FocusMode = TEXT("actors");
                Count = FocusActors.Num();

                if (bOrbit && FocusActors.Num() > 0)
                {
                        FBox Bounds(ForceInit);
                        for (AActor* Actor : FocusActors)
                        {
                                if (Actor)
                                {
                                        Bounds += Actor->GetComponentsBoundingBox(true);
                                }
                        }

                        if (Bounds.IsValid)
                        {
                                const FVector Center = Bounds.GetCenter();
                                const float Radius = Bounds.GetExtent().Size();
                                if (FEditorViewportClient* ViewportClient = GetActiveLevelViewportClient())
                                {
                                        const FVector Forward = ViewportClient->GetViewRotation().Vector();
                                        const float Distance = FMath::Max(Radius * 2.5f, 10.0f);
                                        const FVector NewLocation = Center - Forward * Distance;
                                        ViewportClient->SetViewLocation(NewLocation);
                                        ViewportClient->SetLookAtLocation(Center);
                                        ViewportClient->Invalidate();
                                }
                                else
                                {
                                        UEditorViewportLibrary::FocusViewportOnActors(FocusActors);
                                }
                        }
                        else
                        {
                                UEditorViewportLibrary::FocusViewportOnActors(FocusActors);
                        }
                }
                else
                {
                        UEditorViewportLibrary::FocusViewportOnActors(FocusActors);
                }
        }
        else if (bHasBox)
        {
                FocusMode = TEXT("box");
                Count = 1;

                const FBox FocusBox = FBox::BuildAABB(BoxOrigin, BoxExtent);
                if (bOrbit)
                {
                        if (FEditorViewportClient* ViewportClient = GetActiveLevelViewportClient())
                        {
                                const FVector Center = FocusBox.GetCenter();
                                const float Radius = FocusBox.GetExtent().Size();
                                const FVector Forward = ViewportClient->GetViewRotation().Vector();
                                const float Distance = FMath::Max(Radius * 2.5f, 10.0f);
                                const FVector NewLocation = Center - Forward * Distance;
                                const FVector LookDirection = (Center - NewLocation).GetSafeNormal();
                                ViewportClient->SetViewLocation(NewLocation);
                                ViewportClient->SetViewRotation(LookDirection.Rotation());
                                ViewportClient->Invalidate();
                        }
                        else
                        {
                                UEditorViewportLibrary::FocusViewportOnBox(FocusBox);
                        }
                }
                else
                {
                        UEditorViewportLibrary::FocusViewportOnBox(FocusBox);
                }
        }
        else if (bHasLocation)
        {
                FocusMode = TEXT("location");
                Count = 1;

                if (FEditorViewportClient* ViewportClient = GetActiveLevelViewportClient())
                {
                        const FRotator CurrentRotation = ViewportClient->GetViewRotation();
                        ViewportClient->SetViewLocation(LocationTarget);
                        ViewportClient->SetViewRotation(CurrentRotation);
                        ViewportClient->Invalidate();
                }
                else
                {
                        UEditorViewportLibrary::SetViewportCameraLocation(LocationTarget);
                }
        }
        else
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Provide actors, box or location"));
                Error->SetStringField(TEXT("errorCode"), TEXT("VIEWPORT_FOCUS_NO_TARGET"));
                return Error;
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetStringField(TEXT("focused"), FocusMode);
        Data->SetNumberField(TEXT("count"), Count);

        if (TransitionSeconds > KINDA_SMALL_NUMBER)
        {
                Data->SetNumberField(TEXT("transitionSec"), TransitionSeconds);
                Data->SetBoolField(TEXT("transitionPerformed"), false);
        }

        if (bOrbit)
        {
                Data->SetBoolField(TEXT("orbit"), true);
        }

        if (FocusMode == TEXT("actors"))
        {
                TArray<TSharedPtr<FJsonValue>> ActorsJson;
                AppendActorArray(FocusActors, ActorsJson);
                Data->SetArrayField(TEXT("actors"), ActorsJson);
        }
        else if (FocusMode == TEXT("location"))
        {
                TArray<TSharedPtr<FJsonValue>> LocationArray;
                LocationArray.Add(MakeShared<FJsonValueNumber>(LocationTarget.X));
                LocationArray.Add(MakeShared<FJsonValueNumber>(LocationTarget.Y));
                LocationArray.Add(MakeShared<FJsonValueNumber>(LocationTarget.Z));
                Data->SetArrayField(TEXT("location"), LocationArray);
        }
        else if (FocusMode == TEXT("box"))
        {
                TSharedPtr<FJsonObject> BoxJson = MakeShared<FJsonObject>();
                TArray<TSharedPtr<FJsonValue>> OriginArray;
                OriginArray.Add(MakeShared<FJsonValueNumber>(BoxOrigin.X));
                OriginArray.Add(MakeShared<FJsonValueNumber>(BoxOrigin.Y));
                OriginArray.Add(MakeShared<FJsonValueNumber>(BoxOrigin.Z));

                TArray<TSharedPtr<FJsonValue>> ExtentArray;
                ExtentArray.Add(MakeShared<FJsonValueNumber>(BoxExtent.X));
                ExtentArray.Add(MakeShared<FJsonValueNumber>(BoxExtent.Y));
                ExtentArray.Add(MakeShared<FJsonValueNumber>(BoxExtent.Z));

                BoxJson->SetArrayField(TEXT("origin"), OriginArray);
                BoxJson->SetArrayField(TEXT("extent"), ExtentArray);
                Data->SetObjectField(TEXT("box"), BoxJson);
        }

        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FEditorNavTools::CameraBookmark(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
                Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_INVALID_PARAMS"));
                return Error;
        }

        FString Op;
        if (!Params->TryGetStringField(TEXT("op"), Op))
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing op parameter"));
                Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_OP_REQUIRED"));
                return Error;
        }

        const FString NormalizedOp = NormalizeOp(Op);
        int32 Index = 0;
        if (!ParseIndex(Params, Index) || Index < 0 || Index >= MaxBookmarkSlots)
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Bookmark index must be between 0 and 9"));
                Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_INVALID_INDEX"));
                return Error;
        }

        EnsureBookmarkBuffer();

        if (NormalizedOp == TEXT("set"))
        {
                const bool bPersist = Params->HasTypedField<EJson::Boolean>(TEXT("persist")) && Params->GetBoolField(TEXT("persist"));

                FCameraBookmarkPose Pose;
                FString PoseError;
                if (!CaptureCameraPose(Pose, PoseError))
                {
                        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(PoseError);
                        Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_CAPTURE_FAILED"));
                        return Error;
                }

                GSessionBookmarks[Index] = Pose;

                bool bPersisted = false;
                FString PersistError;
                if (bPersist)
                {
                        if (PersistBookmark(Index, PersistError))
                        {
                                bPersisted = true;
                        }
                        else
                        {
                                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(PersistError);
                                Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_PERSIST_FAILED"));
                                return Error;
                        }
                }

                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("ok"), true);
                Data->SetNumberField(TEXT("index"), Index);
                Data->SetBoolField(TEXT("persisted"), bPersisted);

                return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
        }
        else if (NormalizedOp == TEXT("jump"))
        {
                if (!GSessionBookmarks[Index].IsSet())
                {
                        if (!LoadPersistentBookmark(Index))
                        {
                                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Bookmark slot empty"));
                                Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_SLOT_EMPTY"));
                                return Error;
                        }

                        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                        Data->SetBoolField(TEXT("ok"), true);
                        Data->SetNumberField(TEXT("index"), Index);
                        Data->SetBoolField(TEXT("persisted"), true);
                        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
                }

                FString ApplyError;
                if (!ApplyCameraPose(GSessionBookmarks[Index].GetValue(), ApplyError))
                {
                        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ApplyError);
                        Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_APPLY_FAILED"));
                        return Error;
                }

                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("ok"), true);
                Data->SetNumberField(TEXT("index"), Index);
                Data->SetBoolField(TEXT("persisted"), false);
                return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
        }
        else if (NormalizedOp == TEXT("list"))
        {
                TArray<TSharedPtr<FJsonValue>> BookmarksJson;
                for (int32 Slot = 0; Slot < MaxBookmarkSlots; ++Slot)
                {
                        TSharedPtr<FJsonObject> SlotJson = MakeShared<FJsonObject>();
                        SlotJson->SetNumberField(TEXT("index"), Slot);
                        const bool bHasSession = GSessionBookmarks.Num() > Slot && GSessionBookmarks[Slot].IsSet();
                        const bool bHasPersistent = HasPersistentBookmark(Slot);
                        SlotJson->SetBoolField(TEXT("hasSession"), bHasSession);
                        SlotJson->SetBoolField(TEXT("hasPersistent"), bHasPersistent);
                        SlotJson->SetBoolField(TEXT("has"), bHasSession || bHasPersistent);
                        BookmarksJson.Add(MakeShared<FJsonValueObject>(SlotJson));
                }

                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("ok"), true);
                Data->SetArrayField(TEXT("bookmarks"), BookmarksJson);
                return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
        }

        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Unsupported bookmark op"));
        Error->SetStringField(TEXT("errorCode"), TEXT("BOOKMARK_UNSUPPORTED_OP"));
        return Error;
}

