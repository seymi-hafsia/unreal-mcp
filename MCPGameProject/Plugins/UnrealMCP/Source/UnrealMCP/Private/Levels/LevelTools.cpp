#include "Levels/LevelTools.h"

#include "Commands/UnrealMCPCommonUtils.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "FileHelpers.h"              // FEditorFileUtils, UEditorLoadingAndSavingUtils
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Permissions/WriteGate.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "WorldPartition/DataLayer/DataLayerSubsystem.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParams = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodeEditorUnavailable = TEXT("EDITOR_UNAVAILABLE");
    constexpr const TCHAR* ErrorCodeAssetNotFound = TEXT("ASSET_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeSaveFailed = TEXT("SAVE_FAILED");
    constexpr const TCHAR* ErrorCodeMapOpenFailed = TEXT("MAP_OPEN_FAILED");
    constexpr const TCHAR* ErrorCodeHasUnsavedChanges = TEXT("HAS_UNSAVED_CHANGES");
    constexpr const TCHAR* ErrorCodeSublevelNotFound = TEXT("SUBLEVEL_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeUnloadFailed = TEXT("UNLOAD_FAILED");
    constexpr const TCHAR* ErrorCodeStreamingFailed = TEXT("STREAMING_FAILED");
    constexpr const TCHAR* ErrorCodeSourceControlRequired = TEXT("SOURCE_CONTROL_REQUIRED");

    UWorld* GetEditorWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }

        if (FWorldContext* PieContext = GEditor->GetPIEWorldContext())
        {
            if (PieContext->World())
            {
                return PieContext->World();
            }
        }

        return GEditor->GetEditorWorldContext().World();
    }

    FString NormalizeMapPath(const FString& InPath)
    {
        FString Trimmed = InPath;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return Trimmed;
        }

        FString Normalized = Trimmed;
        if (!Trimmed.StartsWith(TEXT("/")))
        {
            Normalized = FString::Printf(TEXT("/Game/%s"), *Trimmed);
        }

        if (Normalized.Contains(TEXT(".")))
        {
            if (FPackageName::IsValidObjectPath(Normalized))
            {
                return Normalized;
            }
        }

        if (!FPackageName::IsValidLongPackageName(Normalized))
        {
            return FString();
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(Normalized);
        return FString::Printf(TEXT("%s.%s"), *Normalized, *AssetName);
    }

    FString ObjectPathToPackagePath(const FString& ObjectPath)
    {
        if (ObjectPath.Contains(TEXT(".")))
        {
            return FPackageName::ObjectPathToPackageName(ObjectPath);
        }

        return ObjectPath;
    }

    FString MakeErrorMessage(const FString& Message)
    {
        return Message.IsEmpty() ? FString(TEXT("Unknown error")) : Message;
    }

    TSharedPtr<FJsonObject> MakeError(const FString& Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(MakeErrorMessage(Message));
        Error->SetStringField(TEXT("errorCode"), Code);
        return Error;
    }

    FString GetLevelObjectPath(const ULevel* Level)
    {
        if (!Level)
        {
            return FString();
        }

        if (const UPackage* Package = Level->GetOutermost())
        {
            const FString PackageName = Package->GetName();
            const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
            return FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }

        return FString();
    }

    void CollectOpenMapObjectPaths(UWorld* World, TArray<FString>& OutMaps)
    {
        if (!World)
        {
            return;
        }

        if (ULevel* PersistentLevel = World->PersistentLevel)
        {
            const FString ObjectPath = GetLevelObjectPath(PersistentLevel);
            if (!ObjectPath.IsEmpty())
            {
                OutMaps.AddUnique(ObjectPath);
            }
        }

        for (ULevelStreaming* Streaming : World->GetStreamingLevels())
        {
            if (!Streaming)
            {
                continue;
            }

            if (!Streaming->GetLoadedLevel())
            {
                continue;
            }

            const FString PackageName = Streaming->GetWorldAssetPackageName();
            if (PackageName.IsEmpty())
            {
                continue;
            }

            const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
            const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
            OutMaps.AddUnique(ObjectPath);
        }
    }

    void AppendAuditAction(TArray<TSharedPtr<FJsonValue>>& AuditActions, const FString& Op, const TFunctionRef<void(TSharedPtr<FJsonObject>&)>& Builder)
    {
        TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
        Action->SetStringField(TEXT("op"), Op);
        Builder(Action);
        AuditActions.Add(MakeShared<FJsonValueObject>(Action));
    }

    bool EnsureCheckout(const FString& PackagePath, FString& OutError)
    {
        TSharedPtr<FJsonObject> CheckoutError;
        if (!FWriteGate::EnsureCheckoutForContentPath(PackagePath, CheckoutError))
        {
            if (CheckoutError.IsValid())
            {
                if (CheckoutError->HasField(TEXT("message")))
                {
                    OutError = CheckoutError->GetStringField(TEXT("message"));
                }
                if (OutError.IsEmpty() && CheckoutError->HasField(TEXT("code")))
                {
                    OutError = CheckoutError->GetStringField(TEXT("code"));
                }
            }
            return false;
        }

        return true;
    }

    bool PackageMatchesIdentifier(const FString& PackageName, const FString& Identifier)
    {
        if (Identifier.IsEmpty())
        {
            return false;
        }

        const FString NormalizedIdentifier = NormalizeMapPath(Identifier);
        if (NormalizedIdentifier.IsEmpty())
        {
            return false;
        }

        const FString IdentifierPackage = ObjectPathToPackagePath(NormalizedIdentifier);
        if (IdentifierPackage.IsEmpty())
        {
            return false;
        }

        if (PackageName.Equals(IdentifierPackage, ESearchCase::IgnoreCase))
        {
            return true;
        }

        const FString ShortName = FPackageName::GetLongPackageAssetName(PackageName);
        if (ShortName.Equals(Identifier, ESearchCase::IgnoreCase))
        {
            return true;
        }

        return false;
    }

    ULevelStreaming* FindStreamingLevel(UWorld* World, const FString& Identifier)
    {
        if (!World)
        {
            return nullptr;
        }

        for (ULevelStreaming* Streaming : World->GetStreamingLevels())
        {
            if (!Streaming)
            {
                continue;
            }

            const FString PackageName = Streaming->GetWorldAssetPackageName();
            if (PackageMatchesIdentifier(PackageName, Identifier))
            {
                return Streaming;
            }

            if (Streaming->GetWorldAsset() && Streaming->GetWorldAsset()->GetName().Equals(Identifier, ESearchCase::IgnoreCase))
            {
                return Streaming;
            }
        }

        return nullptr;
    }

    void FlushStreaming(UWorld* World)
    {
        if (!World)
        {
            return;
        }

        World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
        if (GEditor)
        {
            GEditor->RedrawAllViewports(false);
        }
    }

    void TickUntil(const TFunctionRef<bool()>& Condition, float TimeoutSeconds)
    {
        if (!GEditor)
        {
            return;
        }

        const double StartTime = FPlatformTime::Seconds();
        while (!Condition())
        {
            const double Elapsed = FPlatformTime::Seconds() - StartTime;
            if (Elapsed > TimeoutSeconds)
            {
                break;
            }

            GEditor->Tick(0.f, false);
            FPlatformProcess::Sleep(0.01f);
        }
    }

    bool ResolveDataLayerNames(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FName>& OutNames)
    {
        if (!Params.IsValid())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* LayersJson = nullptr;
        if (!Params->TryGetArrayField(FieldName, LayersJson) || !LayersJson)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *LayersJson)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                continue;
            }

            FString LayerName = Value->AsString();
            LayerName.TrimStartAndEndInline();
            if (!LayerName.IsEmpty())
            {
                OutNames.Add(FName(*LayerName));
            }
        }

        return OutNames.Num() > 0;
    }

    bool SetDataLayersState(UWorld* World, const TArray<FName>& DataLayers, bool bLoaded, bool bActivateOnly, TArray<FName>& OutProcessed)
    {
        if (!World)
        {
            return false;
        }

        UDataLayerSubsystem* DataLayerSubsystem = World->GetSubsystem<UDataLayerSubsystem>();
        if (!DataLayerSubsystem)
        {
            return false;
        }

        bool bAnyChanged = false;
        for (const FName& LayerName : DataLayers)
        {
            if (!LayerName.IsNone())
            {
                DataLayerSubsystem->SetDataLayerLoaded(LayerName, bLoaded);
                DataLayerSubsystem->SetDataLayerActive(LayerName, bLoaded || bActivateOnly);
                OutProcessed.Add(LayerName);
                bAnyChanged = true;
            }
        }

        return bAnyChanged;
    }

    bool DataLayerExists(UWorld* World, const FName& LayerName)
    {
        if (!World)
        {
            return false;
        }

        if (UDataLayerSubsystem* DataLayerSubsystem = World->GetSubsystem<UDataLayerSubsystem>())
        {
            return DataLayerSubsystem->GetDataLayerInstance(LayerName) != nullptr;
        }

        return false;
    }

    void AppendNamesToJsonArray(const TArray<FName>& Names, TArray<TSharedPtr<FJsonValue>>& OutArray)
    {
        for (const FName& Name : Names)
        {
            if (!Name.IsNone())
            {
                OutArray.Add(MakeShared<FJsonValueString>(Name.ToString()));
            }
        }
    }
}

