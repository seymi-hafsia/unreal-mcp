#include "Assets/AssetCrud.h"
#include "CoreMinimal.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Permissions/WriteGate.h"
#include "SourceControlService.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParams = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodePathNotAllowed = TEXT("PATH_NOT_ALLOWED");
    constexpr const TCHAR* ErrorCodeDirectoryExists = TEXT("DIRECTORY_EXISTS");
    constexpr const TCHAR* ErrorCodeCreateFolderFailed = TEXT("CREATE_FOLDER_FAILED");
    constexpr const TCHAR* ErrorCodeAssetNotFound = TEXT("ASSET_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeAssetExists = TEXT("ASSET_ALREADY_EXISTS");
    constexpr const TCHAR* ErrorCodeSourceControlRequired = TEXT("SOURCE_CONTROL_REQUIRED");
    constexpr const TCHAR* ErrorCodeRenameFailed = TEXT("RENAME_FAILED");
    constexpr const TCHAR* ErrorCodeDeleteFailed = TEXT("DELETE_FAILED");
    constexpr const TCHAR* ErrorCodeHasReferencers = TEXT("HAS_REFERENCERS");
    constexpr const TCHAR* ErrorCodeSaveFailed = TEXT("SAVE_FAILED");

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

    bool IsPathAllowed(const FString& ContentPath, FString& OutReason)
    {
        return FWriteGate::IsPathAllowed(ContentPath, OutReason);
    }

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
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetBoolField(TEXT("success"), true);
        if (Payload.IsValid())
        {
            Response->SetObjectField(TEXT("data"), Payload);
        }
        return Response;
    }

    IAssetRegistry& GetAssetRegistry()
    {
        static FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        return AssetRegistryModule.Get();
    }

    bool EnsureCheckout(const FString& ContentPath, FString& OutErrorMessage)
    {
        TSharedPtr<FJsonObject> CheckoutError;
        if (!FWriteGate::EnsureCheckoutForContentPath(ContentPath, CheckoutError))
        {
            OutErrorMessage = CheckoutError.IsValid() && CheckoutError->HasField(TEXT("message"))
                ? CheckoutError->GetStringField(TEXT("message"))
                : FString(TEXT("Source control checkout required"));
            return false;
        }

        return true;
    }

    bool ConvertPackagesToFiles(const TArray<FString>& PackagePaths, TArray<FString>& OutFiles, FString& OutError)
    {
        if (PackagePaths.Num() == 0)
        {
            return true;
        }

        if (!FSourceControlService::AssetPathsToFiles(PackagePaths, OutFiles, OutError))
        {
            return false;
        }

        return true;
    }

    bool MarkFilesForAdd(const TArray<FString>& Files, FString& OutError)
    {
        if (Files.Num() == 0 || !FSourceControlService::IsEnabled())
        {
            return true;
        }

        TMap<FString, bool> PerFileResult;
        FString OperationError;
        if (!FSourceControlService::MarkForAdd(Files, PerFileResult, OperationError))
        {
            OutError = OperationError;
            return false;
        }

        for (const TPair<FString, bool>& Pair : PerFileResult)
        {
            if (!Pair.Value)
            {
                OutError = OperationError;
                return false;
            }
        }

        return true;
    }

    bool DoesObjectPathNeedSaving(const FString& ObjectPath)
    {
        const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
        if (PackageName.IsEmpty())
        {
            return false;
        }

        if (UPackage* Package = FindPackage(nullptr, *PackageName))
        {
            return UEditorLoadingAndSavingUtils::DoesPackageNeedSaving(Package, true);
        }

        return false;
    }

    bool CollectAssetsForSave(const TArray<FString>& Paths, bool bModifiedOnly, TArray<FString>& OutObjectPaths)
    {
        IAssetRegistry& AssetRegistry = GetAssetRegistry();

        if (Paths.Num() == 0)
        {
            TArray<FAssetData> AllAssets;
            AssetRegistry.GetAllAssets(AllAssets);
            for (const FAssetData& AssetData : AllAssets)
            {
                const FString ObjectPath = AssetData.ToSoftObjectPath().ToString();
                if (!bModifiedOnly || DoesObjectPathNeedSaving(ObjectPath))
                {
                    OutObjectPaths.AddUnique(ObjectPath);
                }
            }
            return true;
        }

        for (const FString& Path : Paths)
        {
            const FName PathName(*Path);
            TArray<FAssetData> PathAssets;
            AssetRegistry.GetAssetsByPath(PathName, PathAssets, true);

            for (const FAssetData& AssetData : PathAssets)
            {
                const FString ObjectPath = AssetData.ToSoftObjectPath().ToString();
                if (!bModifiedOnly || DoesObjectPathNeedSaving(ObjectPath))
                {
                    OutObjectPaths.AddUnique(ObjectPath);
                }
            }
        }

        return true;
    }
}

