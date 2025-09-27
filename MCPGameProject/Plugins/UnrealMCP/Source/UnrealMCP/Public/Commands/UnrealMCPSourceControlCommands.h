#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UNREALMCP_API FUnrealMCPSourceControlCommands
{
public:
        FUnrealMCPSourceControlCommands();

        TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
        TSharedPtr<FJsonObject> HandleStatus(const TSharedPtr<FJsonObject>& Params);
        TSharedPtr<FJsonObject> HandleCheckout(const TSharedPtr<FJsonObject>& Params);
        TSharedPtr<FJsonObject> HandleAdd(const TSharedPtr<FJsonObject>& Params);
        TSharedPtr<FJsonObject> HandleRevert(const TSharedPtr<FJsonObject>& Params);
        TSharedPtr<FJsonObject> HandleSubmit(const TSharedPtr<FJsonObject>& Params);

        bool ExtractTargets(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutFiles, TArray<FString>& OutAssets, FString& OutError) const;
        TArray<TSharedPtr<FJsonValue>> BuildPerFileResultArray(const TMap<FString, bool>& PerFileResult) const;
        TArray<TSharedPtr<FJsonValue>> BuildPerFileStatusArray(const TMap<FString, FString>& PerFileStatus) const;
        TSharedPtr<FJsonObject> MakeErrorResponse(const FString& Code, const FString& Message) const;
};
