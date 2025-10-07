#include "Content/ContentTools.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "EditorAssetLibrary.h"
#include "EditorLoadingAndSavingUtils.h"
#include "Engine/Texture.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstance.h"
#include "Misc/PackageName.h"
#include "Permissions/WriteGate.h"
#include "PhysicsEngine/BodySetup.h"
#include "Regex.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/TopLevelAssetPath.h"
#include "Containers/ScriptArray.h"
#include "Containers/ScriptMap.h"
#include "Containers/ScriptSet.h"

namespace
{
        constexpr const TCHAR* ErrorCodeScanFailed = TEXT("SCAN_FAILED");
        constexpr const TCHAR* ErrorCodeValidateFailed = TEXT("VALIDATE_FAILED");
        constexpr const TCHAR* ErrorCodeFixRedirectorsFailed = TEXT("FIX_REDIRECTORS_FAILED");
        constexpr const TCHAR* ErrorCodeThumbnailFailed = TEXT("THUMBNAIL_GEN_FAILED");
        constexpr const TCHAR* ErrorCodeSaveFailed = TEXT("SAVE_FAILED");
        constexpr const TCHAR* ErrorCodeWriteNotAllowed = TEXT("WRITE_NOT_ALLOWED");

        FString SanitizePath(const FString& InPath)
        {
                FString Result = InPath;
                Result.TrimStartAndEndInline();
                Result.ReplaceInline(TEXT("\\"), TEXT("/"));
                if (Result.StartsWith(TEXT("\"")) && Result.EndsWith(TEXT("\"")) && Result.Len() > 1)
                {
                        Result = Result.Mid(1, Result.Len() - 2);
                }
                return Result;
        }

        bool TryFixSoftPath(FSoftObjectPath& Path, const TMap<FString, FString>& RedirectMap)
        {
                const FString Current = Path.ToString();
                if (Current.IsEmpty())
                {
                        return false;
                }

                if (const FString* Replacement = RedirectMap.Find(Current))
                {
                        Path.SetPath(*Replacement);
                        return true;
                }

                return false;
        }

