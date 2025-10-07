#include "Materials/MaterialInstanceTools.h"
#include "CoreMinimal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "String/LexFromString.h"
#include "String/LexToString.h"
#include "Permissions/WriteGate.h"
#include "SourceControlService.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParams = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodeParentInvalid = TEXT("PARENT_NOT_FOUND_OR_INVALID");
    constexpr const TCHAR* ErrorCodeAssetExists = TEXT("ASSET_ALREADY_EXISTS");
    constexpr const TCHAR* ErrorCodeCreateFailed = TEXT("CREATE_ASSET_FAILED");
    constexpr const TCHAR* ErrorCodeAssetNotFound = TEXT("ASSET_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeInvalidParameterType = TEXT("INVALID_PARAMETER_TYPE");
    constexpr const TCHAR* ErrorCodeSaveFailed = TEXT("SAVE_FAILED");
    constexpr const TCHAR* ErrorCodeSourceControlFailed = TEXT("SC_OPERATION_FAILED");

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

        if (Trimmed.Contains(TEXT(".")))
        {
            Trimmed = FPackageName::ObjectPathToPackageName(Trimmed);
        }

        return Trimmed;
    }

    FString BuildObjectPath(const FString& PackageName)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
        return FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    }

    bool IsPathAllowed(const FString& ContentPath, FString& OutReason)
    {
        return FWriteGate::IsPathAllowed(ContentPath, OutReason);
    }

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

    TSharedPtr<FJsonObject> MakeActionJson(const FString& Op, const TMap<FString, FString>& Args)
    {
        TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
        Action->SetStringField(TEXT("op"), Op);
        for (const TPair<FString, FString>& Pair : Args)
        {
            Action->SetStringField(Pair.Key, Pair.Value);
        }
        return Action;
    }

    bool MarkForAdd(const FString& PackageName, FString& OutError)
    {
        if (!FSourceControlService::IsEnabled())
        {
            return true;
        }

        FString PackageFilename;
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename))
        {
            OutError = FString::Printf(TEXT("Failed to convert package '%s' to filename"), *PackageName);
            return false;
        }

        TArray<FString> Files;
        Files.Add(PackageFilename);

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
                OutError = OperationError.IsEmpty() ? FString::Printf(TEXT("Failed to mark '%s' for add"), *Pair.Key) : OperationError;
                return false;
            }
        }

        return true;
    }

    bool ParseNumericValue(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
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

    bool ParseLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
    {
        if (!Value.IsValid() || Value->Type != EJson::Array)
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>& Components = Value->AsArray();
        if (Components.Num() < 3 || Components.Num() > 4)
        {
            return false;
        }

        double Parsed[4] = {1.0, 1.0, 1.0, 1.0};
        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            if (!ParseNumericValue(Components[Index], Parsed[Index]))
            {
                return false;
            }
        }

        if (Components.Num() == 3)
        {
            Parsed[3] = 1.0;
        }

        OutColor = FLinearColor(
            static_cast<float>(Parsed[0]),
            static_cast<float>(Parsed[1]),
            static_cast<float>(Parsed[2]),
            static_cast<float>(Parsed[3]));
        return true;
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

    TSharedPtr<FJsonObject> BuildNotFoundJson(const TArray<FString>& Scalars, const TArray<FString>& Vectors, const TArray<FString>& Textures, const TArray<FString>& Switches)
    {
        TSharedPtr<FJsonObject> NotFound = MakeShared<FJsonObject>();
        AppendStringArrayField(NotFound, TEXT("scalars"), Scalars);
        AppendStringArrayField(NotFound, TEXT("vectors"), Vectors);
        AppendStringArrayField(NotFound, TEXT("textures"), Textures);
        AppendStringArrayField(NotFound, TEXT("switches"), Switches);
        return NotFound;
    }

    TSharedPtr<FJsonObject> BuildChangedJson(const TArray<FString>& Scalars, const TArray<FString>& Vectors, const TArray<FString>& Textures, const TArray<FString>& Switches)
    {
        TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
        AppendStringArrayField(Changed, TEXT("scalars"), Scalars);
        AppendStringArrayField(Changed, TEXT("vectors"), Vectors);
        AppendStringArrayField(Changed, TEXT("textures"), Textures);
        AppendStringArrayField(Changed, TEXT("switches"), Switches);
        return Changed;
    }

    void CollectParameterNames(UMaterialInstanceConstant& MIC, TSet<FName>& ScalarNames, TSet<FName>& VectorNames, TSet<FName>& TextureNames, TSet<FName>& SwitchNames)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Ids;

        Infos.Reset();
        Ids.Reset();
        MIC.GetAllScalarParameterInfo(Infos, Ids);
        for (const FMaterialParameterInfo& Info : Infos)
        {
            ScalarNames.Add(Info.Name);
        }

        Infos.Reset();
        Ids.Reset();
        MIC.GetAllVectorParameterInfo(Infos, Ids);
        for (const FMaterialParameterInfo& Info : Infos)
        {
            VectorNames.Add(Info.Name);
        }

        Infos.Reset();
        Ids.Reset();
        MIC.GetAllTextureParameterInfo(Infos, Ids);
        for (const FMaterialParameterInfo& Info : Infos)
        {
            TextureNames.Add(Info.Name);
        }

        Infos.Reset();
        Ids.Reset();
        MIC.GetAllStaticSwitchParameterInfo(Infos, Ids);
        for (const FMaterialParameterInfo& Info : Infos)
        {
            SwitchNames.Add(Info.Name);
        }
    }
}