TSharedPtr<FJsonObject> FAssetCrud::CreateFolder(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    FString RawPath;
    if (!Params->TryGetStringField(TEXT("path"), RawPath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing path parameter"));
    }

    const FString Path = NormalizeContentPath(RawPath);
    FString PathReason;
    if (!IsPathAllowed(Path, PathReason))
    {
        return MakeErrorResponse(ErrorCodePathNotAllowed, PathReason);
    }

    if (!Path.StartsWith(TEXT("/Game/")))
    {
        return MakeErrorResponse(ErrorCodePathNotAllowed, TEXT("Only /Game paths are supported"));
    }

    if (UEditorAssetLibrary::DoesDirectoryExist(Path))
    {
        return MakeErrorResponse(ErrorCodeDirectoryExists, FString::Printf(TEXT("Directory already exists: %s"), *Path));
    }

    if (!UEditorAssetLibrary::MakeDirectory(Path))
    {
        return MakeErrorResponse(ErrorCodeCreateFolderFailed, FString::Printf(TEXT("Failed to create directory: %s"), *Path));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetBoolField(TEXT("created"), true);
    Data->SetStringField(TEXT("path"), Path);
    return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FAssetCrud::Rename(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    FString FromObjectPath;
    FString ToPackagePath;
    if (!Params->TryGetStringField(TEXT("fromObjectPath"), FromObjectPath) ||
        !Params->TryGetStringField(TEXT("toPackagePath"), ToPackagePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing fromObjectPath or toPackagePath"));
    }

    FromObjectPath = NormalizeContentPath(FromObjectPath);
    ToPackagePath = NormalizeContentPath(ToPackagePath);

    FString TargetReason;
    if (!IsPathAllowed(ToPackagePath, TargetReason))
    {
        return MakeErrorResponse(ErrorCodePathNotAllowed, TargetReason);
    }

    FSoftObjectPath FromSoftPath(FromObjectPath);
    if (!FromSoftPath.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, FString::Printf(TEXT("Invalid object path: %s"), *FromObjectPath));
    }

    FString FromPackageName = FPackageName::ObjectPathToPackageName(FromObjectPath);
    if (!FPackageName::IsValidLongPackageName(FromPackageName))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, FString::Printf(TEXT("Invalid package path: %s"), *FromPackageName));
    }

    if (!FPackageName::IsValidLongPackageName(ToPackagePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, FString::Printf(TEXT("Invalid target package path: %s"), *ToPackagePath));
    }

    IAssetRegistry& AssetRegistry = GetAssetRegistry();
    const FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FromSoftPath);
    if (!ExistingAsset.IsValid())
    {
        return MakeErrorResponse(ErrorCodeAssetNotFound, FString::Printf(TEXT("Asset not found: %s"), *FromObjectPath));
    }

    FString Reason;
    if (!IsPathAllowed(FromPackageName, Reason))
    {
        return MakeErrorResponse(ErrorCodePathNotAllowed, Reason);
    }

    const FString NewAssetName = FPackageName::GetLongPackageAssetName(ToPackagePath);
    if (NewAssetName.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, FString::Printf(TEXT("Invalid target package path: %s"), *ToPackagePath));
    }

    const FString ToObjectPath = FString::Printf(TEXT("%s.%s"), *ToPackagePath, *NewAssetName);
    if (UEditorAssetLibrary::DoesAssetExist(ToObjectPath))
    {
        return MakeErrorResponse(ErrorCodeAssetExists, FString::Printf(TEXT("Target asset already exists: %s"), *ToObjectPath));
    }

    FString CheckoutError;
    if (!EnsureCheckout(FromObjectPath, CheckoutError))
    {
        return MakeErrorResponse(ErrorCodeSourceControlRequired, CheckoutError);
    }

    if (!UEditorAssetLibrary::RenameAsset(FromObjectPath, ToObjectPath))
    {
        return MakeErrorResponse(ErrorCodeRenameFailed, TEXT("Failed to rename asset"));
    }

    FString MarkForAddError;
    TArray<FString> NewFiles;
    if (!ConvertPackagesToFiles({ToPackagePath}, NewFiles, MarkForAddError))
    {
        return MakeErrorResponse(ErrorCodeSourceControlRequired, MarkForAddError);
    }

    FString MarkErrorDetail;
    if (!MarkFilesForAdd(NewFiles, MarkErrorDetail) && !MarkErrorDetail.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeSourceControlRequired, MarkErrorDetail);
    }

    bool bRedirectorCreated = false;
    const FAssetData PostRenameData = AssetRegistry.GetAssetByObjectPath(FromSoftPath);
    if (PostRenameData.IsValid())
    {
        bRedirectorCreated = PostRenameData.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName();
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetStringField(TEXT("from"), FromPackageName);
    Data->SetStringField(TEXT("to"), ToPackagePath);
    Data->SetStringField(TEXT("objectPath"), ToObjectPath);
    Data->SetBoolField(TEXT("redirectorCreated"), bRedirectorCreated);
    return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FAssetCrud::Delete(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    const TArray<TSharedPtr<FJsonValue>>* ObjectPathsJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("objectPaths"), ObjectPathsJson) || !ObjectPathsJson)
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing objectPaths array"));
    }

    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    IAssetRegistry& AssetRegistry = GetAssetRegistry();
    TArray<FString> ObjectPaths;
    ObjectPaths.Reserve(ObjectPathsJson->Num());

    for (const TSharedPtr<FJsonValue>& Value : *ObjectPathsJson)
    {
        if (Value->Type != EJson::String)
        {
            continue;
        }

        FString Normalized = NormalizeContentPath(Value->AsString());
        if (!Normalized.IsEmpty())
        {
            ObjectPaths.Add(Normalized);
        }
    }

    if (ObjectPaths.Num() == 0)
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("No object paths supplied"));
    }

    TArray<TSharedPtr<FJsonValue>> Results;

    for (const FString& ObjectPath : ObjectPaths)
    {
        FSoftObjectPath SoftPath(ObjectPath);
        if (!SoftPath.IsValid())
        {
            return MakeErrorResponse(ErrorCodeInvalidParams, FString::Printf(TEXT("Invalid object path: %s"), *ObjectPath));
        }

        const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
        FString PathReason;
        if (!IsPathAllowed(PackageName, PathReason))
        {
            return MakeErrorResponse(ErrorCodePathNotAllowed, PathReason);
        }

        const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(SoftPath);
        if (!AssetData.IsValid())
        {
            return MakeErrorResponse(ErrorCodeAssetNotFound, FString::Printf(TEXT("Asset not found: %s"), *ObjectPath));
        }

        if (!bForce)
        {
            using namespace UE::AssetRegistry;
            TArray<FName> Referencers;
            FDependencyQuery Query;
            Query.Flags = EDependencyFlags::Hard;
            AssetRegistry.GetReferencers(AssetData.PackageName, Referencers, EDependencyCategory::Package, Query);

            Referencers.RemoveAll([&](const FName& Ref)
            {
                return Ref == AssetData.PackageName;
            });

            if (Referencers.Num() > 0)
            {
                return MakeErrorResponse(ErrorCodeHasReferencers, FString::Printf(TEXT("Asset has referencers: %s"), *Referencers[0].ToString()));
            }
        }

        FString CheckoutError;
        if (!EnsureCheckout(ObjectPath, CheckoutError))
        {
            return MakeErrorResponse(ErrorCodeSourceControlRequired, CheckoutError);
        }

        if (!UEditorAssetLibrary::DeleteAsset(ObjectPath))
        {
            return MakeErrorResponse(ErrorCodeDeleteFailed, FString::Printf(TEXT("Failed to delete asset: %s"), *ObjectPath));
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("objectPath"), ObjectPath);
        Entry->SetBoolField(TEXT("deleted"), true);
        Results.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetArrayField(TEXT("results"), Results);
    return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FAssetCrud::FixRedirectors(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    const TArray<TSharedPtr<FJsonValue>>* PathsJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("paths"), PathsJson) || !PathsJson)
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing paths array"));
    }

    bool bRecursive = true;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);

    IAssetRegistry& AssetRegistry = GetAssetRegistry();
    TArray<UObjectRedirector*> RedirectorsToFix;
    TArray<FString> FixedPaths;

    for (const TSharedPtr<FJsonValue>& Value : *PathsJson)
    {
        if (Value->Type != EJson::String)
        {
            continue;
        }

        const FString Normalized = NormalizeContentPath(Value->AsString());
        FString PathReason;
        if (!IsPathAllowed(Normalized, PathReason))
        {
            return MakeErrorResponse(ErrorCodePathNotAllowed, PathReason);
        }

        const FName PathName(*Normalized);
        TArray<FAssetData> PathAssets;
        AssetRegistry.GetAssetsByPath(PathName, PathAssets, bRecursive);

        for (const FAssetData& AssetData : PathAssets)
        {
            if (AssetData.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName())
            {
                if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(AssetData.GetAsset()))
                {
                    RedirectorsToFix.Add(Redirector);
                }
            }
        }
    }

    if (RedirectorsToFix.Num() == 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetNumberField(TEXT("fixedCount"), 0);
        Data->SetArrayField(TEXT("fixed"), {});
        return MakeSuccessResponse(Data);
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    AssetToolsModule.Get().FixupReferencers(RedirectorsToFix);

    for (UObjectRedirector* Redirector : RedirectorsToFix)
    {
        if (Redirector)
        {
            FixedPaths.Add(Redirector->GetPathName());
        }
    }

    TArray<TSharedPtr<FJsonValue>> FixedJson;
    for (const FString& Path : FixedPaths)
    {
        FixedJson.Add(MakeShared<FJsonValueString>(Path));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetNumberField(TEXT("fixedCount"), FixedPaths.Num());
    Data->SetArrayField(TEXT("fixed"), FixedJson);
    return MakeSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FAssetCrud::SaveAll(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FString> NormalizedPaths;
    bool bModifiedOnly = true;

    if (Params.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* PathsJson = nullptr;
        if (Params->TryGetArrayField(TEXT("paths"), PathsJson) && PathsJson)
        {
            for (const TSharedPtr<FJsonValue>& Value : *PathsJson)
            {
                if (Value->Type != EJson::String)
                {
                    continue;
                }

                const FString Normalized = NormalizeContentPath(Value->AsString());
                FString PathReason;
                if (!IsPathAllowed(Normalized, PathReason))
                {
                    return MakeErrorResponse(ErrorCodePathNotAllowed, PathReason);
                }

                NormalizedPaths.Add(Normalized);
            }
        }

        bool bValue;
        if (Params->TryGetBoolField(TEXT("modifiedOnly"), bValue))
        {
            bModifiedOnly = bValue;
        }
    }

    TArray<FString> ObjectPaths;
    if (!CollectAssetsForSave(NormalizedPaths, bModifiedOnly, ObjectPaths))
    {
        return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to collect packages"));
    }

    TArray<TSharedPtr<FJsonValue>> SavedPackagesJson;
    int32 SavedCount = 0;

    for (const FString& ObjectPath : ObjectPaths)
    {
        const bool bSaved = UEditorAssetLibrary::SaveAsset(ObjectPath, bModifiedOnly);
        if (bSaved)
        {
            SavedCount++;
            const FString PackagePath = FPackageName::ObjectPathToPackageName(ObjectPath);
            SavedPackagesJson.Add(MakeShared<FJsonValueString>(PackagePath));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetNumberField(TEXT("savedCount"), SavedCount);
    Data->SetArrayField(TEXT("savedPackages"), SavedPackagesJson);
    return MakeSuccessResponse(Data);
}