        bool RemapSoftValue(FProperty* Property, void* ValuePtr, const TMap<FString, FString>& RedirectMap, int32& OutRemappedCount)
        {
                if (!Property || !ValuePtr)
                {
                        return false;
                }

                bool bModified = false;

                if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Property))
                {
                        FSoftObjectPtr CurrentValue = SoftProp->GetPropertyValue(ValuePtr);
                        FSoftObjectPath Path = CurrentValue.ToSoftObjectPath();
                        if (TryFixSoftPath(Path, RedirectMap))
                        {
                                FSoftObjectPtr NewValue(Path);
                                SoftProp->SetPropertyValue(ValuePtr, NewValue);
                                ++OutRemappedCount;
                                bModified = true;
                        }
                }
                else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
                {
                        if (StructProp->Struct == TBaseStructure<FSoftObjectPath>::Get())
                        {
                                FSoftObjectPath* Path = StructProp->ContainerPtrToValuePtr<FSoftObjectPath>(ValuePtr);
                                if (Path && TryFixSoftPath(*Path, RedirectMap))
                                {
                                        ++OutRemappedCount;
                                        bModified = true;
                                }
                        }
                        else if (StructProp->Struct == TBaseStructure<FSoftClassPath>::Get())
                        {
                                FSoftClassPath* Path = StructProp->ContainerPtrToValuePtr<FSoftClassPath>(ValuePtr);
                                if (Path)
                                {
                                        FSoftObjectPath ObjectPath(Path->GetAssetPath());
                                        if (TryFixSoftPath(ObjectPath, RedirectMap))
                                        {
                                                *Path = FSoftClassPath(ObjectPath.ToString());
                                                ++OutRemappedCount;
                                                bModified = true;
                                        }
                                }
                        }
                }
                else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
                {
                        FScriptArrayHelper Helper(ArrayProp, ValuePtr);
                        for (int32 Index = 0; Index < Helper.Num(); ++Index)
                        {
                                uint8* ElementPtr = Helper.GetRawPtr(Index);
                                if (RemapSoftValue(ArrayProp->Inner, ElementPtr, RedirectMap, OutRemappedCount))
                                {
                                        bModified = true;
                                }
                        }
                }
                else if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
                {
                        FScriptSetHelper Helper(SetProp, ValuePtr);
                        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
                        {
                                if (!Helper.IsValidIndex(Index))
                                {
                                        continue;
                                }

                                uint8* ElementPtr = Helper.GetElementPtr(Index);
                                if (RemapSoftValue(SetProp->ElementProp, ElementPtr, RedirectMap, OutRemappedCount))
                                {
                                        bModified = true;
                                }
                        }
                }
                else if (FMapProperty* MapProp = CastField<FMapProperty>(Property))
                {
                        FScriptMapHelper Helper(MapProp, ValuePtr);
                        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
                        {
                                if (!Helper.IsValidIndex(Index))
                                {
                                        continue;
                                }

                                uint8* KeyPtr = Helper.GetKeyPtr(Index);
                                uint8* ValuePtrMap = Helper.GetValuePtr(Index);
                                if (RemapSoftValue(MapProp->KeyProp, KeyPtr, RedirectMap, OutRemappedCount) |
                                    RemapSoftValue(MapProp->ValueProp, ValuePtrMap, RedirectMap, OutRemappedCount))
                                {
                                        bModified = true;
                                }
                        }
                }

                return bModified;
        }

        bool RemapSoftReferencesInObject(UObject* Object, const TMap<FString, FString>& RedirectMap, int32& OutRemappedCount, TSet<UPackage*>& OutDirtyPackages)
        {
                if (!Object)
                {
                        return false;
                }

                bool bModified = false;
                for (FProperty* Property : TFieldRange<FProperty>(Object->GetClass()))
                {
                        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
                        if (!ValuePtr)
                        {
                                continue;
                        }

                        if (RemapSoftValue(Property, ValuePtr, RedirectMap, OutRemappedCount))
                        {
                                bModified = true;
                        }
                }

                if (bModified)
                {
                        if (UPackage* Package = Object->GetOutermost())
                        {
                                OutDirtyPackages.Add(Package);
                        }
                        Object->MarkPackageDirty();
                }

                return bModified;
        }

        bool ShouldMarkAsOrphan(const FString& PackagePath, const TArray<FName>& Referencers)
        {
                if (PackagePath.StartsWith(TEXT("/Game/Temp")))
                {
                        return true;
                }
                return Referencers.Num() == 0;
        }

        bool IsRegexMatch(const FString& Value, const FString& Pattern)
        {
                FRegexPattern RegexPattern(Pattern);
                FRegexMatcher Matcher(RegexPattern, Value);
                return Matcher.FindNext();
        }

        bool ResolveClassPathForRule(const FString& RuleKey, FTopLevelAssetPath& OutPath)
        {
                static const FTopLevelAssetPath StaticMeshPath = UStaticMesh::StaticClass()->GetClassPathName();
                static const FTopLevelAssetPath TexturePath = UTexture::StaticClass()->GetClassPathName();
                static const FTopLevelAssetPath MaterialInstancePath = UMaterialInstance::StaticClass()->GetClassPathName();

                if (RuleKey.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
                {
                        OutPath = StaticMeshPath;
                        return true;
                }
                if (RuleKey.Equals(TEXT("Texture"), ESearchCase::IgnoreCase))
                {
                        OutPath = TexturePath;
                        return true;
                }
                if (RuleKey.Equals(TEXT("MaterialInstance"), ESearchCase::IgnoreCase) ||
                    RuleKey.Equals(TEXT("MaterialInstanceConstant"), ESearchCase::IgnoreCase))
                {
                        OutPath = MaterialInstancePath;
                        return true;
                }

                if (const UClass* FoundClass = FindObject<UClass>(nullptr, *RuleKey))
                {
                        OutPath = FoundClass->GetClassPathName();
                        return true;
                }

                return false;
        }

}

FContentTools::FContentTools() = default;

TSharedPtr<FJsonObject> FContentTools::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
        if (CommandType == TEXT("content.scan"))
        {
                return HandleScan(Params);
        }
        if (CommandType == TEXT("content.validate"))
        {
                return HandleValidate(Params);
        }
        if (CommandType == TEXT("content.fix_missing"))
        {
                return HandleFixMissing(Params);
        }
        if (CommandType == TEXT("content.generate_thumbnails"))
        {
                return HandleGenerateThumbnails(Params);
        }

        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown content command: %s"), *CommandType));
        Error->SetStringField(TEXT("errorCode"), TEXT("UNKNOWN_COMMAND"));
        return Error;
}