TSharedPtr<FJsonObject> FMaterialInstanceTools::Create(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    FString ParentPath;
    FString RawMiPath;
    const bool bHasParent = Params->TryGetStringField(TEXT("parent"), ParentPath);
    const bool bHasPath = Params->TryGetStringField(TEXT("miPath"), RawMiPath);

    if (bHasParent)
    {
        ParentPath.TrimStartAndEndInline();
    }

    if (bHasPath)
    {
        RawMiPath.TrimStartAndEndInline();
    }

    if (!bHasParent || ParentPath.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parent parameter"));
    }

    if (!bHasPath || RawMiPath.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing miPath parameter"));
    }

    const bool bOverwriteIfExists = Params->HasTypedField<EJson::Boolean>(TEXT("overwriteIfExists")) && Params->GetBoolField(TEXT("overwriteIfExists"));
    const bool bSave = !Params->HasField(TEXT("save")) || Params->GetBoolField(TEXT("save"));

    FString NormalizedPath = NormalizeContentPath(RawMiPath);
    if (!NormalizedPath.StartsWith(TEXT("/Game/")))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("miPath must be within /Game"));
    }

    if (!FPackageName::IsValidLongPackageName(NormalizedPath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("miPath is not a valid long package name"));
    }

    FString PathReason;
    if (!IsPathAllowed(NormalizedPath, PathReason))
    {
        return MakeErrorResponse(TEXT("PATH_NOT_ALLOWED"), PathReason);
    }

    const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedPath);
    if (AssetName.IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("miPath is missing asset name"));
    }

    const FString MiObjectPath = BuildObjectPath(NormalizedPath);

    UMaterialInterface* ParentMaterial = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
    if (!ParentMaterial)
    {
        return MakeErrorResponse(ErrorCodeParentInvalid, TEXT("Parent material not found"));
    }

    if (!ParentMaterial->IsA<UMaterial>() && !ParentMaterial->IsA<UMaterialInstance>())
    {
        return MakeErrorResponse(ErrorCodeParentInvalid, TEXT("Parent must be a Material or MaterialInstance"));
    }

    const bool bExists = UEditorAssetLibrary::DoesAssetExist(MiObjectPath);
    if (bExists && !bOverwriteIfExists)
    {
        return MakeErrorResponse(ErrorCodeAssetExists, TEXT("Material instance already exists"));
    }

    if (bExists)
    {
        if (!UEditorAssetLibrary::DeleteAsset(MiObjectPath))
        {
            return MakeErrorResponse(ErrorCodeCreateFailed, TEXT("Failed to overwrite existing material instance"));
        }
    }

    UPackage* Package = CreatePackage(*NormalizedPath);
    if (!Package)
    {
        return MakeErrorResponse(ErrorCodeCreateFailed, TEXT("Failed to create package"));
    }

    Package->FullyLoad();

    UMaterialInstanceConstant* MaterialInstance = NewObject<UMaterialInstanceConstant>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!MaterialInstance)
    {
        return MakeErrorResponse(ErrorCodeCreateFailed, TEXT("Failed to create material instance object"));
    }

    MaterialInstance->SetParentEditorOnly(ParentMaterial);
    MaterialInstance->PostEditChange();

    FAssetRegistryModule::AssetCreated(MaterialInstance);
    Package->MarkPackageDirty();

    if (!bExists)
    {
        FString ScError;
        if (!MarkForAdd(NormalizedPath, ScError))
        {
            return MakeErrorResponse(ErrorCodeSourceControlFailed, ScError);
        }
    }

    if (bSave)
    {
        if (!UEditorAssetLibrary::SaveAsset(MiObjectPath, false))
        {
            return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save material instance"));
        }
    }

    TMap<FString, FString> ActionArgs;
    ActionArgs.Add(TEXT("parent"), ParentPath);
    ActionArgs.Add(TEXT("dst"), NormalizedPath);
    TArray<TSharedPtr<FJsonValue>> Actions;
    Actions.Add(MakeShared<FJsonValueObject>(MakeActionJson(TEXT("create_mi"), ActionArgs)));

    TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
    Result->SetStringField(TEXT("miObjectPath"), MiObjectPath);
    Result->SetStringField(TEXT("parentClass"), ParentMaterial->GetClass()->GetName());
    Result->SetBoolField(TEXT("created"), true);
    Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, Actions));

    return Result;
}

