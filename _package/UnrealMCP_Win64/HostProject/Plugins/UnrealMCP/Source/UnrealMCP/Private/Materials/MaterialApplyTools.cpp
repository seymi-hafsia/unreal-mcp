#include "Materials/MaterialApplyTools.h"

#include "Algo/Transform.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EditorLoadingAndSavingUtils.h"
#include "Engine/Level.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Permissions/WriteGate.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParams = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodeActorNotFound = TEXT("ACTOR_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeComponentNotFound = TEXT("COMPONENT_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeMaterialNotFound = TEXT("MI_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeSlotNotFound = TEXT("SLOT_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeAssetNotFound = TEXT("ASSET_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeUnsupportedMesh = TEXT("MESH_UNSUPPORTED_TYPE");
    constexpr const TCHAR* ErrorCodeSlotConflict = TEXT("SLOT_NAME_CONFLICT");
    constexpr const TCHAR* ErrorCodeReorderInvalid = TEXT("REORDER_INVALID");
    constexpr const TCHAR* ErrorCodeSaveFailed = TEXT("SAVE_FAILED");

    TSharedPtr<FJsonObject> MakeErrorResponse(const FString& Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetBoolField(TEXT("success"), false);
        Error->SetBoolField(TEXT("ok"), false);
        Error->SetStringField(TEXT("errorCode"), Code);
        Error->SetStringField(TEXT("error"), Message);
        return Error;
    }

    TSharedPtr<FJsonObject> MakeSuccessResponse()
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetBoolField(TEXT("ok"), true);
        return Result;
    }

    TSharedPtr<FJsonObject> MakeAuditObject(bool bDryRun, const TArray<TSharedPtr<FJsonValue>>& Actions)
    {
        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("dryRun"), bDryRun);
        Audit->SetArrayField(TEXT("actions"), Actions);
        return Audit;
    }

    void AppendStringArrayField(TSharedPtr<FJsonObject> JsonObject, const FString& FieldName, const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> JsonArray;
        for (const FString& Value : Values)
        {
            JsonArray.Add(MakeShared<FJsonValueString>(Value));
        }
        JsonObject->SetArrayField(FieldName, JsonArray);
    }

    UWorld* GetEditorWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }

        if (FWorldContext* PieContext = GEditor->GetPIEWorldContext())
        {
            if (UWorld* PieWorld = PieContext->World())
            {
                return PieWorld;
            }
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

    UMeshComponent* ResolveMeshComponent(AActor& Actor, const FString& ComponentName)
    {
        if (!ComponentName.IsEmpty())
        {
            FString Trimmed = ComponentName;
            Trimmed.TrimStartAndEndInline();
            if (!Trimmed.IsEmpty())
            {
                for (UActorComponent* Component : Actor.GetComponents())
                {
                    if (Component && Component->GetName() == Trimmed)
                    {
                        return Cast<UMeshComponent>(Component);
                    }
                }
            }
        }
        else if (UMeshComponent* RootMesh = Cast<UMeshComponent>(Actor.GetRootComponent()))
        {
            return RootMesh;
        }

        TArray<UMeshComponent*> MeshComponents;
        Actor.GetComponents<UMeshComponent>(MeshComponents);
        if (MeshComponents.Num() > 0)
        {
            return MeshComponents[0];
        }

        return nullptr;
    }

    bool ResolveSlotIndex(UMeshComponent& Component, const TSharedPtr<FJsonValue>& SlotValue, int32& OutIndex, bool& bOutWasName)
    {
        bOutWasName = false;
        if (!SlotValue.IsValid())
        {
            return false;
        }

        if (SlotValue->Type == EJson::Number)
        {
            const double SlotNumber = SlotValue->AsNumber();
            const int32 Index = FMath::FloorToInt(SlotNumber);
            if (Index < 0 || Index >= Component.GetNumMaterials())
            {
                return false;
            }

            OutIndex = Index;
            return true;
        }

        if (SlotValue->Type == EJson::String)
        {
            const FString SlotNameString = SlotValue->AsString();
            FName SlotName(*SlotNameString);
            const TArray<FName> SlotNames = Component.GetMaterialSlotNames();
            const int32 FoundIndex = SlotNames.IndexOfByKey(SlotName);
            if (FoundIndex == INDEX_NONE)
            {
                return false;
            }

            bOutWasName = true;
            OutIndex = FoundIndex;
            return true;
        }

        return false;
    }

    UMaterialInterface* LoadMaterialInterface(const FString& Path)
    {
        FString Trimmed = Path;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return nullptr;
        }

        return LoadObject<UMaterialInterface>(nullptr, *Trimmed);
    }

    FString NormalizeContentPath(const FString& InPath)
    {
        FString Trimmed = InPath;
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

    FString BuildObjectPath(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
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
}

TSharedPtr<FJsonObject> FMaterialApplyTools::BatchApply(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    const TArray<TSharedPtr<FJsonValue>>* TargetsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("targets"), TargetsArray) || !TargetsArray)
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing targets array"));
    }

    const bool bStrict = !Params->HasField(TEXT("strict")) || Params->GetBoolField(TEXT("strict"));
    const bool bSaveActors = Params->HasField(TEXT("saveActors")) && Params->GetBoolField(TEXT("saveActors"));

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("No editor world available"));
    }

    TArray<TSharedPtr<FJsonValue>> AppliedItems;
    TArray<TSharedPtr<FJsonValue>> SkippedItems;
    TArray<TSharedPtr<FJsonValue>> AuditActions;

    TSet<UPackage*> PackagesToSave;

    for (const TSharedPtr<FJsonValue>& TargetValue : *TargetsArray)
    {
        if (!TargetValue.IsValid() || TargetValue->Type != EJson::Object)
        {
            return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Invalid target entry"));
        }

        const TSharedPtr<FJsonObject> TargetObject = TargetValue->AsObject();
        if (!TargetObject.IsValid())
        {
            return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Invalid target entry"));
        }

        FString ActorPath;
        if (!TargetObject->TryGetStringField(TEXT("actorPath"), ActorPath) || ActorPath.TrimStartAndEnd().IsEmpty())
        {
            return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Target missing actorPath"));
        }

        AActor* TargetActor = ResolveActor(ActorPath);
        if (!TargetActor)
        {
            return MakeErrorResponse(ErrorCodeActorNotFound, FString::Printf(TEXT("Actor not found: %s"), *ActorPath));
        }

        FString LevelPath;
        if (ULevel* Level = TargetActor->GetLevel())
        {
            if (UPackage* Package = Level->GetOutermost())
            {
                LevelPath = NormalizeContentPath(Package->GetName());
            }
        }

        if (!LevelPath.IsEmpty())
        {
            FString PathReason;
            if (!FWriteGate::IsPathAllowed(LevelPath, PathReason))
            {
                return MakeErrorResponse(TEXT("PATH_NOT_ALLOWED"), PathReason);
            }
        }

        FString ComponentName;
        TargetObject->TryGetStringField(TEXT("component"), ComponentName);
        UMeshComponent* MeshComponent = ResolveMeshComponent(*TargetActor, ComponentName);
        if (!MeshComponent)
        {
            return MakeErrorResponse(ErrorCodeComponentNotFound, FString::Printf(TEXT("Mesh component not found on %s"), *ActorPath));
        }

        const TArray<TSharedPtr<FJsonValue>>* AssignArray = nullptr;
        if (!TargetObject->TryGetArrayField(TEXT("assign"), AssignArray) || !AssignArray)
        {
            return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Target missing assign array"));
        }

        bool bComponentModified = false;
        MeshComponent->Modify();

        for (const TSharedPtr<FJsonValue>& AssignValue : *AssignArray)
        {
            if (!AssignValue.IsValid() || AssignValue->Type != EJson::Object)
            {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Invalid assign entry"));
            }

            const TSharedPtr<FJsonObject> AssignObject = AssignValue->AsObject();
            if (!AssignObject.IsValid())
            {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Invalid assign entry"));
            }

            TSharedPtr<FJsonValue> SlotValue = AssignObject->TryGetField(TEXT("slot"));
            if (!SlotValue.IsValid())
            {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Assign entry missing slot"));
            }

            FString MiPath;
            if (!AssignObject->TryGetStringField(TEXT("mi"), MiPath) || MiPath.TrimStartAndEnd().IsEmpty())
            {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Assign entry missing mi path"));
            }

            int32 SlotIndex = INDEX_NONE;
            bool bSlotWasName = false;
            if (!ResolveSlotIndex(*MeshComponent, SlotValue, SlotIndex, bSlotWasName))
            {
                if (bStrict)
                {
                    return MakeErrorResponse(ErrorCodeSlotNotFound, FString::Printf(TEXT("Slot not found on %s"), *ActorPath));
                }

                TSharedPtr<FJsonObject> Skipped = MakeShared<FJsonObject>();
                Skipped->SetStringField(TEXT("actor"), ActorPath);
                if (!ComponentName.IsEmpty())
                {
                    Skipped->SetStringField(TEXT("component"), ComponentName);
                }

                if (SlotValue->Type == EJson::Number)
                {
                    Skipped->SetNumberField(TEXT("slot"), SlotValue->AsNumber());
                }
                else if (SlotValue->Type == EJson::String)
                {
                    Skipped->SetStringField(TEXT("slot"), SlotValue->AsString());
                }

                Skipped->SetStringField(TEXT("reason"), TEXT("slot_not_found"));
                SkippedItems.Add(MakeShared<FJsonValueObject>(Skipped));
                continue;
            }

            UMaterialInterface* Material = LoadMaterialInterface(MiPath);
            if (!Material)
            {
                return MakeErrorResponse(ErrorCodeMaterialNotFound, FString::Printf(TEXT("Material interface not found: %s"), *MiPath));
            }

            UMaterialInterface* PreviousMaterial = MeshComponent->GetMaterial(SlotIndex);
            MeshComponent->SetMaterial(SlotIndex, Material);
            bComponentModified = true;

            TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
            Applied->SetStringField(TEXT("actor"), ActorPath);
            if (!ComponentName.IsEmpty())
            {
                Applied->SetStringField(TEXT("component"), ComponentName);
            }

            if (bSlotWasName && SlotValue->Type == EJson::String)
            {
                Applied->SetStringField(TEXT("slot"), SlotValue->AsString());
            }
            else
            {
                Applied->SetNumberField(TEXT("slot"), SlotIndex);
            }

            Applied->SetStringField(TEXT("mi"), MiPath);
            Applied->SetStringField(TEXT("prev"), PreviousMaterial ? PreviousMaterial->GetPathName() : FString());
            AppliedItems.Add(MakeShared<FJsonValueObject>(Applied));

            TMap<FString, FString> AuditArgs;
            AuditArgs.Add(TEXT("actor"), ActorPath);
            if (!ComponentName.IsEmpty())
            {
                AuditArgs.Add(TEXT("component"), ComponentName);
            }
            if (bSlotWasName && SlotValue->Type == EJson::String)
            {
                AuditArgs.Add(TEXT("slot"), SlotValue->AsString());
            }
            else
            {
                AuditArgs.Add(TEXT("slot"), FString::FromInt(SlotIndex));
            }
            AuditArgs.Add(TEXT("mi"), MiPath);
            AppendAuditAction(AuditActions, TEXT("apply_mi"), AuditArgs);
        }

        if (bComponentModified)
        {
            MeshComponent->MarkRenderStateDirty();
            if (ULevel* Level = TargetActor->GetLevel())
            {
                if (UPackage* Package = Level->GetOutermost())
                {
                    PackagesToSave.Add(Package);
                }
            }
        }
    }

    if (bSaveActors && PackagesToSave.Num() > 0)
    {
        TArray<UPackage*> PackagesArray = PackagesToSave.Array();
        if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesArray, /*bOnlyDirty*/false))
        {
            return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save modified levels"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
    Result->SetArrayField(TEXT("applied"), AppliedItems);
    Result->SetArrayField(TEXT("skipped"), SkippedItems);
    Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, AuditActions));
    return Result;
}

