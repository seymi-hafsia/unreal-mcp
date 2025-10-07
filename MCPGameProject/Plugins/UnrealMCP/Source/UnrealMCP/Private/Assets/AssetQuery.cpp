#include "Assets/AssetQuery.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Templates/Optional.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealMCPLog.h"
#include "Algo/Sort.h"
#include "Engine/World.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    constexpr int32 MaxLimit = 1000;

    IAssetRegistry& GetAssetRegistry()
    {
        static FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        return AssetRegistryModule.Get();
    }

    bool ResolveClassPath(const FString& ClassName, FTopLevelAssetPath& OutPath)
    {
        if (ClassName.IsEmpty())
        {
            return false;
        }

        if (ClassName.Contains(TEXT(".")) || ClassName.Contains(TEXT("/")))
        {
            FTopLevelAssetPath ParsedPath(ClassName);
            if (ParsedPath.IsNull())
            {
                return false;
            }

            OutPath = ParsedPath;
            return true;
        }

        if (UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *ClassName))
        {
            OutPath = FoundClass->GetClassPathName();
            return true;
        }

        OutPath = FTopLevelAssetPath(FName(TEXT("/Script/Engine")), FName(*ClassName));
        return true;
    }

    bool MatchesTagQuery(const FAssetData& AssetData, const TMap<FName, TArray<FString>>& TagQuery)
    {
        if (TagQuery.Num() == 0)
        {
            return true;
        }

        const FAssetDataTagMapSharedView Tags = AssetData.GetTagValues();
        for (const TPair<FName, TArray<FString>>& Pair : TagQuery)
        {
            const FAssetDataTagMapSharedView::FFindTagResult TagValue = Tags.FindTag(Pair.Key);
            if (!TagValue.IsSet())
            {
                return false;
            }

            const FString TagString = TagValue.GetValue();
            for (const FString& ExpectedValue : Pair.Value)
            {
                if (!TagString.Contains(ExpectedValue, ESearchCase::IgnoreCase))
                {
                    return false;
                }
            }
        }

        return true;
    }

    TArray<FString> ParseTagValues(const FString& RawValue)
    {
        TArray<FString> ParsedValues;
        FString Sanitized = RawValue;
        Sanitized.TrimStartAndEndInline();

        if (Sanitized.IsEmpty())
        {
            return ParsedValues;
        }

        if ((Sanitized.StartsWith(TEXT("[")) && Sanitized.EndsWith(TEXT("]"))) ||
            (Sanitized.StartsWith(TEXT("{")) && Sanitized.EndsWith(TEXT("}"))))
        {
            TSharedPtr<FJsonValue> JsonValue;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Sanitized);
            if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
            {
                if (JsonValue->Type == EJson::Array)
                {
                    for (const TSharedPtr<FJsonValue>& Entry : JsonValue->AsArray())
                    {
                        ParsedValues.Add(Entry->AsString());
                    }
                }
                else if (JsonValue->Type == EJson::String)
                {
                    ParsedValues.Add(JsonValue->AsString());
                }

                if (ParsedValues.Num() > 0)
                {
                    return ParsedValues;
                }
            }
        }

        Sanitized.ParseIntoArray(ParsedValues, TEXT(","), true);
        if (ParsedValues.Num() == 0)
        {
            ParsedValues.Add(Sanitized);
        }

        for (FString& Value : ParsedValues)
        {
            Value.TrimStartAndEndInline();
        }

        ParsedValues.RemoveAllSwap([](const FString& Value)
        {
            return Value.IsEmpty();
        }, false);

        if (ParsedValues.Num() == 0)
        {
            ParsedValues.Add(Sanitized);
        }

        return ParsedValues;
    }

    void CopyTags(const FAssetData& AssetData, TMap<FString, TArray<FString>>& OutTags)
    {
        OutTags.Reset();
        for (const FName& TagName : AssetData.GetTagNames())
        {
            FString TagValueString;
            if (AssetData.GetTagValue(TagName, TagValueString))
            {
                OutTags.Add(TagName.ToString(), ParseTagValues(TagValueString));
            }
        }
    }

    FString DependencyPackageToObjectPath(const FName& PackageName, IAssetRegistry& AssetRegistry)
    {
        FString Result;
        TArray<FAssetData> AssetsInPackage;
        if (AssetRegistry.GetAssetsByPackageName(PackageName, AssetsInPackage))
        {
            if (AssetsInPackage.Num() > 0)
            {
                Result = AssetsInPackage[0].ToSoftObjectPath().ToString();
            }
        }

        if (Result.IsEmpty())
        {
            Result = PackageName.ToString();
        }

        return Result;
    }

    void CollectDependencies(const FAssetData& AssetData, IAssetRegistry& AssetRegistry, const UE::AssetRegistry::EDependencyFlags Flags, TArray<FString>& OutDependencies)
    {
        using namespace UE::AssetRegistry;
        OutDependencies.Reset();

        TArray<FName> PackageDependencies;
        FDependencyQuery Query;
        Query.Flags = Flags;
        AssetRegistry.GetDependencies(AssetData.PackageName, PackageDependencies, EDependencyCategory::Package, Query);

        for (const FName& Dependency : PackageDependencies)
        {
            OutDependencies.Add(DependencyPackageToObjectPath(Dependency, AssetRegistry));
        }
    }
}

