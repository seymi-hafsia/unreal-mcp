#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Containers/Map.h"
#include "Containers/Array.h"
#include "Templates/Optional.h"

class FJsonObject;

struct FAssetFindParams
{
    TArray<FString> Paths;
    TArray<FString> ClassNames;
    TOptional<FString> NameContains;
    TMap<FName, TArray<FString>> TagQuery;
    bool bRecursive = true;
    int32 Limit = 200;
    int32 Offset = 0;

    enum class ESortBy
    {
        Name,
        Class,
        Path
    };

    ESortBy SortBy = ESortBy::Name;
    bool bSortAscending = true;
};

struct FAssetLite
{
    FString ObjectPath;
    FString PackagePath;
    FString AssetName;
    FString ClassName;
    TMap<FString, TArray<FString>> Tags;
};

class FAssetQuery
{
public:
    static bool Find(const FAssetFindParams& Params, int32& OutTotal, TArray<FAssetLite>& OutItems, FString& OutError);
    static bool Exists(const FString& ObjectPath, bool& bOutExists, FString& OutClassName, FString& OutError);
    static bool Metadata(const FString& ObjectPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
};