TSharedPtr<FJsonObject> FLevelTools::SaveOpen(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return MakeError(ErrorCodeEditorUnavailable, TEXT("Editor instance unavailable"));
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return MakeError(ErrorCodeEditorUnavailable, TEXT("No active editor world"));
    }

    bool bModifiedOnly = true;
    bool bShowDialog = false;
    bool bSaveExternalActors = true;

    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("modifiedOnly"), bModifiedOnly);
        Params->TryGetBoolField(TEXT("showDialog"), bShowDialog);
        Params->TryGetBoolField(TEXT("saveExternalActors"), bSaveExternalActors);
    }

    UE_UNUSED(bShowDialog);

    TArray<FString> RequestedMaps;
    if (Params.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* MapsArray = nullptr;
        if (Params->TryGetArrayField(TEXT("maps"), MapsArray) && MapsArray)
        {
            for (const TSharedPtr<FJsonValue>& Value : *MapsArray)
            {
                if (!Value.IsValid() || Value->Type != EJson::String)
                {
                    continue;
                }

                const FString Normalized = NormalizeMapPath(Value->AsString());
                if (!Normalized.IsEmpty())
                {
                    RequestedMaps.AddUnique(Normalized);
                }
            }
        }
    }

    if (RequestedMaps.Num() == 0)
    {
        CollectOpenMapObjectPaths(World, RequestedMaps);
    }

    if (RequestedMaps.Num() == 0)
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("No maps available to save"));
    }

    TArray<FString> SavedMaps;
    TArray<FString> SkippedMaps;
    TArray<UPackage*> PackagesToSave;
    TArray<TSharedPtr<FJsonValue>> AuditActions;

    const bool bDryRun = FWriteGate::ShouldDryRun();

    for (const FString& MapObjectPath : RequestedMaps)
    {
        const FString PackagePath = ObjectPathToPackagePath(MapObjectPath);
        FString PathReason;
        if (!FWriteGate::IsPathAllowed(PackagePath, PathReason))
        {
            return MakeError(TEXT("PATH_NOT_ALLOWED"), PathReason);
        }

        UPackage* Package = FindPackage(nullptr, *PackagePath);
        if (!Package)
        {
            Package = LoadPackage(nullptr, *PackagePath, LOAD_None);
        }

        if (!Package)
        {
            return MakeError(ErrorCodeAssetNotFound, FString::Printf(TEXT("Map package not found: %s"), *PackagePath));
        }

        const bool bIsDirty = Package->IsDirty();
        if (bModifiedOnly && !bIsDirty)
        {
            SkippedMaps.Add(MapObjectPath);
            AppendAuditAction(AuditActions, TEXT("save_map"), [MapObjectPath](TSharedPtr<FJsonObject>& Action)
            {
                Action->SetStringField(TEXT("map"), ObjectPathToPackagePath(MapObjectPath));
                Action->SetBoolField(TEXT("executed"), false);
                Action->SetStringField(TEXT("reason"), TEXT("not_dirty"));
            });
            continue;
        }

        if (!bDryRun)
        {
            FString CheckoutError;
            if (!EnsureCheckout(PackagePath, CheckoutError))
            {
                return MakeError(ErrorCodeSourceControlRequired, CheckoutError);
            }
        }

        PackagesToSave.AddUnique(Package);
        SavedMaps.Add(MapObjectPath);

        AppendAuditAction(AuditActions, TEXT("save_map"), [MapObjectPath, bDryRun](TSharedPtr<FJsonObject>& Action)
        {
            Action->SetStringField(TEXT("map"), ObjectPathToPackagePath(MapObjectPath));
            Action->SetBoolField(TEXT("executed"), !bDryRun);
        });
    }

    bool bSaveOk = true;

    if (!bDryRun && PackagesToSave.Num() > 0)
    {
        bSaveOk = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty*/ bModifiedOnly);
    }

    if (!bSaveOk)
    {
        return MakeError(ErrorCodeSaveFailed, TEXT("Failed to save map packages"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetNumberField(TEXT("savedCount"), bDryRun ? 0 : PackagesToSave.Num());

    TArray<TSharedPtr<FJsonValue>> SavedJson;
    if (!bDryRun)
    {
        for (const FString& Saved : SavedMaps)
        {
            SavedJson.Add(MakeShared<FJsonValueString>(Saved));
        }
    }

    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    for (const FString& Skipped : SkippedMaps)
    {
        SkippedJson.Add(MakeShared<FJsonValueString>(Skipped));
    }

    Data->SetArrayField(TEXT("saved"), SavedJson);
    Data->SetArrayField(TEXT("skipped"), SkippedJson);

    if (bSaveExternalActors)
    {
        AppendAuditAction(AuditActions, TEXT("save_external_actors"), [bDryRun, WorldPtr = World](TSharedPtr<FJsonObject>& Action)
        {
            Action->SetBoolField(TEXT("executed"), !bDryRun && WorldPtr && WorldPtr->GetWorldPartition() != nullptr);
        });
    }

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), bDryRun);
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FLevelTools::Load(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    if (!GEditor)
    {
        return MakeError(ErrorCodeEditorUnavailable, TEXT("Editor instance unavailable"));
    }

    FString MapPath;
    if (!Params->TryGetStringField(TEXT("mapPath"), MapPath))
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("Missing mapPath parameter"));
    }

    const FString NormalizedObjectPath = NormalizeMapPath(MapPath);
    if (NormalizedObjectPath.IsEmpty())
    {
        return MakeError(ErrorCodeAssetNotFound, TEXT("Invalid map path"));
    }

    const FString PackagePath = ObjectPathToPackagePath(NormalizedObjectPath);
    if (!FPackageName::DoesPackageExist(PackagePath))
    {
        return MakeError(ErrorCodeAssetNotFound, FString::Printf(TEXT("Map asset not found: %s"), *PackagePath));
    }

    bool bMakeCurrent = true;
    bool bDiscardUnsaved = false;
    FString LoadSublevelsMode(TEXT("none"));

    Params->TryGetBoolField(TEXT("makeCurrent"), bMakeCurrent);
    Params->TryGetBoolField(TEXT("discardUnsaved"), bDiscardUnsaved);
    Params->TryGetStringField(TEXT("loadSublevels"), LoadSublevelsMode);

    LoadSublevelsMode = LoadSublevelsMode.IsEmpty() ? TEXT("none") : LoadSublevelsMode.ToLower();

    TArray<FString> SublevelNames;
    const TArray<TSharedPtr<FJsonValue>>* SublevelsArray = nullptr;
    if (Params->TryGetArrayField(TEXT("sublevels"), SublevelsArray) && SublevelsArray)
    {
        for (const TSharedPtr<FJsonValue>& Value : *SublevelsArray)
        {
            if (Value.IsValid() && Value->Type == EJson::String)
            {
                FString Name = Value->AsString();
                Name.TrimStartAndEndInline();
                if (!Name.IsEmpty())
                {
                    SublevelNames.AddUnique(Name);
                }
            }
        }
    }

    if (!bDiscardUnsaved)
    {
        bool bHasDirty = false;
        for (const FWorldContext& Context : GEditor->GetWorldContexts())
        {
            if (UWorld* World = Context.World())
            {
                if (ULevel* PersistentLevel = World->PersistentLevel)
                {
                    if (PersistentLevel->GetOutermost() && PersistentLevel->GetOutermost()->IsDirty())
                    {
                        bHasDirty = true;
                        break;
                    }
                }

                for (ULevelStreaming* Streaming : World->GetStreamingLevels())
                {
                    if (Streaming && Streaming->GetLoadedLevel())
                    {
                        if (ULevel* Level = Streaming->GetLoadedLevel())
                        {
                            if (Level->GetOutermost() && Level->GetOutermost()->IsDirty())
                            {
                                bHasDirty = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (bHasDirty)
            {
                break;
            }
        }

        if (bHasDirty)
        {
            return MakeError(ErrorCodeHasUnsavedChanges, TEXT("There are unsaved maps"));
        }
    }

    const bool bDryRun = FWriteGate::ShouldDryRun();

    if (!bDryRun)
    {
        if (!FEditorFileUtils::LoadMap(PackagePath, false, true))
        {
            return MakeError(ErrorCodeMapOpenFailed, TEXT("Failed to open map"));
        }
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return MakeError(ErrorCodeMapOpenFailed, TEXT("Failed to resolve editor world"));
    }

    TArray<FString> LoadedSublevels;
    TArray<ULevelStreaming*> StreamingTargets;
    TArray<FName> DataLayersToLoad;

    const bool bLoadAll = LoadSublevelsMode == TEXT("all");
    const bool bByNames = LoadSublevelsMode == TEXT("bynames");

    for (ULevelStreaming* Streaming : World->GetStreamingLevels())
    {
        if (!Streaming)
        {
            continue;
        }

        const FString PackageName = Streaming->GetWorldAssetPackageName();
        const FString ShortName = FPackageName::GetLongPackageAssetName(PackageName);
        const bool bShouldLoad = bLoadAll || (bByNames && (SublevelNames.Contains(ShortName) || SublevelNames.Contains(PackageName)));
        if (!bShouldLoad)
        {
            continue;
        }

        StreamingTargets.Add(Streaming);
        LoadedSublevels.AddUnique(ShortName);
    }

    if (World->GetWorldPartition() && bByNames)
    {
        for (const FString& Name : SublevelNames)
        {
            const FName LayerName(*Name);
            if (!LayerName.IsNone() && DataLayerExists(World, LayerName))
            {
                DataLayersToLoad.AddUnique(LayerName);
                LoadedSublevels.AddUnique(Name);
            }
        }
    }

    const bool bShouldTransact = !bDryRun && (StreamingTargets.Num() > 0 || DataLayersToLoad.Num() > 0);
    if (bShouldTransact)
    {
        FScopedTransaction Transaction(TEXT("MCP Levels v1"));

        for (ULevelStreaming* StreamingTarget : StreamingTargets)
        {
            if (StreamingTarget)
            {
                StreamingTarget->SetShouldBeLoaded(true);
                StreamingTarget->SetShouldBeVisible(true);
            }
        }

        if (DataLayersToLoad.Num() > 0)
        {
            TArray<FName> Processed;
            if (!SetDataLayersState(World, DataLayersToLoad, /*bLoaded*/ true, /*bActivateOnly*/ false, Processed))
            {
                return MakeError(ErrorCodeStreamingFailed, TEXT("Failed to load data layers"));
            }
        }
    }

    if (!bDryRun && (StreamingTargets.Num() > 0 || DataLayersToLoad.Num() > 0))
    {
        FlushStreaming(World);
    }

    if (!bDryRun && bMakeCurrent)
    {
        if (ULevel* Persistent = World->PersistentLevel)
        {
            World->SetCurrentLevel(Persistent);
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetStringField(TEXT("opened"), NormalizedObjectPath);

    TArray<TSharedPtr<FJsonValue>> LoadedJson;
    for (const FString& Name : LoadedSublevels)
    {
        LoadedJson.Add(MakeShared<FJsonValueString>(Name));
    }
    Data->SetArrayField(TEXT("loadedSublevels"), LoadedJson);

    TArray<TSharedPtr<FJsonValue>> AuditActions;
    AppendAuditAction(AuditActions, TEXT("open_map"), [PackagePath, bDryRun](TSharedPtr<FJsonObject>& Action)
    {
        Action->SetStringField(TEXT("path"), PackagePath);
        Action->SetBoolField(TEXT("executed"), !bDryRun);
    });

    if (LoadedSublevels.Num() > 0)
    {
        AppendAuditAction(AuditActions, TEXT("load_sublevels"), [&LoadedSublevels, bDryRun](TSharedPtr<FJsonObject>& Action)
        {
            Action->SetNumberField(TEXT("count"), LoadedSublevels.Num());
            Action->SetBoolField(TEXT("executed"), !bDryRun);
        });
    }

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), bDryRun);
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FLevelTools::Unload(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    if (!GEditor)
    {
        return MakeError(ErrorCodeEditorUnavailable, TEXT("Editor instance unavailable"));
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return MakeError(ErrorCodeEditorUnavailable, TEXT("No active editor world"));
    }

    const TArray<TSharedPtr<FJsonValue>>* SublevelsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("sublevels"), SublevelsArray) || !SublevelsArray || SublevelsArray->Num() == 0)
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("No sublevels specified"));
    }

    bool bAllowMissing = true;
    Params->TryGetBoolField(TEXT("allowMissing"), bAllowMissing);

    TArray<FString> Identifiers;
    for (const TSharedPtr<FJsonValue>& Value : *SublevelsArray)
    {
        if (Value.IsValid() && Value->Type == EJson::String)
        {
            FString Identifier = Value->AsString();
            Identifier.TrimStartAndEndInline();
            if (!Identifier.IsEmpty())
            {
                Identifiers.AddUnique(Identifier);
            }
        }
    }

    if (Identifiers.Num() == 0)
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("No valid sublevels provided"));
    }

    const bool bDryRun = FWriteGate::ShouldDryRun();

    TArray<FString> Unloaded;
    TArray<FString> NotFound;
    TArray<TSharedPtr<FJsonValue>> AuditActions;

    TArray<FName> PendingDataLayers;
    TArray<ULevelStreaming*> StreamingTargets;

    const bool bHasWorldPartition = World->GetWorldPartition() != nullptr;

    for (const FString& Identifier : Identifiers)
    {
        ULevelStreaming* Streaming = FindStreamingLevel(World, Identifier);
        if (Streaming)
        {
            StreamingTargets.Add(Streaming);
            Unloaded.Add(Identifier);
            AppendAuditAction(AuditActions, TEXT("unload"), [&Identifier, bDryRun](TSharedPtr<FJsonObject>& Action)
            {
                Action->SetStringField(TEXT("name"), Identifier);
                Action->SetBoolField(TEXT("executed"), !bDryRun);
            });

            continue;
        }

        if (bHasWorldPartition)
        {
            const FName LayerName(*Identifier);
            if (DataLayerExists(World, LayerName))
            {
                PendingDataLayers.Add(LayerName);
                Unloaded.Add(Identifier);
                AppendAuditAction(AuditActions, TEXT("unload"), [&Identifier, bDryRun](TSharedPtr<FJsonObject>& Action)
                {
                    Action->SetStringField(TEXT("name"), Identifier);
                    Action->SetBoolField(TEXT("executed"), !bDryRun);
                });
                continue;
            }
        }

        NotFound.Add(Identifier);
    }

    if (NotFound.Num() > 0 && !bAllowMissing)
    {
        return MakeError(ErrorCodeSublevelNotFound, TEXT("One or more sublevels not found"));
    }

    if (!bDryRun && (StreamingTargets.Num() > 0 || PendingDataLayers.Num() > 0))
    {
        FScopedTransaction Transaction(TEXT("MCP Levels v1"));

        for (ULevelStreaming* StreamingTarget : StreamingTargets)
        {
            if (StreamingTarget)
            {
                StreamingTarget->SetShouldBeLoaded(false);
                StreamingTarget->SetShouldBeVisible(false);
            }
        }

        if (PendingDataLayers.Num() > 0)
        {
            TArray<FName> Processed;
            if (!SetDataLayersState(World, PendingDataLayers, /*bLoaded*/ false, /*bActivateOnly*/ false, Processed))
            {
                return MakeError(ErrorCodeUnloadFailed, TEXT("Failed to update data layers"));
            }
        }

        FlushStreaming(World);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);

    TArray<TSharedPtr<FJsonValue>> UnloadedJson;
    for (const FString& Name : Unloaded)
    {
        UnloadedJson.Add(MakeShared<FJsonValueString>(Name));
    }
    Data->SetArrayField(TEXT("unloaded"), UnloadedJson);

    TArray<TSharedPtr<FJsonValue>> NotFoundJson;
    for (const FString& Name : NotFound)
    {
        NotFoundJson.Add(MakeShared<FJsonValueString>(Name));
    }
    Data->SetArrayField(TEXT("notFound"), NotFoundJson);

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), bDryRun);
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FLevelTools::StreamSublevel(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    if (!GEditor)
    {
        return MakeError(ErrorCodeEditorUnavailable, TEXT("Editor instance unavailable"));
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return MakeError(ErrorCodeEditorUnavailable, TEXT("No active editor world"));
    }

    FString TargetName;
    if (!Params->TryGetStringField(TEXT("name"), TargetName))
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("Missing name parameter"));
    }

    TargetName.TrimStartAndEndInline();
    if (TargetName.IsEmpty())
    {
        return MakeError(ErrorCodeInvalidParams, TEXT("Empty sublevel name"));
    }

    bool bLoad = true;
    bool bBlockUntilVisible = true;
    Params->TryGetBoolField(TEXT("load"), bLoad);
    Params->TryGetBoolField(TEXT("blockUntilVisible"), bBlockUntilVisible);

    const bool bDryRun = FWriteGate::ShouldDryRun();

    ULevelStreaming* Streaming = FindStreamingLevel(World, TargetName);
    bool bResolved = false;
    TArray<FName> TargetDataLayers;
    bool bActivateOnly = false;

    if (!Streaming)
    {
        if (World->GetWorldPartition())
        {
            const TSharedPtr<FJsonObject>* WorldPartitionOptions = nullptr;
            if (Params->TryGetObjectField(TEXT("worldPartition"), WorldPartitionOptions) && WorldPartitionOptions && WorldPartitionOptions->IsValid())
            {
                const TSharedPtr<FJsonObject>& Options = *WorldPartitionOptions;
                ResolveDataLayerNames(Options, TEXT("dataLayers"), TargetDataLayers);
                Options->TryGetBoolField(TEXT("activateOnly"), bActivateOnly);
                TargetDataLayers.RemoveAll([World](const FName& Name)
                {
                    return Name.IsNone() || !DataLayerExists(World, Name);
                });
            }

            if (TargetDataLayers.Num() == 0)
            {
                const FName LayerName(*TargetName);
                if (!LayerName.IsNone() && DataLayerExists(World, LayerName))
                {
                    TargetDataLayers.Add(LayerName);
                }
            }
        }
    }
    else
    {
        TargetDataLayers.Reset();
    }

    if (!Streaming && TargetDataLayers.Num() == 0)
    {
        return MakeError(ErrorCodeSublevelNotFound, TEXT("Sublevel or data layer not found"));
    }

    if (!bDryRun)
    {
        FScopedTransaction Transaction(TEXT("MCP Levels v1"));
        bool bChanged = false;

        if (Streaming)
        {
            Streaming->SetShouldBeLoaded(bLoad);
            Streaming->SetShouldBeVisible(bLoad);
            bChanged = true;
        }
        else if (TargetDataLayers.Num() > 0)
        {
            TArray<FName> Processed;
            if (!SetDataLayersState(World, TargetDataLayers, bLoad, bActivateOnly, Processed))
            {
                return MakeError(ErrorCodeStreamingFailed, TEXT("Failed to update data layer state"));
            }
            bChanged = Processed.Num() > 0;
        }

        if (bChanged)
        {
            FlushStreaming(World);
            if (bBlockUntilVisible && bLoad)
            {
                TickUntil([World, Streaming, &TargetDataLayers]()
                {
                    if (Streaming)
                    {
                        return Streaming->IsLevelVisible();
                    }

                    if (TargetDataLayers.Num() > 0)
                    {
                        if (UDataLayerSubsystem* DataLayerSubsystem = World->GetSubsystem<UDataLayerSubsystem>())
                        {
                            for (const FName& LayerName : TargetDataLayers)
                            {
                                if (!DataLayerSubsystem->IsDataLayerLoaded(LayerName))
                                {
                                    return false;
                                }
                            }
                            return true;
                        }
                    }

                    return true;
                }, 5.0f);
            }
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetStringField(TEXT("action"), bLoad ? TEXT("loaded") : TEXT("unloaded"));
    Data->SetStringField(TEXT("target"), TargetName);

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), bDryRun);
    TArray<TSharedPtr<FJsonValue>> AuditActions;
    AppendAuditAction(AuditActions, TEXT("stream"), [&TargetName, bLoad, bDryRun](TSharedPtr<FJsonObject>& Action)
    {
        Action->SetStringField(TEXT("target"), TargetName);
        Action->SetBoolField(TEXT("load"), bLoad);
        Action->SetBoolField(TEXT("executed"), !bDryRun);
    });
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}