TSharedPtr<FJsonObject> FContentTools::HandleScan(const TSharedPtr<FJsonObject>& Params)
{
        TArray<FString> Paths;
        FString ParseError;
        if (!CollectContentPaths(Params, Paths, ParseError))
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ParseError);
                Error->SetStringField(TEXT("errorCode"), ErrorCodeScanFailed);
                return Error;
        }

        bool bRecursive = true;
        bool bIncludeUnusedTextures = false;
        bool bIncludeReferencers = true;

        if (Params.IsValid())
        {
                Params->TryGetBoolField(TEXT("recursive"), bRecursive);
                Params->TryGetBoolField(TEXT("includeUnusedTextures"), bIncludeUnusedTextures);
                Params->TryGetBoolField(TEXT("includeReferencers"), bIncludeReferencers);
        }

        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        FARFilter Filter;
        Filter.bRecursivePaths = bRecursive;
        Filter.bRecursiveClasses = true;
        for (const FString& Path : Paths)
        {
                Filter.PackagePaths.Add(*Path);
        }

        TArray<FAssetData> Assets;
        if (!AssetRegistry.GetAssets(Filter, Assets))
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to query Asset Registry"));
                Error->SetStringField(TEXT("errorCode"), ErrorCodeScanFailed);
                return Error;
        }

        TArray<TSharedPtr<FJsonValue>> RedirectorsArray;
        TArray<TSharedPtr<FJsonValue>> MissingArray;
        TArray<TSharedPtr<FJsonValue>> BrokenArray;
        TArray<TSharedPtr<FJsonValue>> UnusedTexturesArray;
        TArray<TSharedPtr<FJsonValue>> OrphansArray;

        int32 RedirectorCount = 0;
        int32 MissingCount = 0;
        int32 BrokenCount = 0;
        int32 UnusedCount = 0;
        int32 OrphanCount = 0;

        FAssetRegistryDependencyOptions DependencyOptions;
        DependencyOptions.bIncludePackages = true;
        DependencyOptions.bIncludeHard = true;
        DependencyOptions.bIncludeSoft = true;
        DependencyOptions.bIncludeSearchableNames = false;
        DependencyOptions.bIncludeManageDependencies = false;

        const FTopLevelAssetPath RedirectorClassPath = UObjectRedirector::StaticClass()->GetClassPathName();
        const FTopLevelAssetPath TextureClassPath = UTexture::StaticClass()->GetClassPathName();

        for (const FAssetData& AssetData : Assets)
        {
                const FString ObjectPath = AssetData.ToSoftObjectPath().ToString();

                if (AssetData.AssetClassPath == RedirectorClassPath)
                {
                        RedirectorsArray.Add(MakeShared<FJsonValueString>(ObjectPath));
                        ++RedirectorCount;
                        continue;
                }

                // Missing hard/soft dependencies
                TArray<FName> Dependencies;
                AssetRegistry.GetDependencies(AssetData.PackageName, Dependencies, DependencyOptions);
                for (const FName& DependencyPackage : Dependencies)
                {
                        TArray<FAssetData> DependencyAssets;
                        AssetRegistry.GetAssetsByPackageName(DependencyPackage, DependencyAssets);
                        if (DependencyAssets.Num() == 0)
                        {
                                TSharedPtr<FJsonObject> MissingEntry = MakeShared<FJsonObject>();
                                MissingEntry->SetStringField(TEXT("referencer"), ObjectPath);
                                MissingEntry->SetStringField(TEXT("needed"), DependencyPackage.ToString());
                                MissingEntry->SetStringField(TEXT("type"), AssetData.AssetClassPath.GetAssetName().ToString());

                                if (bIncludeReferencers)
                                {
                                        TArray<FName> Referencers;
                                        AssetRegistry.GetReferencers(DependencyPackage, Referencers, EAssetRegistryDependencyType::All);

                                        TArray<TSharedPtr<FJsonValue>> ReferencerJson;
                                        for (const FName& Referencer : Referencers)
                                        {
                                                ReferencerJson.Add(MakeShared<FJsonValueString>(Referencer.ToString()));
                                        }
                                        MissingEntry->SetArrayField(TEXT("referencers"), ReferencerJson);
                                }

                                MissingArray.Add(MakeShared<FJsonValueObject>(MissingEntry));
                                ++MissingCount;
                        }
                }

                // Broken soft references via tags
                for (const TPair<FName, FAssetTagValueRef>& TagPair : AssetData.TagsAndValues())
                {
                        const FString TagValue = TagPair.Value.AsString();
                        FSoftObjectPath SoftPath(TagValue);
                        if (SoftPath.IsNull())
                        {
                                continue;
                        }

                        const FString LongPackageName = SoftPath.GetLongPackageName();
                        if (!LongPackageName.StartsWith(TEXT("/Game/")))
                        {
                                continue;
                        }

                        const FAssetData TargetAsset = AssetRegistry.GetAssetByObjectPath(SoftPath);
                        if (!TargetAsset.IsValid())
                        {
                                TSharedPtr<FJsonObject> BrokenEntry = MakeShared<FJsonObject>();
                                BrokenEntry->SetStringField(TEXT("owner"), ObjectPath);
                                BrokenEntry->SetStringField(TEXT("prop"), TagPair.Key.ToString());
                                BrokenEntry->SetStringField(TEXT("softPath"), SoftPath.ToString());
                                BrokenArray.Add(MakeShared<FJsonValueObject>(BrokenEntry));
                                ++BrokenCount;
                        }
                }

                // Referencers for orphan/unused detection
                TArray<FName> Referencers;
                AssetRegistry.GetReferencers(AssetData.PackageName, Referencers, EAssetRegistryDependencyType::Packages);

                if (bIncludeUnusedTextures && AssetData.IsInstanceOf(TextureClassPath) && Referencers.Num() == 0)
                {
                        UnusedTexturesArray.Add(MakeShared<FJsonValueString>(ObjectPath));
                        ++UnusedCount;
                }

                const FString PackageName = AssetData.PackageName.ToString();
                if (ShouldMarkAsOrphan(PackageName, Referencers))
                {
                        OrphansArray.Add(MakeShared<FJsonValueString>(ObjectPath));
                        ++OrphanCount;
                }
        }

        TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
        Stats->SetNumberField(TEXT("assets"), Assets.Num());
        Stats->SetNumberField(TEXT("redirectors"), RedirectorCount);
        Stats->SetNumberField(TEXT("missing"), MissingCount);
        Stats->SetNumberField(TEXT("brokenRefs"), BrokenCount);
        Stats->SetNumberField(TEXT("unusedTextures"), UnusedCount);
        Stats->SetNumberField(TEXT("orphans"), OrphanCount);

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetObjectField(TEXT("stats"), Stats);
        Data->SetArrayField(TEXT("redirectors"), RedirectorsArray);
        Data->SetArrayField(TEXT("missingAssets"), MissingArray);
        Data->SetArrayField(TEXT("brokenRefs"), BrokenArray);
        Data->SetArrayField(TEXT("unusedTextures"), UnusedTexturesArray);
        Data->SetArrayField(TEXT("orphans"), OrphansArray);

        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FContentTools::HandleValidate(const TSharedPtr<FJsonObject>& Params)
{
        TArray<FString> Paths;
        FString ParseError;
        if (!CollectContentPaths(Params, Paths, ParseError))
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ParseError);
                Error->SetStringField(TEXT("errorCode"), ErrorCodeValidateFailed);
                return Error;
        }

        TSharedPtr<FJsonObject> RulesObject;
        if (Params.IsValid())
        {
                const TSharedPtr<FJsonObject>* RulesPtr = nullptr;
                if (Params->TryGetObjectField(TEXT("rules"), RulesPtr))
                {
                        RulesObject = *RulesPtr;
                }
        }

        TMap<FTopLevelAssetPath, FString> NamingRules;
        int32 TextureMaxSize = 0;
        bool bTexturePowerOfTwo = false;
        bool bHasTextureRules = false;
        bool bRequiresCollision = false;
        int32 StaticMeshMinLODs = 0;
        bool bHasStaticMeshRules = false;
        TArray<FName> RequiredMIParams;
        bool bHasMIRules = false;

        if (RulesObject.IsValid())
        {
                const TSharedPtr<FJsonObject>* NamingObject = nullptr;
                if (RulesObject->TryGetObjectField(TEXT("naming"), NamingObject))
                {
                        for (const auto& Pair : (*NamingObject)->Values)
                        {
                                if (Pair.Value->Type != EJson::String)
                                {
                                        continue;
                                }

                                FTopLevelAssetPath ClassPath;
                                if (!ResolveClassPathForRule(Pair.Key, ClassPath))
                                {
                                        continue;
                                }

                                NamingRules.Add(ClassPath, Pair.Value->AsString());
                        }
                }

                const TSharedPtr<FJsonObject>* TextureRules = nullptr;
                if (RulesObject->TryGetObjectField(TEXT("texture"), TextureRules))
                {
                        if ((*TextureRules)->HasTypedField<EJson::Number>(TEXT("maxSize")))
                        {
                                TextureMaxSize = static_cast<int32>((*TextureRules)->GetNumberField(TEXT("maxSize")));
                                bHasTextureRules = true;
                        }
                        if ((*TextureRules)->HasTypedField<EJson::Boolean>(TEXT("powerOfTwo")))
                        {
                                bTexturePowerOfTwo = (*TextureRules)->GetBoolField(TEXT("powerOfTwo"));
                                bHasTextureRules = true;
                        }
                }

                const TSharedPtr<FJsonObject>* StaticMeshRules = nullptr;
                if (RulesObject->TryGetObjectField(TEXT("staticMesh"), StaticMeshRules))
                {
                        if ((*StaticMeshRules)->HasTypedField<EJson::Boolean>(TEXT("requiresCollision")))
                        {
                                bRequiresCollision = (*StaticMeshRules)->GetBoolField(TEXT("requiresCollision"));
                                bHasStaticMeshRules = true;
                        }
                        if ((*StaticMeshRules)->HasTypedField<EJson::Number>(TEXT("minLODs")))
                        {
                                StaticMeshMinLODs = static_cast<int32>((*StaticMeshRules)->GetNumberField(TEXT("minLODs")));
                                bHasStaticMeshRules = true;
                        }
                }

                const TSharedPtr<FJsonObject>* MIRules = nullptr;
                if (RulesObject->TryGetObjectField(TEXT("mi"), MIRules))
                {
                        const TArray<TSharedPtr<FJsonValue>>* RequireParams = nullptr;
                        if ((*MIRules)->TryGetArrayField(TEXT("requireParams"), RequireParams))
                        {
                                for (const TSharedPtr<FJsonValue>& Value : *RequireParams)
                                {
                                        if (Value->Type == EJson::String)
                                        {
                                                RequiredMIParams.Add(*Value->AsString());
                                        }
                                }
                                bHasMIRules = RequiredMIParams.Num() > 0;
                        }
                }
        }

        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        FARFilter Filter;
        Filter.bRecursiveClasses = true;
        Filter.bRecursivePaths = true;
        for (const FString& Path : Paths)
        {
                Filter.PackagePaths.Add(*Path);
        }

        TArray<FAssetData> Assets;
        if (!AssetRegistry.GetAssets(Filter, Assets))
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to query Asset Registry"));
                Error->SetStringField(TEXT("errorCode"), ErrorCodeValidateFailed);
                return Error;
        }

        const FTopLevelAssetPath TextureClassPath = UTexture::StaticClass()->GetClassPathName();
        const FTopLevelAssetPath StaticMeshClassPath = UStaticMesh::StaticClass()->GetClassPathName();
        const FTopLevelAssetPath MaterialInstanceClassPath = UMaterialInstance::StaticClass()->GetClassPathName();

        TArray<TSharedPtr<FJsonValue>> Violations;
        TMap<FString, int32> ViolationsByRule;

        auto AddViolation = [&Violations, &ViolationsByRule](const FString& AssetPath, const FString& RuleId, const FString& Message)
        {
                TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
                Violation->SetStringField(TEXT("asset"), AssetPath);
                Violation->SetStringField(TEXT("rule"), RuleId);
                Violation->SetStringField(TEXT("message"), Message);
                Violations.Add(MakeShared<FJsonValueObject>(Violation));
                ViolationsByRule.FindOrAdd(RuleId) += 1;
        };

        for (const FAssetData& AssetData : Assets)
        {
                const FString ObjectPath = AssetData.ToSoftObjectPath().ToString();

                // Naming
                for (const TPair<FTopLevelAssetPath, FString>& Rule : NamingRules)
                {
                        if (!AssetData.IsInstanceOf(Rule.Key))
                        {
                                continue;
                        }

                        const FString AssetName = AssetData.AssetName.ToString();
                        if (!IsRegexMatch(AssetName, Rule.Value))
                        {
                                AddViolation(ObjectPath, FString::Printf(TEXT("naming.%s"), *Rule.Key.GetAssetName().ToString()),
                                             FString::Printf(TEXT("Name should match %s"), *Rule.Value));
                        }
                }

                // Texture rules
                if (bHasTextureRules && AssetData.IsInstanceOf(TextureClassPath))
                {
                        if (UTexture* Texture = Cast<UTexture>(AssetData.GetAsset()))
                        {
                                const int32 Width = Texture->GetSurfaceWidth();
                                const int32 Height = Texture->GetSurfaceHeight();
                                if (TextureMaxSize > 0 && (Width > TextureMaxSize || Height > TextureMaxSize))
                                {
                                        AddViolation(ObjectPath, TEXT("texture.maxSize"),
                                                     FString::Printf(TEXT("Texture dimensions %dx%d exceed max %d"), Width, Height, TextureMaxSize));
                                }

                                if (bTexturePowerOfTwo)
                                {
                                        if (!FMath::IsPowerOfTwo(Width) || !FMath::IsPowerOfTwo(Height))
                                        {
                                                AddViolation(ObjectPath, TEXT("texture.powerOfTwo"), TEXT("Texture dimensions must be powers of two"));
                                        }
                                }
                        }
                }

                // Static mesh rules
                if (bHasStaticMeshRules && AssetData.IsInstanceOf(StaticMeshClassPath))
                {
                        if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
                        {
                                if (StaticMeshMinLODs > 0)
                                {
                                        const int32 NumLODs = StaticMesh->GetNumLODs();
                                        if (NumLODs < StaticMeshMinLODs)
                                        {
                                                AddViolation(ObjectPath, TEXT("staticMesh.minLODs"),
                                                             FString::Printf(TEXT("Expected >=%d LODs, got %d"), StaticMeshMinLODs, NumLODs));
                                        }
                                }

                                if (bRequiresCollision)
                                {
                                        const UBodySetup* BodySetup = StaticMesh->GetBodySetup();
                                        const bool bHasCollision = BodySetup && (BodySetup->AggGeom.GetElementCount() > 0 || BodySetup->bHasCookedCollisionData);
                                        if (!bHasCollision)
                                        {
                                                AddViolation(ObjectPath, TEXT("staticMesh.requiresCollision"), TEXT("Static mesh is missing collision"));
                                        }
                                }
                        }
                }

                // Material instance rules
                if (bHasMIRules && AssetData.IsInstanceOf(MaterialInstanceClassPath))
                {
                        if (UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(AssetData.GetAsset()))
                        {
                                TSet<FName> AvailableParams;
                                TArray<FMaterialParameterInfo> Infos;
                                TArray<FGuid> Ids;

                                Infos.Reset();
                                Ids.Reset();
                                MaterialInstance->GetAllScalarParameterInfo(Infos, Ids);
                                for (const FMaterialParameterInfo& Info : Infos)
                                {
                                        AvailableParams.Add(Info.Name);
                                }

                                Infos.Reset();
                                Ids.Reset();
                                MaterialInstance->GetAllVectorParameterInfo(Infos, Ids);
                                for (const FMaterialParameterInfo& Info : Infos)
                                {
                                        AvailableParams.Add(Info.Name);
                                }

                                Infos.Reset();
                                Ids.Reset();
                                MaterialInstance->GetAllTextureParameterInfo(Infos, Ids);
                                for (const FMaterialParameterInfo& Info : Infos)
                                {
                                        AvailableParams.Add(Info.Name);
                                }

                                Infos.Reset();
                                Ids.Reset();
                                MaterialInstance->GetAllStaticSwitchParameterInfo(Infos, Ids);
                                for (const FMaterialParameterInfo& Info : Infos)
                                {
                                        AvailableParams.Add(Info.Name);
                                }

                                for (const FName& RequiredParam : RequiredMIParams)
                                {
                                        if (!AvailableParams.Contains(RequiredParam))
                                        {
                                                AddViolation(ObjectPath, TEXT("mi.requireParams"),
                                                             FString::Printf(TEXT("Missing required parameter %s"), *RequiredParam.ToString()));
                                        }
                                }
                        }
                }
        }

        TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("violations"), Violations.Num());

        TSharedPtr<FJsonObject> ByRule = MakeShared<FJsonObject>();
        for (const TPair<FString, int32>& Pair : ViolationsByRule)
        {
                ByRule->SetNumberField(Pair.Key, Pair.Value);
        }
        Summary->SetObjectField(TEXT("byRule"), ByRule);

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetArrayField(TEXT("violations"), Violations);
        Data->SetObjectField(TEXT("summary"), Summary);

        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FContentTools::HandleFixMissing(const TSharedPtr<FJsonObject>& Params)
{
        if (!FWriteGate::IsWriteAllowed())
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Writes are currently disabled"));
                Error->SetStringField(TEXT("errorCode"), ErrorCodeWriteNotAllowed);
                return Error;
        }

        TArray<FString> Paths;
        FString ParseError;
        if (!CollectContentPaths(Params, Paths, ParseError))
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ParseError);
                Error->SetStringField(TEXT("errorCode"), ErrorCodeFixRedirectorsFailed);
                return Error;
        }

        for (const FString& Path : Paths)
        {
                FString Reason;
                if (!FWriteGate::IsPathAllowed(Path, Reason))
                {
                        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(Reason);
                        Error->SetStringField(TEXT("errorCode"), TEXT("PATH_NOT_ALLOWED"));
                        return Error;
                }
        }

        bool bRecursive = true;
        bool bFixRedirectors = true;
        bool bRemapReferences = true;
        bool bDeleteRedirectors = true;
        bool bSave = true;

        if (Params.IsValid())
        {
                Params->TryGetBoolField(TEXT("recursive"), bRecursive);
                Params->TryGetBoolField(TEXT("save"), bSave);

                const TSharedPtr<FJsonObject>* FixObject = nullptr;
                if (Params->TryGetObjectField(TEXT("fix"), FixObject) && FixObject->IsValid())
                {
                        (*FixObject)->TryGetBoolField(TEXT("redirectors"), bFixRedirectors);
                        (*FixObject)->TryGetBoolField(TEXT("remapReferences"), bRemapReferences);
                        (*FixObject)->TryGetBoolField(TEXT("deleteStaleRedirectors"), bDeleteRedirectors);
                }
        }

        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        TArray<UObjectRedirector*> Redirectors;
        TMap<FString, FString> RedirectMap;
        FARFilter Filter;
        Filter.bRecursivePaths = bRecursive;
        Filter.bRecursiveClasses = true;
        for (const FString& Path : Paths)
        {
                Filter.PackagePaths.Add(*Path);
        }

        TArray<FAssetData> Assets;
        AssetRegistry.GetAssets(Filter, Assets);

        const FTopLevelAssetPath RedirectorClassPath = UObjectRedirector::StaticClass()->GetClassPathName();
        const FString CombinedPaths = FString::Join(Paths, TEXT(","));
        for (const FAssetData& AssetData : Assets)
        {
                if (AssetData.AssetClassPath != RedirectorClassPath)
                {
                        continue;
                }

                if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(AssetData.GetAsset()))
                {
                        Redirectors.Add(Redirector);
                        if (Redirector->DestinationObject)
                        {
                                RedirectMap.Add(Redirector->GetPathName(), Redirector->DestinationObject->GetPathName());
                        }
                }
        }

        const int32 InitialRedirectorCount = Redirectors.Num();

        int32 FixedRedirectorCount = 0;
        int32 RemappedCount = 0;
        int32 DeletedCount = 0;

        TArray<TSharedPtr<FJsonValue>> AuditActions;

        if (bFixRedirectors && Redirectors.Num() > 0)
        {
                FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
                if (!AssetTools.Get().FixupReferencers(Redirectors))
                {
                        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to fix redirectors"));
                        Error->SetStringField(TEXT("errorCode"), ErrorCodeFixRedirectorsFailed);
                        return Error;
                }

                FixedRedirectorCount = Redirectors.Num();
        }

        TSet<UPackage*> DirtyPackages;
        if (bRemapReferences && RedirectMap.Num() > 0)
        {
                for (const FAssetData& AssetData : Assets)
                {
                        if (AssetData.AssetClassPath == RedirectorClassPath)
                        {
                                continue;
                        }

                        UObject* Asset = AssetData.GetAsset();
                        if (!Asset)
                        {
                                continue;
                        }

                        RemapSoftReferencesInObject(Asset, RedirectMap, RemappedCount, DirtyPackages);
                }

        }

        if (bDeleteRedirectors && Redirectors.Num() > 0)
        {
                for (UObjectRedirector* Redirector : Redirectors)
                {
                        if (!Redirector)
                        {
                                continue;
                        }

                        const FString ObjectPath = Redirector->GetPathName();
                        const FString PackageName = Redirector->GetOutermost()->GetName();

                        TArray<FName> Referencers;
                        AssetRegistry.GetReferencers(*PackageName, Referencers, EAssetRegistryDependencyType::All);
                        if (Referencers.Num() > 0)
                        {
                                continue;
                        }

                        if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
                        {
                                if (UEditorAssetLibrary::DeleteAsset(ObjectPath))
                                {
                                        ++DeletedCount;
                                }
                        }
                }
        }

        if (bFixRedirectors)
        {
                TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
                Action->SetStringField(TEXT("op"), TEXT("fix_redirectors"));
                Action->SetNumberField(TEXT("count"), FixedRedirectorCount);
                Action->SetBoolField(TEXT("recursive"), bRecursive);
                Action->SetBoolField(TEXT("executed"), InitialRedirectorCount > 0);
                if (!CombinedPaths.IsEmpty())
                {
                        Action->SetStringField(TEXT("paths"), CombinedPaths);
                }
                AuditActions.Add(MakeShared<FJsonValueObject>(Action));
        }

        if (bRemapReferences)
        {
                TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
                Action->SetStringField(TEXT("op"), TEXT("remap_soft_refs"));
                Action->SetNumberField(TEXT("count"), RemappedCount);
                if (!CombinedPaths.IsEmpty())
                {
                        Action->SetStringField(TEXT("paths"), CombinedPaths);
                }
                AuditActions.Add(MakeShared<FJsonValueObject>(Action));
        }

        if (bDeleteRedirectors)
        {
                TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
                Action->SetStringField(TEXT("op"), TEXT("delete_redirectors"));
                Action->SetNumberField(TEXT("count"), DeletedCount);
                if (!CombinedPaths.IsEmpty())
                {
                        Action->SetStringField(TEXT("paths"), CombinedPaths);
                }
                AuditActions.Add(MakeShared<FJsonValueObject>(Action));
        }

        if (bSave && DirtyPackages.Num() > 0)
        {
                TArray<UPackage*> PackagesToSave = DirtyPackages.Array();
                if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty*/ false))
                {
                        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save packages"));
                        Error->SetStringField(TEXT("errorCode"), ErrorCodeSaveFailed);
                        return Error;
                }
        }

        if (bSave)
        {
                TSharedPtr<FJsonObject> SaveAction = MakeShared<FJsonObject>();
                SaveAction->SetStringField(TEXT("op"), TEXT("save_packages"));
                SaveAction->SetNumberField(TEXT("count"), DirtyPackages.Num());
                SaveAction->SetBoolField(TEXT("executed"), DirtyPackages.Num() > 0 && bSave);
                AuditActions.Add(MakeShared<FJsonValueObject>(SaveAction));
        }

        TSharedPtr<FJsonObject> FixedObject = MakeShared<FJsonObject>();
        FixedObject->SetNumberField(TEXT("redirectors"), FixedRedirectorCount);
        FixedObject->SetNumberField(TEXT("remapped"), RemappedCount);
        FixedObject->SetNumberField(TEXT("deletedRedirectors"), DeletedCount);

        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("dryRun"), FWriteGate::ShouldDryRun());
        Audit->SetArrayField(TEXT("actions"), AuditActions);

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetObjectField(TEXT("fixed"), FixedObject);
        Data->SetObjectField(TEXT("audit"), Audit);

        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FContentTools::HandleGenerateThumbnails(const TSharedPtr<FJsonObject>& Params)
{
        if (!FWriteGate::IsWriteAllowed())
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Writes are currently disabled"));
                Error->SetStringField(TEXT("errorCode"), ErrorCodeWriteNotAllowed);
                return Error;
        }

        TArray<FString> Assets;
        FString ParseError;
        if (!CollectAssetPaths(Params, Assets, ParseError))
        {
                TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ParseError);
                Error->SetStringField(TEXT("errorCode"), ErrorCodeThumbnailFailed);
                return Error;
        }

        bool bHiRes = false;
        bool bSave = true;
        if (Params.IsValid())
        {
                Params->TryGetBoolField(TEXT("hiRes"), bHiRes);
                Params->TryGetBoolField(TEXT("save"), bSave);
        }

        UThumbnailManager& ThumbnailManager = UThumbnailManager::Get();

        int32 UpdatedCount = 0;
        TArray<TSharedPtr<FJsonValue>> FailedArray;
        TArray<TSharedPtr<FJsonValue>> AuditActions;
        TSet<UPackage*> PackagesToSave;

        for (const FString& AssetPath : Assets)
        {
                FSoftObjectPath SoftPath(AssetPath);
                UObject* Asset = SoftPath.TryLoad();
                if (!Asset)
                {
                        TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
                        Failure->SetStringField(TEXT("asset"), AssetPath);
                        Failure->SetStringField(TEXT("reason"), TEXT("Asset could not be loaded"));
                        FailedArray.Add(MakeShared<FJsonValueObject>(Failure));
                        continue;
                }

                ThumbnailManager.GenerateThumbnailForObject(Asset);
                if (UPackage* Package = Asset->GetOutermost())
                {
                        PackagesToSave.Add(Package);
                }
                Asset->MarkPackageDirty();
                ++UpdatedCount;

                TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
                Action->SetStringField(TEXT("op"), TEXT("regen_thumbnail"));
                Action->SetStringField(TEXT("asset"), AssetPath);
                Action->SetBoolField(TEXT("hiRes"), bHiRes);
                AuditActions.Add(MakeShared<FJsonValueObject>(Action));
        }

        if (bSave && PackagesToSave.Num() > 0)
        {
                TArray<UPackage*> Packages = PackagesToSave.Array();
                if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false))
                {
                        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save packages"));
                        Error->SetStringField(TEXT("errorCode"), ErrorCodeSaveFailed);
                        return Error;
                }
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);
        Data->SetNumberField(TEXT("updated"), UpdatedCount);
        Data->SetArrayField(TEXT("failed"), FailedArray);

        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("dryRun"), FWriteGate::ShouldDryRun());
        Audit->SetArrayField(TEXT("actions"), AuditActions);
        Data->SetObjectField(TEXT("audit"), Audit);

        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