bool FAssetQuery::Find(const FAssetFindParams& Params, int32& OutTotal, TArray<FAssetLite>& OutItems, FString& OutError)
{
    const double StartTime = FPlatformTime::Seconds();
    IAssetRegistry& AssetRegistry = GetAssetRegistry();

    OutError.Reset();
    OutTotal = 0;
    OutItems.Reset();

    FARFilter Filter;
    Filter.bRecursivePaths = Params.bRecursive;
    Filter.bRecursiveClasses = true;

    for (const FString& Path : Params.Paths)
    {
        if (!Path.IsEmpty())
        {
            Filter.PackagePaths.Add(*Path);
        }
    }

    for (const FString& ClassName : Params.ClassNames)
    {
        FTopLevelAssetPath ClassPath;
        if (ResolveClassPath(ClassName, ClassPath))
        {
            Filter.ClassPaths.Add(ClassPath);
        }
    }

    TArray<FAssetData> AssetResults;
    if (!AssetRegistry.GetAssets(Filter, AssetResults))
    {
        OutError = TEXT("Asset registry query failed");
        return false;
    }

    TArray<FAssetData> Filtered;
    Filtered.Reserve(AssetResults.Num());

    for (const FAssetData& AssetData : AssetResults)
    {
        if (Params.NameContains.IsSet())
        {
            const FString AssetNameString = AssetData.AssetName.ToString();
            if (!AssetNameString.Contains(Params.NameContains.GetValue(), ESearchCase::IgnoreCase))
            {
                continue;
            }
        }

        if (!MatchesTagQuery(AssetData, Params.TagQuery))
        {
            continue;
        }

        Filtered.Add(AssetData);
    }

    Algo::Sort(Filtered, [&Params](const FAssetData& A, const FAssetData& B)
    {
        auto CompareNames = [](const FName& Left, const FName& Right) -> int32
        {
            if (Left == Right)
            {
                return 0;
            }
            return Left.LexicalLess(Right) ? -1 : 1;
        };

        int32 Comparison = 0;
        switch (Params.SortBy)
        {
        case FAssetFindParams::ESortBy::Class:
            Comparison = CompareNames(A.AssetClassPath.GetAssetName(), B.AssetClassPath.GetAssetName());
            break;
        case FAssetFindParams::ESortBy::Path:
            Comparison = CompareNames(A.PackagePath, B.PackagePath);
            break;
        case FAssetFindParams::ESortBy::Name:
        default:
            Comparison = CompareNames(A.AssetName, B.AssetName);
            break;
        }

        if (Comparison == 0)
        {
            Comparison = CompareNames(A.AssetName, B.AssetName);
        }

        if (Params.bSortAscending)
        {
            return Comparison < 0;
        }

        return Comparison > 0;
    });

    const int32 ClampedLimit = FMath::Clamp(Params.Limit, 0, MaxLimit);
    const int32 Offset = FMath::Max(Params.Offset, 0);

    OutTotal = Filtered.Num();

    if (ClampedLimit == 0 || Offset >= Filtered.Num())
    {
        UE_LOG(LogUnrealMCP, Verbose, TEXT("Asset.find returning 0 items (total %d)"), OutTotal);
        return true;
    }

    const int32 EndIndex = FMath::Min(Offset + ClampedLimit, Filtered.Num());
    for (int32 Index = Offset; Index < EndIndex; ++Index)
    {
        const FAssetData& AssetData = Filtered[Index];

        FAssetLite Lite;
        Lite.ObjectPath = AssetData.ToSoftObjectPath().ToString();
        Lite.PackagePath = AssetData.PackagePath.ToString();
        Lite.AssetName = AssetData.AssetName.ToString();
        Lite.ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
        CopyTags(AssetData, Lite.Tags);

        OutItems.Add(MoveTemp(Lite));
    }

    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
    UE_LOG(LogUnrealMCP, Verbose, TEXT("Asset.find returned %d/%d items in %.2f ms"), OutItems.Num(), OutTotal, ElapsedMs);

    return true;
}