TSharedPtr<FJsonObject> FMaterialApplyTools::RemapMaterialSlots(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    FString MeshObjectPath;
    if (!Params->TryGetStringField(TEXT("meshObjectPath"), MeshObjectPath) || MeshObjectPath.TrimStartAndEnd().IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing meshObjectPath"));
    }

    MeshObjectPath.TrimStartAndEndInline();

    const FString PackagePath = NormalizeContentPath(MeshObjectPath.Contains(TEXT(".")) ? FPackageName::ObjectPathToPackageName(MeshObjectPath) : MeshObjectPath);
    if (PackagePath.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Invalid meshObjectPath"));
    }

    FString PathReason;
    if (!FWriteGate::IsPathAllowed(PackagePath, PathReason))
    {
        return MakeErrorResponse(TEXT("PATH_NOT_ALLOWED"), PathReason);
    }

    const FString ObjectPath = BuildObjectPath(PackagePath);

    UObject* MeshObject = LoadObject<UObject>(nullptr, *ObjectPath);
    if (!MeshObject)
    {
        return MakeErrorResponse(ErrorCodeAssetNotFound, FString::Printf(TEXT("Mesh asset not found: %s"), *MeshObjectPath));
    }

    UStaticMesh* StaticMesh = Cast<UStaticMesh>(MeshObject);
    if (!StaticMesh)
    {
        return MakeErrorResponse(ErrorCodeUnsupportedMesh, TEXT("Only static meshes are supported"));
    }

    StaticMesh->Modify();

    TArray<FStaticMaterial>& StaticMaterials = StaticMesh->GetStaticMaterials();
    TArray<FName> OriginalNames;
    OriginalNames.Reserve(StaticMaterials.Num());
    for (const FStaticMaterial& Material : StaticMaterials)
    {
        OriginalNames.Add(Material.MaterialSlotName);
    }

    const TSharedPtr<FJsonObject>* RenameObject = nullptr;
    bool bHasRename = Params->TryGetObjectField(TEXT("rename"), RenameObject) && RenameObject && RenameObject->IsValid() && (*RenameObject)->Values.Num() > 0;

    TArray<TPair<FName, FName>> AppliedRenames;

    if (bHasRename)
    {
        TSet<FName> PendingNames(OriginalNames);

        for (const auto& Pair : (*RenameObject)->Values)
        {
            const FString& OldNameString = Pair.Key;
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
            {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Rename values must be strings"));
            }

            const FString NewNameString = Pair.Value->AsString();
            const FName OldName(*OldNameString);
            const FName NewName(*NewNameString);

            const int32 Index = OriginalNames.IndexOfByKey(OldName);
            if (Index == INDEX_NONE)
            {
                continue;
            }

            if (PendingNames.Contains(NewName) && NewName != OldName)
            {
                return MakeErrorResponse(ErrorCodeSlotConflict, FString::Printf(TEXT("Slot name conflict: %s"), *NewNameString));
            }

            PendingNames.Remove(OldName);
            PendingNames.Add(NewName);

            StaticMaterials[Index].MaterialSlotName = NewName;
            OriginalNames[Index] = NewName;
            AppliedRenames.Emplace(OldName, NewName);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* ReorderArray = nullptr;
    const bool bHasReorder = Params->TryGetArrayField(TEXT("reorder"), ReorderArray) && ReorderArray && ReorderArray->Num() > 0;

    FString FillMissingName;
    Params->TryGetStringField(TEXT("fillMissingWith"), FillMissingName);
    FName FillMissingFName = FillMissingName.IsEmpty() ? NAME_None : FName(*FillMissingName);

    TArray<int32> SourceIndices;
    TArray<FName> FinalNames = OriginalNames;

    if (bHasReorder)
    {
        if (ReorderArray->Num() != OriginalNames.Num())
        {
            return MakeErrorResponse(ErrorCodeReorderInvalid, TEXT("Reorder array must match slot count"));
        }

        TMap<FName, int32> NameToIndex;
        for (int32 Index = 0; Index < OriginalNames.Num(); ++Index)
        {
            NameToIndex.Add(OriginalNames[Index], Index);
        }

        SourceIndices.Reserve(ReorderArray->Num());
        FinalNames.Reset(ReorderArray->Num());

        for (const TSharedPtr<FJsonValue>& Value : *ReorderArray)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Reorder entries must be strings"));
            }

            const FString SlotNameString = Value->AsString();
            const FName SlotName(*SlotNameString);

            int32* FoundIndex = NameToIndex.Find(SlotName);
            if (!FoundIndex && FillMissingFName != NAME_None)
            {
                FoundIndex = NameToIndex.Find(FillMissingFName);
            }

            if (!FoundIndex)
            {
                return MakeErrorResponse(ErrorCodeReorderInvalid, FString::Printf(TEXT("Reorder reference missing slot: %s"), *SlotNameString));
            }

            SourceIndices.Add(*FoundIndex);
            FinalNames.Add(SlotName);
        }
    }
    else
    {
        SourceIndices.Reserve(StaticMaterials.Num());
        for (int32 Index = 0; Index < StaticMaterials.Num(); ++Index)
        {
            SourceIndices.Add(Index);
        }
    }

    TArray<FStaticMaterial> NewMaterials;
    NewMaterials.SetNum(SourceIndices.Num());

    for (int32 NewIndex = 0; NewIndex < SourceIndices.Num(); ++NewIndex)
    {
        const int32 SourceIndex = SourceIndices[NewIndex];
        if (!StaticMaterials.IsValidIndex(SourceIndex))
        {
            return MakeErrorResponse(ErrorCodeReorderInvalid, TEXT("Reorder index out of range"));
        }

        FStaticMaterial& Destination = NewMaterials[NewIndex];
        Destination = StaticMaterials[SourceIndex];
        if (FinalNames.IsValidIndex(NewIndex))
        {
            Destination.MaterialSlotName = FinalNames[NewIndex];
        }
    }

    TArray<int32> OldToNew;
    OldToNew.Init(INDEX_NONE, StaticMaterials.Num());
    for (int32 NewIndex = 0; NewIndex < SourceIndices.Num(); ++NewIndex)
    {
        const int32 OldIndex = SourceIndices[NewIndex];
        if (!OldToNew.IsValidIndex(OldIndex) || OldToNew[OldIndex] == INDEX_NONE)
        {
            OldToNew[OldIndex] = NewIndex;
        }
    }

    StaticMaterials = NewMaterials;

    FMeshSectionInfoMap& SectionInfoMap = StaticMesh->GetSectionInfoMap();
    for (auto It = SectionInfoMap.Map.CreateIterator(); It; ++It)
    {
        FMeshSectionInfo& Info = It.Value();
        if (OldToNew.IsValidIndex(Info.MaterialIndex) && OldToNew[Info.MaterialIndex] != INDEX_NONE)
        {
            Info.MaterialIndex = OldToNew[Info.MaterialIndex];
        }
    }

    StaticMesh->MarkPackageDirty();
    StaticMesh->PostEditChange();

    const bool bRebindActors = Params->HasField(TEXT("rebindActorsInWorld")) && Params->GetBoolField(TEXT("rebindActorsInWorld"));
    TSet<AActor*> ReboundActors;

    if (bRebindActors)
    {
        if (UWorld* World = GetEditorWorld())
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (!Actor)
                {
                    continue;
                }

                TArray<UStaticMeshComponent*> Components;
                Actor->GetComponents<UStaticMeshComponent>(Components);
                for (UStaticMeshComponent* Component : Components)
                {
                    if (!Component || Component->GetStaticMesh() != StaticMesh)
                    {
                        continue;
                    }

                    Component->Modify();

                    TArray<UMaterialInterface*> CurrentMaterials;
                    const int32 OldMaterialCount = Component->GetNumMaterials();
                    CurrentMaterials.Reserve(OldMaterialCount);
                    for (int32 Index = 0; Index < OldMaterialCount; ++Index)
                    {
                        CurrentMaterials.Add(Component->GetMaterial(Index));
                    }

                    for (int32 NewIndex = 0; NewIndex < SourceIndices.Num(); ++NewIndex)
                    {
                        const int32 SourceIndex = SourceIndices[NewIndex];
                        UMaterialInterface* AppliedMaterial = CurrentMaterials.IsValidIndex(SourceIndex) ? CurrentMaterials[SourceIndex] : nullptr;
                        Component->SetMaterial(NewIndex, AppliedMaterial);
                    }

                    Component->MarkRenderStateDirty();
                    Component->ReregisterComponent();
                    ReboundActors.Add(Actor);
                }
            }
        }
    }

    const bool bSave = !Params->HasField(TEXT("save")) || Params->GetBoolField(TEXT("save"));
    if (bSave)
    {
        if (!UEditorAssetLibrary::SaveAsset(ObjectPath, false))
        {
            return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save mesh asset"));
        }
    }

    TSharedPtr<FJsonObject> SlotChanges = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> RenamedArray;
    for (const TPair<FName, FName>& Rename : AppliedRenames)
    {
        TSharedPtr<FJsonObject> RenameObjectJson = MakeShared<FJsonObject>();
        RenameObjectJson->SetStringField(TEXT("from"), Rename.Key.ToString());
        RenameObjectJson->SetStringField(TEXT("to"), Rename.Value.ToString());
        RenamedArray.Add(MakeShared<FJsonValueObject>(RenameObjectJson));
    }
    SlotChanges->SetArrayField(TEXT("renamed"), RenamedArray);

    TArray<FString> ReorderedNames;
    if (bHasReorder)
    {
        Algo::Transform(FinalNames, ReorderedNames, [](const FName& Name)
        {
            return Name.ToString();
        });
    }
    else
    {
        Algo::Transform(StaticMaterials, ReorderedNames, [](const FStaticMaterial& Material)
        {
            return Material.MaterialSlotName.ToString();
        });
    }

    AppendStringArrayField(SlotChanges, TEXT("reordered"), ReorderedNames);

    TArray<TSharedPtr<FJsonValue>> AuditActions;
    if (AppliedRenames.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> PairsArray;
        for (const TPair<FName, FName>& Rename : AppliedRenames)
        {
            TSharedPtr<FJsonObject> PairObject = MakeShared<FJsonObject>();
            PairObject->SetStringField(TEXT("from"), Rename.Key.ToString());
            PairObject->SetStringField(TEXT("to"), Rename.Value.ToString());
            PairsArray.Add(MakeShared<FJsonValueObject>(PairObject));
        }

        TSharedPtr<FJsonObject> RenameAction = MakeShared<FJsonObject>();
        RenameAction->SetStringField(TEXT("op"), TEXT("rename_slots"));
        RenameAction->SetStringField(TEXT("mesh"), MeshObjectPath);
        RenameAction->SetArrayField(TEXT("pairs"), PairsArray);
        AuditActions.Add(MakeShared<FJsonValueObject>(RenameAction));
    }

    if (bHasReorder && SourceIndices.Num() > 0)
    {
        TSharedPtr<FJsonObject> ReorderAction = MakeShared<FJsonObject>();
        ReorderAction->SetStringField(TEXT("op"), TEXT("reorder_slots"));
        ReorderAction->SetStringField(TEXT("mesh"), MeshObjectPath);
        AppendStringArrayField(ReorderAction, TEXT("order"), ReorderedNames);
        AuditActions.Add(MakeShared<FJsonValueObject>(ReorderAction));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
    Result->SetStringField(TEXT("mesh"), MeshObjectPath);
    Result->SetObjectField(TEXT("slotChanges"), SlotChanges);
    Result->SetNumberField(TEXT("reboundActors"), ReboundActors.Num());
    Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, AuditActions));

    return Result;
}