TSharedPtr<FJsonObject> FMaterialInstanceTools::SetParameters(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
    }

    FString MiObjectPath;
    if (!Params->TryGetStringField(TEXT("miObjectPath"), MiObjectPath) || MiObjectPath.TrimStartAndEnd().IsEmpty())
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing miObjectPath parameter"));
    }

    MiObjectPath.TrimStartAndEndInline();

    const FString PackagePath = NormalizeContentPath(MiObjectPath);
    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("miObjectPath must be within /Game"));
    }

    if (!FPackageName::IsValidLongPackageName(PackagePath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("miObjectPath is not a valid package"));
    }

    FString PathReason;
    if (!IsPathAllowed(PackagePath, PathReason))
    {
        return MakeErrorResponse(TEXT("PATH_NOT_ALLOWED"), PathReason);
    }

    const FString NormalizedObjectPath = BuildObjectPath(PackagePath);

    UMaterialInstanceConstant* MaterialInstance = LoadObject<UMaterialInstanceConstant>(nullptr, *NormalizedObjectPath);
    if (!MaterialInstance)
    {
        return MakeErrorResponse(ErrorCodeAssetNotFound, TEXT("Material instance not found"));
    }

    const bool bSave = !Params->HasField(TEXT("save")) || Params->GetBoolField(TEXT("save"));
    const bool bClearUnset = Params->HasTypedField<EJson::Boolean>(TEXT("clearUnset")) && Params->GetBoolField(TEXT("clearUnset"));

    MaterialInstance->Modify();

    bool bModified = false;
    bool bStaticSwitchChanged = false;

    TSet<FName> ScalarNames;
    TSet<FName> VectorNames;
    TSet<FName> TextureNames;
    TSet<FName> SwitchNames;
    CollectParameterNames(*MaterialInstance, ScalarNames, VectorNames, TextureNames, SwitchNames);

    TArray<FString> ChangedScalars;
    TArray<FString> ChangedVectors;
    TArray<FString> ChangedTextures;
    TArray<FString> ChangedSwitches;

    TArray<FString> MissingScalars;
    TArray<FString> MissingVectors;
    TArray<FString> MissingTextures;
    TArray<FString> MissingSwitches;

    TArray<TSharedPtr<FJsonValue>> AuditActions;

    if (bClearUnset)
    {
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
        MaterialInstance->ClearAllOverrideParameters();
#else
        MaterialInstance->ClearParameterOverrides();
#endif
        bModified = true;

        TMap<FString, FString> Args;
        AuditActions.Add(MakeShared<FJsonValueObject>(MakeActionJson(TEXT("clear_overrides"), Args)));
    }

    const TSharedPtr<FJsonObject>* ScalarsObject = nullptr;
    if (Params->TryGetObjectField(TEXT("scalars"), ScalarsObject) && ScalarsObject && ScalarsObject->IsValid())
    {
        for (const auto& Pair : (*ScalarsObject)->Values)
        {
            double NumberValue = 0.0;
            if (!ParseNumericValue(Pair.Value, NumberValue))
            {
                return MakeErrorResponse(ErrorCodeInvalidParameterType, FString::Printf(TEXT("Scalar '%s' is not numeric"), *Pair.Key));
            }

            const FName ParamName(*Pair.Key);
            if (!ScalarNames.Contains(ParamName))
            {
                MissingScalars.Add(Pair.Key);
                continue;
            }

            const float FloatValue = static_cast<float>(NumberValue);
            MaterialInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(ParamName), FloatValue);
            ChangedScalars.Add(Pair.Key);
            bModified = true;

            TMap<FString, FString> Args;
            Args.Add(TEXT("name"), Pair.Key);
            Args.Add(TEXT("value"), LexToString(FloatValue));
            AuditActions.Add(MakeShared<FJsonValueObject>(MakeActionJson(TEXT("set_scalar"), Args)));
        }
    }

    const TSharedPtr<FJsonObject>* VectorsObject = nullptr;
    if (Params->TryGetObjectField(TEXT("vectors"), VectorsObject) && VectorsObject && VectorsObject->IsValid())
    {
        for (const auto& Pair : (*VectorsObject)->Values)
        {
            FLinearColor ColorValue;
            if (!ParseLinearColor(Pair.Value, ColorValue))
            {
                return MakeErrorResponse(ErrorCodeInvalidParameterType, FString::Printf(TEXT("Vector '%s' must be an array of 3 or 4 numbers"), *Pair.Key));
            }

            const FName ParamName(*Pair.Key);
            if (!VectorNames.Contains(ParamName))
            {
                MissingVectors.Add(Pair.Key);
                continue;
            }

            MaterialInstance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(ParamName), ColorValue);
            ChangedVectors.Add(Pair.Key);
            bModified = true;

            TMap<FString, FString> Args;
            Args.Add(TEXT("name"), Pair.Key);
            Args.Add(TEXT("value"), FString::Printf(TEXT("[%g,%g,%g,%g]"), ColorValue.R, ColorValue.G, ColorValue.B, ColorValue.A));
            AuditActions.Add(MakeShared<FJsonValueObject>(MakeActionJson(TEXT("set_vector"), Args)));
        }
    }

    const TSharedPtr<FJsonObject>* TexturesObject = nullptr;
    if (Params->TryGetObjectField(TEXT("textures"), TexturesObject) && TexturesObject && TexturesObject->IsValid())
    {
        for (const auto& Pair : (*TexturesObject)->Values)
        {
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
            {
                return MakeErrorResponse(ErrorCodeInvalidParameterType, FString::Printf(TEXT("Texture '%s' must be a string object path"), *Pair.Key));
            }

            const FString TexturePath = Pair.Value->AsString();
            UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
            if (!Texture)
            {
                MissingTextures.Add(Pair.Key);
                continue;
            }

            const FName ParamName(*Pair.Key);
            if (!TextureNames.Contains(ParamName))
            {
                MissingTextures.Add(Pair.Key);
                continue;
            }

            MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(ParamName), Texture);
            ChangedTextures.Add(Pair.Key);
            bModified = true;

            TMap<FString, FString> Args;
            Args.Add(TEXT("name"), Pair.Key);
            Args.Add(TEXT("value"), TexturePath);
            AuditActions.Add(MakeShared<FJsonValueObject>(MakeActionJson(TEXT("set_texture"), Args)));
        }
    }

    const TSharedPtr<FJsonObject>* SwitchesObject = nullptr;
    if (Params->TryGetObjectField(TEXT("switches"), SwitchesObject) && SwitchesObject && SwitchesObject->IsValid())
    {
        for (const auto& Pair : (*SwitchesObject)->Values)
        {
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Boolean)
            {
                return MakeErrorResponse(ErrorCodeInvalidParameterType, FString::Printf(TEXT("Switch '%s' must be a boolean"), *Pair.Key));
            }

            const FName ParamName(*Pair.Key);
            if (!SwitchNames.Contains(ParamName))
            {
                MissingSwitches.Add(Pair.Key);
                continue;
            }

            const bool bValue = Pair.Value->AsBool();
            bool bPreviousValue = false;
            FGuid ExpressionGuid;
            const bool bHadPreviousValue = MaterialInstance->GetStaticSwitchParameterValue(FMaterialParameterInfo(ParamName), bPreviousValue, ExpressionGuid);
            MaterialInstance->SetStaticSwitchParameterValueEditorOnly(FMaterialParameterInfo(ParamName), bValue);
            const bool bValueChanged = !bHadPreviousValue || bPreviousValue != bValue;
            ChangedSwitches.Add(Pair.Key);
            bModified = true;
            bStaticSwitchChanged = bStaticSwitchChanged || bValueChanged;

            TMap<FString, FString> Args;
            Args.Add(TEXT("name"), Pair.Key);
            Args.Add(TEXT("value"), bValue ? TEXT("true") : TEXT("false"));
            AuditActions.Add(MakeShared<FJsonValueObject>(MakeActionJson(TEXT("set_switch"), Args)));
        }
    }

    if (bStaticSwitchChanged)
    {
        MaterialInstance->InitStaticPermutation();
    }

    if (bModified)
    {
        MaterialInstance->PostEditChange();
        MaterialInstance->MarkPackageDirty();
    }

    if (bSave && bModified)
    {
        if (!UEditorAssetLibrary::SaveAsset(NormalizedObjectPath, false))
        {
            return MakeErrorResponse(ErrorCodeSaveFailed, TEXT("Failed to save material instance"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
    Result->SetStringField(TEXT("miObjectPath"), NormalizedObjectPath);
    Result->SetBoolField(TEXT("modified"), bModified);
    Result->SetBoolField(TEXT("saved"), bSave && bModified);
    Result->SetObjectField(TEXT("changed"), BuildChangedJson(ChangedScalars, ChangedVectors, ChangedTextures, ChangedSwitches));
    Result->SetObjectField(TEXT("notFound"), BuildNotFoundJson(MissingScalars, MissingVectors, MissingTextures, MissingSwitches));
    Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, AuditActions));

    return Result;
}
