#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** High-level content hygiene helpers (scan/validate/fix/thumbnails). */
class UNREALMCPEDITOR_API FContentTools
{
public:
        FContentTools();

        /** Routes a content.* command to the appropriate handler. */
        TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
        TSharedPtr<FJsonObject> HandleScan(const TSharedPtr<FJsonObject>& Params);
        TSharedPtr<FJsonObject> HandleValidate(const TSharedPtr<FJsonObject>& Params);
        TSharedPtr<FJsonObject> HandleFixMissing(const TSharedPtr<FJsonObject>& Params);
        TSharedPtr<FJsonObject> HandleGenerateThumbnails(const TSharedPtr<FJsonObject>& Params);

        // Shared helpers
        static bool CollectContentPaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPaths, FString& OutError);
        static bool CollectAssetPaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutAssets, FString& OutError);
        static FString NormalizeContentPath(const FString& InPath);
        static bool IsContentPathValid(const FString& Path);
};