bool FContentTools::CollectContentPaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPaths, FString& OutError)
{
        OutPaths.Reset();

        const TArray<TSharedPtr<FJsonValue>>* PathsJson = nullptr;
        if (Params.IsValid() && Params->TryGetArrayField(TEXT("paths"), PathsJson) && PathsJson)
        {
                        for (const TSharedPtr<FJsonValue>& Value : *PathsJson)
                        {
                                if (Value->Type != EJson::String)
                                {
                                        continue;
                                }

                                const FString Normalized = NormalizeContentPath(Value->AsString());
                                if (!IsContentPathValid(Normalized))
                                {
                                        OutError = FString::Printf(TEXT("Invalid content path: %s"), *Value->AsString());
                                        return false;
                                }

                                OutPaths.Add(Normalized);
                        }
        }

        if (OutPaths.Num() == 0)
        {
                OutPaths.Add(TEXT("/Game"));
        }

        return true;
}

bool FContentTools::CollectAssetPaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutAssets, FString& OutError)
{
        OutAssets.Reset();

        const TArray<TSharedPtr<FJsonValue>>* AssetsJson = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("assets"), AssetsJson) || !AssetsJson)
        {
                OutError = TEXT("Missing assets array");
                return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *AssetsJson)
        {
                if (Value->Type != EJson::String)
                {
                        continue;
                }

                const FString Normalized = NormalizeContentPath(Value->AsString());
                if (!FPackageName::IsValidObjectPath(Normalized))
                {
                        OutError = FString::Printf(TEXT("Invalid asset object path: %s"), *Value->AsString());
                        return false;
                }

                OutAssets.Add(Normalized);
        }

        if (OutAssets.Num() == 0)
        {
                OutError = TEXT("No assets provided");
                return false;
        }

        return true;
}

FString FContentTools::NormalizeContentPath(const FString& InPath)
{
        return SanitizePath(InPath);
}

bool FContentTools::IsContentPathValid(const FString& Path)
{
        if (!Path.StartsWith(TEXT("/Game")))
        {
                return false;
        }

        return FPackageName::IsValidLongPackageName(Path);
}

