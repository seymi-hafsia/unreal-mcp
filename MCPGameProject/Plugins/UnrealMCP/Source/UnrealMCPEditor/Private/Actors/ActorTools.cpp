#include "Actors/ActorTools.h"
#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Char.h"
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
#include "UObject/UObjectGlobals.h"
#include "Components/SceneComponent.h"

namespace
{
        constexpr const TCHAR* ErrorCodeInvalidParams = TEXT("INVALID_PARAMETERS");
        constexpr const TCHAR* ErrorCodeClassNotFound = TEXT("CLASS_NOT_FOUND");
        constexpr const TCHAR* ErrorCodeSpawnFailed = TEXT("SPAWN_FAILED");
        constexpr const TCHAR* ErrorCodeActorNotFound = TEXT("ACTOR_NOT_FOUND");
        constexpr const TCHAR* ErrorCodeDestroyFailed = TEXT("DESTROY_FAILED");
        constexpr const TCHAR* ErrorCodeAttachFailed = TEXT("ATTACH_FAILED");
        constexpr const TCHAR* ErrorCodeTransformFailed = TEXT("TRANSFORM_FAILED");

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
                if (!GEditor)
                {
                        return nullptr;
                }

                FWorldContext* WorldContext = GEditor->GetPIEWorldContext();
                if (WorldContext && WorldContext->World())
                {
                        return WorldContext->World();
                }

