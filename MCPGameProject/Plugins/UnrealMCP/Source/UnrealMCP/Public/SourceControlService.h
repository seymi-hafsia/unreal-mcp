#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FJsonObject;

/**
 * Helper utilities to execute source control operations in a provider agnostic way.
 */
class UNREALMCP_API FSourceControlService
{
public:
        static bool IsEnabled();
        static bool EnsureProviderReady(FString& OutError);

        static bool UpdateStatus(const TArray<FString>& Files, TMap<FString, FString>& OutPerFileState, FString& OutError);
        static bool Checkout(const TArray<FString>& Files, TMap<FString, bool>& OutPerFileOk, FString& OutError);
        static bool MarkForAdd(const TArray<FString>& Files, TMap<FString, bool>& OutPerFileOk, FString& OutError);
        static bool Revert(const TArray<FString>& Files, TMap<FString, bool>& OutPerFileOk, FString& OutError);
        static bool Submit(const TArray<FString>& Files, const FString& Description, TMap<FString, bool>& OutPerFileOk, FString& OutError);

        static bool AssetPathsToFiles(const TArray<FString>& AssetPaths, TArray<FString>& OutFiles, FString& OutError);

        static void AppendResultArray(const TMap<FString, bool>& PerFileResult, TArray<TSharedPtr<FJsonValue>>& OutArray);
        static void AppendStatusArray(const TMap<FString, FString>& PerFileStatus, TArray<TSharedPtr<FJsonValue>>& OutArray);

private:
        static bool ExecuteSimpleOperation(const TArray<FString>& Files, const TFunctionRef<TSharedRef<class ISourceControlOperation>(void)>& OperationFactory, TMap<FString, bool>& OutPerFileOk, FString& OutError);
        static FString DescribeState(const class ISourceControlState& State);
        static bool CollectExistingFiles(const TArray<FString>& Files, TArray<FString>& OutExistingFiles);
};