bool FAssetQuery::Exists(const FString& ObjectPath, bool& bOutExists, FString& OutClassName, FString& OutError)
{
    bOutExists = false;
    OutClassName.Reset();
    OutError.Reset();

    if (ObjectPath.IsEmpty())
    {
        OutError = TEXT("Missing objectPath parameter");
        return false;
    }

    IAssetRegistry& AssetRegistry = GetAssetRegistry();
    FSoftObjectPath SoftPath(ObjectPath);
    if (!SoftPath.IsValid())
    {
        OutError = TEXT("Invalid object path");
        return false;
    }

    FAssetData AssetData;
    if (!AssetRegistry.GetAssetByObjectPath(SoftPath, AssetData))
    {
        bOutExists = false;
        return true;
    }

    bOutExists = AssetData.IsValid();
    if (bOutExists)
    {
        OutClassName = AssetData.AssetClassPath.GetAssetName().ToString();
    }

    return true;
}

bool FAssetQuery::Metadata(const FString& ObjectPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    OutJson.Reset();
    OutError.Reset();

    if (ObjectPath.IsEmpty())
    {
        OutError = TEXT("Missing objectPath parameter");
        return false;
    }

    IAssetRegistry& AssetRegistry = GetAssetRegistry();
    FSoftObjectPath SoftPath(ObjectPath);
    if (!SoftPath.IsValid())
    {
        OutError = TEXT("Invalid object path");
        return false;
    }

    FAssetData AssetData;
    if (!AssetRegistry.GetAssetByObjectPath(SoftPath, AssetData) || !AssetData.IsValid())
    {
        OutError = TEXT("Asset not found");
        return false;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("objectPath"), AssetData.ToSoftObjectPath().ToString());
    Result->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
    Result->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
    Result->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());
    Result->SetStringField(TEXT("assetName"), AssetData.AssetName.ToString());
    Result->SetBoolField(TEXT("isRedirector"), AssetData.IsRedirector());
    Result->SetBoolField(TEXT("isUWorld"), AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName());

    FString PackageFilename;
    if (FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), nullptr, &PackageFilename))
    {
        const int64 FileSize = IFileManager::Get().FileSize(*PackageFilename);
        if (FileSize >= 0)
        {
            Result->SetNumberField(TEXT("sizeOnDisk"), static_cast<double>(FileSize));
        }
    }

    const FString PackageNameString = AssetData.PackageName.ToString();
    bool bIsDirtyKnown = false;
    bool bIsDirty = false;

    if (!PackageNameString.IsEmpty())
    {
        if (UPackage* Package = FindPackage(nullptr, *PackageNameString))
        {
            bIsDirtyKnown = true;
            bIsDirty = Package->IsDirty();
        }
    }

    Result->SetBoolField(TEXT("isDirty"), bIsDirty);
    Result->SetBoolField(TEXT("isDirtyKnown"), bIsDirtyKnown);

    TSharedPtr<FJsonObject> TagsJson = MakeShared<FJsonObject>();
    for (const FName& TagName : AssetData.GetTagNames())
    {
        FString TagValueString;
        if (AssetData.GetTagValue(TagName, TagValueString))
        {
            TArray<FString> Parsed = ParseTagValues(TagValueString);
            TArray<TSharedPtr<FJsonValue>> JsonArray;
            for (const FString& Value : Parsed)
            {
                JsonArray.Add(MakeShared<FJsonValueString>(Value));
            }
            TagsJson->SetArrayField(TagName.ToString(), JsonArray);
        }
    }
    Result->SetObjectField(TEXT("tags"), TagsJson);

    using namespace UE::AssetRegistry;
    TSharedPtr<FJsonObject> DependenciesJson = MakeShared<FJsonObject>();

    TArray<FString> HardDependencies;
    CollectDependencies(AssetData, AssetRegistry, EDependencyFlags::Hard, HardDependencies);
    TArray<TSharedPtr<FJsonValue>> HardArray;
    for (const FString& Dependency : HardDependencies)
    {
        HardArray.Add(MakeShared<FJsonValueString>(Dependency));
    }
    DependenciesJson->SetArrayField(TEXT("hard"), HardArray);

    TArray<FString> SoftDependencies;
    CollectDependencies(AssetData, AssetRegistry, EDependencyFlags::Soft, SoftDependencies);
    TArray<TSharedPtr<FJsonValue>> SoftArray;
    for (const FString& Dependency : SoftDependencies)
    {
        SoftArray.Add(MakeShared<FJsonValueString>(Dependency));
    }
    DependenciesJson->SetArrayField(TEXT("soft"), SoftArray);

    Result->SetObjectField(TEXT("dependencies"), DependenciesJson);

    OutJson = Result;
    return true;
}