                return GEditor->GetEditorWorldContext().World();
        }

        AActor* ResolveActor(const FString& ActorIdentifier)
        {
                if (ActorIdentifier.IsEmpty())
                {
                        return nullptr;
                }

                FString Trimmed = ActorIdentifier;
                Trimmed.TrimStartAndEndInline();
                if (Trimmed.IsEmpty())
                {
                        return nullptr;
                }

                if (AActor* FoundActor = FindObject<AActor>(nullptr, *Trimmed))
                {
                        return FoundActor;
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

        bool ParseNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
        {
                if (!Value.IsValid())
                {
                        return false;
                }

                if (Value->Type == EJson::Number)
                {
                        OutNumber = Value->AsNumber();
                        return true;
                }

                if (Value->Type == EJson::String)
                {
                        const FString StringValue = Value->AsString();
                        return LexTryParseString(OutNumber, *StringValue);
                }

                return false;
        }

        bool ParseVector(const TArray<TSharedPtr<FJsonValue>>& Values, FVector& OutVector)
        {
                if (Values.Num() != 3)
                {
                        return false;
                }

                double Components[3];
                for (int32 Index = 0; Index < 3; ++Index)
                {
                        if (!ParseNumber(Values[Index], Components[Index]))
                        {
                                return false;
                        }
                }

                OutVector = FVector(static_cast<float>(Components[0]), static_cast<float>(Components[1]), static_cast<float>(Components[2]));
                return true;
        }

        bool ParseRotator(const TArray<TSharedPtr<FJsonValue>>& Values, FRotator& OutRotator)
        {
                FVector Vector;
                if (!ParseVector(Values, Vector))
                {
                        return false;
                }

                OutRotator = FRotator(Vector.X, Vector.Y, Vector.Z);
                return true;
        }

        bool ParseFNameArray(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FName>& OutNames)
        {
                for (const TSharedPtr<FJsonValue>& Value : Values)
                {
                        if (!Value.IsValid() || Value->Type != EJson::String)
                        {
                                return false;
                        }

                        FString TagString = Value->AsString();
                        TagString.TrimStartAndEndInline();
                        if (!TagString.IsEmpty())
                        {
                                OutNames.Add(FName(*TagString));
                        }
                }

                return true;
        }

        UClass* ResolveActorClass(const FString& ClassPath)
        {
                FString Trimmed = ClassPath;
                Trimmed.TrimStartAndEndInline();
                if (Trimmed.IsEmpty())
                {
                        return nullptr;
                }

                if (Trimmed.EndsWith(TEXT("_C")))
                {
                        return LoadObject<UClass>(nullptr, *Trimmed);
                }

                return StaticLoadClass(AActor::StaticClass(), nullptr, *Trimmed);
        }

        void ApplyTags(AActor& Actor, const TArray<FName>& Tags)
        {
                if (Tags.Num() == 0)
                {
                        return;
                }

                Actor.Tags.Reset(Tags.Num());
                for (const FName& Tag : Tags)
                {
                        Actor.Tags.Add(Tag);
                }
        }

        void AppendTags(AActor& Actor, const TArray<FName>& Tags)
        {
                for (const FName& Tag : Tags)
                {
                        if (!Actor.Tags.Contains(Tag))
                        {
                                Actor.Tags.Add(Tag);
                        }
                }
        }

        void RemoveTags(AActor& Actor, const TArray<FName>& Tags)
        {
                if (Tags.Num() == 0)
                {
                        return;
                }

                Actor.Tags.RemoveAll([&Tags](const FName& Existing)
                {
                        return Tags.Contains(Existing);
                });
        }

        void SelectSingleActor(AActor* Actor)
        {
                if (!GEditor || !Actor)
                {
                        return;
                }

                GEditor->SelectNone(false, true, false);
                GEditor->SelectActor(Actor, true, true);
        }

        void DeselectActor(AActor* Actor)
        {
                if (!GEditor || !Actor)
                {
                        return;
                }

                if (GEditor->GetSelectedActors()->IsSelected(Actor))
                {
                        GEditor->SelectActor(Actor, false, true);
                }
        }

        TSharedPtr<FJsonObject> MakeTransformJson(const FVector& Location, const FRotator& Rotation, const FVector& Scale)
        {
                TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

                TArray<TSharedPtr<FJsonValue>> LocationArray;
                LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
                LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
                LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
                Json->SetArrayField(TEXT("location"), LocationArray);

                TArray<TSharedPtr<FJsonValue>> RotationArray;
                RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
                RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
                RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
                Json->SetArrayField(TEXT("rotation"), RotationArray);

                TArray<TSharedPtr<FJsonValue>> ScaleArray;
                ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
                ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
                ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
                Json->SetArrayField(TEXT("scale"), ScaleArray);

                return Json;
        }
}

TSharedPtr<FJsonObject> FActorTools::Spawn(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString ClassPath;
        if (!Params->TryGetStringField(TEXT("classPath"), ClassPath) || ClassPath.IsEmpty())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing classPath parameter"));
        }

        UClass* ActorClass = ResolveActorClass(ClassPath);
        if (!ActorClass)
        {
                return MakeErrorResponse(ErrorCodeClassNotFound, FString::Printf(TEXT("Unable to load class: %s"), *ClassPath));
        }

        if (!ActorClass->IsChildOf(AActor::StaticClass()))
        {
                return MakeErrorResponse(ErrorCodeClassNotFound, FString::Printf(TEXT("Class is not an Actor: %s"), *ClassPath));
        }

        FVector Location = FVector::ZeroVector;
        if (Params->HasField(TEXT("location")))
        {
                const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
                if (!Params->TryGetArrayField(TEXT("location"), LocationArray) || !ParseVector(*LocationArray, Location))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("location must be an array of three numbers"));
                }
        }

        FRotator Rotation = FRotator::ZeroRotator;
        if (Params->HasField(TEXT("rotation")))
        {
                const TArray<TSharedPtr<FJsonValue>>* RotationArray = nullptr;
                if (!Params->TryGetArrayField(TEXT("rotation"), RotationArray) || !ParseRotator(*RotationArray, Rotation))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("rotation must be an array of three numbers"));
                }
        }

        FVector Scale = FVector::OneVector;
        if (Params->HasField(TEXT("scale")))
        {
                const TArray<TSharedPtr<FJsonValue>>* ScaleArray = nullptr;
                if (!Params->TryGetArrayField(TEXT("scale"), ScaleArray) || !ParseVector(*ScaleArray, Scale))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("scale must be an array of three numbers"));
                }
        }

        bool bSelect = Params->HasTypedField<EJson::Boolean>(TEXT("select")) && Params->GetBoolField(TEXT("select"));
        bool bDeferred = Params->HasTypedField<EJson::Boolean>(TEXT("deferred")) && Params->GetBoolField(TEXT("deferred"));

        TArray<FName> SpawnTags;
        if (Params->HasField(TEXT("tags")))
        {
                const TArray<TSharedPtr<FJsonValue>>* TagArray = nullptr;
                if (!Params->TryGetArrayField(TEXT("tags"), TagArray) || !ParseFNameArray(*TagArray, SpawnTags))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("tags must be an array of strings"));
                }
        }

        UWorld* World = GetEditorWorld();
        if (!World)
        {
                return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("Editor world is unavailable"));
        }

        const FTransform SpawnTransform(Rotation, Location, Scale);

        AActor* NewActor = nullptr;
        if (bDeferred)
        {
                NewActor = World->SpawnActorDeferred<AActor>(ActorClass, SpawnTransform);
                if (!NewActor)
                {
                        return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("SpawnActorDeferred returned null"));
                }

                if (SpawnTags.Num() > 0)
                {
                        ApplyTags(*NewActor, SpawnTags);
                }

                NewActor->FinishSpawning(SpawnTransform);
        }
        else
        {
                FActorSpawnParameters SpawnParameters;
                SpawnParameters.bNoFail = false;
                SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

                NewActor = World->SpawnActor<AActor>(ActorClass, SpawnTransform, SpawnParameters);
                if (!NewActor)
                {
                        return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("SpawnActor returned null"));
                }

                if (SpawnTags.Num() > 0)
                {
                        ApplyTags(*NewActor, SpawnTags);
                }
        }

        if (!NewActor)
        {
                return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("Failed to spawn actor"));
        }

        if (bSelect)
        {
                SelectSingleActor(NewActor);
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actorPath"), NewActor->GetPathName());
        Data->SetStringField(TEXT("class"), NewActor->GetClass()->GetName());
        Data->SetObjectField(TEXT("transform"), MakeTransformJson(NewActor->GetActorLocation(), NewActor->GetActorRotation(), NewActor->GetActorScale3D()));

        if (SpawnTags.Num() > 0)
        {
                TArray<TSharedPtr<FJsonValue>> TagValues;
                for (const FName& Tag : SpawnTags)
                {
                        TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
                }
                Data->SetArrayField(TEXT("tags"), TagValues);
        }

        return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FActorTools::Destroy(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
        if (!Params->TryGetArrayField(TEXT("actors"), ActorsArray) || ActorsArray->Num() == 0)
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("actors must be a non-empty array"));
        }

        const bool bAllowMissing = Params->HasTypedField<EJson::Boolean>(TEXT("allowMissing")) && Params->GetBoolField(TEXT("allowMissing"));

        TArray<TSharedPtr<FJsonValue>> ResultArray;

        for (const TSharedPtr<FJsonValue>& Value : *ActorsArray)
        {
                if (!Value.IsValid() || Value->Type != EJson::String)
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("actors must contain string identifiers"));
                }

                const FString ActorPath = Value->AsString();
                AActor* TargetActor = ResolveActor(ActorPath);

                if (!TargetActor)
                {
                        if (!bAllowMissing)
                        {
                                return MakeErrorResponse(ErrorCodeActorNotFound, FString::Printf(TEXT("Actor not found: %s"), *ActorPath));
                        }

                        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                        Entry->SetStringField(TEXT("actorPath"), ActorPath);
                        Entry->SetBoolField(TEXT("deleted"), false);
                        Entry->SetBoolField(TEXT("missing"), true);
                        ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
                        continue;
                }

                TargetActor->Modify();
                DeselectActor(TargetActor);

                if (!TargetActor->Destroy(true))
                {
                        return MakeErrorResponse(ErrorCodeDestroyFailed, FString::Printf(TEXT("Failed to destroy actor: %s"), *ActorPath));
                }

                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("actorPath"), ActorPath);
                Entry->SetBoolField(TEXT("deleted"), true);
                ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("results"), ResultArray);
        Data->SetNumberField(TEXT("count"), ResultArray.Num());

        return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FActorTools::Attach(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString ChildIdentifier;
        FString ParentIdentifier;
        if (!Params->TryGetStringField(TEXT("child"), ChildIdentifier) || !Params->TryGetStringField(TEXT("parent"), ParentIdentifier))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing child or parent parameter"));
        }

        AActor* ChildActor = ResolveActor(ChildIdentifier);
        if (!ChildActor)
        {
                return MakeErrorResponse(ErrorCodeActorNotFound, FString::Printf(TEXT("Child actor not found: %s"), *ChildIdentifier));
        }

        AActor* ParentActor = ResolveActor(ParentIdentifier);
        if (!ParentActor)
        {
                return MakeErrorResponse(ErrorCodeActorNotFound, FString::Printf(TEXT("Parent actor not found: %s"), *ParentIdentifier));
        }

        USceneComponent* ChildRoot = ChildActor->GetRootComponent();
        USceneComponent* ParentRoot = ParentActor->GetRootComponent();
        if (!ChildRoot || !ParentRoot)
        {
                return MakeErrorResponse(ErrorCodeAttachFailed, TEXT("Both actors must have a root component"));
        }

        const bool bKeepWorld = !Params->HasField(TEXT("keepWorldTransform")) || (Params->HasTypedField<EJson::Boolean>(TEXT("keepWorldTransform")) && Params->GetBoolField(TEXT("keepWorldTransform")));
        const bool bWeldBodies = Params->HasTypedField<EJson::Boolean>(TEXT("weldSimulatedBodies")) && Params->GetBoolField(TEXT("weldSimulatedBodies"));

        FAttachmentTransformRules AttachRules = bKeepWorld ? FAttachmentTransformRules::KeepWorldTransform : FAttachmentTransformRules::KeepRelativeTransform;
        AttachRules.bWeldSimulatedBodies = bWeldBodies;

        FName SocketName = NAME_None;
        if (Params->HasField(TEXT("socketName")))
        {
                FString SocketString;
                if (!Params->TryGetStringField(TEXT("socketName"), SocketString))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("socketName must be a string"));
                }

                SocketName = SocketString.IsEmpty() ? NAME_None : FName(*SocketString);
        }

        ChildActor->Modify();
        if (!ChildRoot->AttachToComponent(ParentRoot, AttachRules, SocketName))
        {
                return MakeErrorResponse(ErrorCodeAttachFailed, TEXT("AttachToComponent failed"));
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("child"), ChildActor->GetPathName());
        Data->SetStringField(TEXT("parent"), ParentActor->GetPathName());
        Data->SetBoolField(TEXT("keepWorldTransform"), bKeepWorld);
        Data->SetBoolField(TEXT("weldSimulatedBodies"), bWeldBodies);
        if (SocketName != NAME_None)
        {
                Data->SetStringField(TEXT("socket"), SocketName.ToString());
        }

        return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FActorTools::Transform(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString ActorIdentifier;
        if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing actor parameter"));
        }

        AActor* TargetActor = ResolveActor(ActorIdentifier);
        if (!TargetActor)
        {
                return MakeErrorResponse(ErrorCodeActorNotFound, FString::Printf(TEXT("Actor not found: %s"), *ActorIdentifier));
        }

        FVector Location = TargetActor->GetActorLocation();
        FRotator Rotation = TargetActor->GetActorRotation();
        FVector Scale = TargetActor->GetActorScale3D();

        const TSharedPtr<FJsonObject>* SetObjectPtr = nullptr;
        if (Params->TryGetObjectField(TEXT("set"), SetObjectPtr))
        {
                const TSharedPtr<FJsonObject>& SetObject = *SetObjectPtr;
                if (SetObject.IsValid())
                {
                        if (SetObject->HasField(TEXT("location")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
                                if (!SetObject->TryGetArrayField(TEXT("location"), LocationArray) || !ParseVector(*LocationArray, Location))
                                {
                                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("set.location must be an array of three numbers"));
                                }
                        }

                        if (SetObject->HasField(TEXT("rotation")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>* RotationArray = nullptr;
                                if (!SetObject->TryGetArrayField(TEXT("rotation"), RotationArray) || !ParseRotator(*RotationArray, Rotation))
                                {
                                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("set.rotation must be an array of three numbers"));
                                }
                        }

                        if (SetObject->HasField(TEXT("scale")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>* ScaleArray = nullptr;
                                if (!SetObject->TryGetArrayField(TEXT("scale"), ScaleArray) || !ParseVector(*ScaleArray, Scale))
                                {
                                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("set.scale must be an array of three numbers"));
                                }
                        }
                }
        }

        const TSharedPtr<FJsonObject>* AddObjectPtr = nullptr;
        if (Params->TryGetObjectField(TEXT("add"), AddObjectPtr))
        {
                const TSharedPtr<FJsonObject>& AddObject = *AddObjectPtr;
                if (AddObject.IsValid())
                {
                        if (AddObject->HasField(TEXT("location")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
                                FVector LocationDelta = FVector::ZeroVector;
                                if (!AddObject->TryGetArrayField(TEXT("location"), LocationArray) || !ParseVector(*LocationArray, LocationDelta))
                                {
                                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("add.location must be an array of three numbers"));
                                }
                                Location += LocationDelta;
                        }

                        if (AddObject->HasField(TEXT("rotation")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>* RotationArray = nullptr;
                                FRotator RotationDelta = FRotator::ZeroRotator;
                                if (!AddObject->TryGetArrayField(TEXT("rotation"), RotationArray) || !ParseRotator(*RotationArray, RotationDelta))
                                {
                                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("add.rotation must be an array of three numbers"));
                                }
                                Rotation += RotationDelta;
                        }

                        if (AddObject->HasField(TEXT("scale")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>* ScaleArray = nullptr;
                                FVector ScaleDelta = FVector::ZeroVector;
                                if (!AddObject->TryGetArrayField(TEXT("scale"), ScaleArray) || !ParseVector(*ScaleArray, ScaleDelta))
                                {
                                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("add.scale must be an array of three numbers"));
                                }
                                Scale += ScaleDelta;
                        }
                }
        }

        TargetActor->Modify();
        if (!TargetActor->SetActorTransform(FTransform(Rotation, Location, Scale), false, nullptr, ETeleportType::TeleportPhysics))
        {
                return MakeErrorResponse(ErrorCodeTransformFailed, TEXT("Failed to set actor transform"));
        }

        TSharedPtr<FJsonObject> Data = MakeTransformJson(Location, Rotation, Scale);
        Data->SetStringField(TEXT("actor"), TargetActor->GetPathName());

        return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FActorTools::Tag(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString ActorIdentifier;
        if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing actor parameter"));
        }

        AActor* TargetActor = ResolveActor(ActorIdentifier);
        if (!TargetActor)
        {
                return MakeErrorResponse(ErrorCodeActorNotFound, FString::Printf(TEXT("Actor not found: %s"), *ActorIdentifier));
        }

        TArray<FName> ReplaceTags;
        bool bReplaceProvided = false;
        if (Params->HasField(TEXT("replace")))
        {
                const TSharedPtr<FJsonValue> ReplaceValue = Params->TryGetField(TEXT("replace"));
                if (!ReplaceValue.IsValid() || ReplaceValue->Type == EJson::Null)
                {
                        bReplaceProvided = true;
                        ReplaceTags.Reset();
                }
                else if (ReplaceValue->Type == EJson::Array)
                {
                        bReplaceProvided = true;
                        if (!ParseFNameArray(ReplaceValue->AsArray(), ReplaceTags))
                        {
                                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("replace must be an array of strings or null"));
                        }
                }
                else
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("replace must be an array or null"));
                }
        }

        TArray<FName> AddTags;
        if (Params->HasField(TEXT("add")))
        {
                                const TArray<TSharedPtr<FJsonValue>>* AddArray = nullptr;
                if (!Params->TryGetArrayField(TEXT("add"), AddArray) || !ParseFNameArray(*AddArray, AddTags))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("add must be an array of strings"));
                }
        }

        TArray<FName> RemoveTagsArray;
        if (Params->HasField(TEXT("remove")))
        {
                const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
                if (!Params->TryGetArrayField(TEXT("remove"), RemoveArray) || !ParseFNameArray(*RemoveArray, RemoveTagsArray))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("remove must be an array of strings"));
                }
        }

        TargetActor->Modify();

        if (bReplaceProvided)
        {
                TargetActor->Tags.Reset(ReplaceTags.Num());
                for (const FName& Tag : ReplaceTags)
                {
                        TargetActor->Tags.Add(Tag);
                }
        }

        if (AddTags.Num() > 0)
        {
                AppendTags(*TargetActor, AddTags);
        }

        if (RemoveTagsArray.Num() > 0)
        {
                RemoveTags(*TargetActor, RemoveTagsArray);
        }

        TArray<TSharedPtr<FJsonValue>> TagValues;
        for (const FName& Tag : TargetActor->Tags)
        {
                TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actor"), TargetActor->GetPathName());
        Data->SetArrayField(TEXT("tags"), TagValues);

        return MakeSuccessResponse(Data);
}
