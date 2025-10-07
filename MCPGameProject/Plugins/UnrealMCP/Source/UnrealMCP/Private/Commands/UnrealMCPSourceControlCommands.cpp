#include "CoreMinimal.h"
#include "Commands/UnrealMCPSourceControlCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "SourceControlService.h"

#include "Dom/JsonValue.h"

FUnrealMCPSourceControlCommands::FUnrealMCPSourceControlCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPSourceControlCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
        if (CommandType == TEXT("sc.status"))
        {
                return HandleStatus(Params);
        }
        else if (CommandType == TEXT("sc.checkout"))
        {
                return HandleCheckout(Params);
        }
        else if (CommandType == TEXT("sc.add"))
        {
                return HandleAdd(Params);
        }
        else if (CommandType == TEXT("sc.revert"))
        {
                return HandleRevert(Params);
        }
        else if (CommandType == TEXT("sc.submit"))
        {
                return HandleSubmit(Params);
        }

        return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), FString::Printf(TEXT("Unknown source control command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPSourceControlCommands::HandleStatus(const TSharedPtr<FJsonObject>& Params)
{
        if (!FSourceControlService::IsEnabled())
        {
                return MakeErrorResponse(TEXT("SC_NOT_AVAILABLE"), TEXT("Source control integration is disabled"));
        }

        TArray<FString> Files;
        TArray<FString> Assets;
        FString ExtractionError;
        if (!ExtractTargets(Params, Files, Assets, ExtractionError))
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), ExtractionError);
        }

        FString AssetConversionError;
        if (Assets.Num() > 0)
        {
                TArray<FString> AssetFiles;
                if (!FSourceControlService::AssetPathsToFiles(Assets, AssetFiles, AssetConversionError))
                {
                        return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), AssetConversionError);
                }

                Files.Append(AssetFiles);
        }

        TMap<FString, FString> PerFileStates;
        FString OperationError;
        const bool bSucceeded = FSourceControlService::UpdateStatus(Files, PerFileStates, OperationError);

        if (!bSucceeded && !OperationError.IsEmpty())
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), OperationError);
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Results = BuildPerFileStatusArray(PerFileStates);
        Data->SetArrayField(TEXT("perFileResults"), Results);

        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPSourceControlCommands::HandleCheckout(const TSharedPtr<FJsonObject>& Params)
{
        if (!FSourceControlService::IsEnabled())
        {
                return MakeErrorResponse(TEXT("SC_NOT_AVAILABLE"), TEXT("Source control integration is disabled"));
        }

        TArray<FString> Files;
        TArray<FString> Assets;
        FString ExtractionError;
        if (!ExtractTargets(Params, Files, Assets, ExtractionError))
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), ExtractionError);
        }

        FString AssetConversionError;
        if (Assets.Num() > 0)
        {
                TArray<FString> AssetFiles;
                if (!FSourceControlService::AssetPathsToFiles(Assets, AssetFiles, AssetConversionError))
                {
                        return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), AssetConversionError);
                }

                Files.Append(AssetFiles);
        }

        TMap<FString, bool> PerFileResults;
        FString OperationError;
        const bool bSucceeded = FSourceControlService::Checkout(Files, PerFileResults, OperationError);

        if (!bSucceeded && !OperationError.IsEmpty())
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), OperationError);
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("perFileResults"), BuildPerFileResultArray(PerFileResults));
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPSourceControlCommands::HandleAdd(const TSharedPtr<FJsonObject>& Params)
{
        if (!FSourceControlService::IsEnabled())
        {
                return MakeErrorResponse(TEXT("SC_NOT_AVAILABLE"), TEXT("Source control integration is disabled"));
        }

        TArray<FString> Files;
        TArray<FString> Assets;
        FString ExtractionError;
        if (!ExtractTargets(Params, Files, Assets, ExtractionError))
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), ExtractionError);
        }

        FString AssetConversionError;
        if (Assets.Num() > 0)
        {
                TArray<FString> AssetFiles;
                if (!FSourceControlService::AssetPathsToFiles(Assets, AssetFiles, AssetConversionError))
                {
                        return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), AssetConversionError);
                }

                Files.Append(AssetFiles);
        }

        TMap<FString, bool> PerFileResults;
        FString OperationError;
        const bool bSucceeded = FSourceControlService::MarkForAdd(Files, PerFileResults, OperationError);

        if (!bSucceeded && !OperationError.IsEmpty())
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), OperationError);
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("perFileResults"), BuildPerFileResultArray(PerFileResults));
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPSourceControlCommands::HandleRevert(const TSharedPtr<FJsonObject>& Params)
{
        if (!FSourceControlService::IsEnabled())
        {
                return MakeErrorResponse(TEXT("SC_NOT_AVAILABLE"), TEXT("Source control integration is disabled"));
        }

        TArray<FString> Files;
        TArray<FString> Assets;
        FString ExtractionError;
        if (!ExtractTargets(Params, Files, Assets, ExtractionError))
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), ExtractionError);
        }

        FString AssetConversionError;
        if (Assets.Num() > 0)
        {
                TArray<FString> AssetFiles;
                if (!FSourceControlService::AssetPathsToFiles(Assets, AssetFiles, AssetConversionError))
                {
                        return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), AssetConversionError);
                }

                Files.Append(AssetFiles);
        }

        TMap<FString, bool> PerFileResults;
        FString OperationError;
        const bool bSucceeded = FSourceControlService::Revert(Files, PerFileResults, OperationError);

        if (!bSucceeded && !OperationError.IsEmpty())
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), OperationError);
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("perFileResults"), BuildPerFileResultArray(PerFileResults));
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPSourceControlCommands::HandleSubmit(const TSharedPtr<FJsonObject>& Params)
{
        if (!FSourceControlService::IsEnabled())
        {
                return MakeErrorResponse(TEXT("SC_NOT_AVAILABLE"), TEXT("Source control integration is disabled"));
        }

        FString Description;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("description"), Description) || Description.IsEmpty())
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), TEXT("Missing or empty 'description'"));
        }

        TArray<FString> Files;
        TArray<FString> Assets;
        FString ExtractionError;
        if (!ExtractTargets(Params, Files, Assets, ExtractionError))
        {
                return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), ExtractionError);
        }

        FString AssetConversionError;
        if (Assets.Num() > 0)
        {
                TArray<FString> AssetFiles;
                if (!FSourceControlService::AssetPathsToFiles(Assets, AssetFiles, AssetConversionError))
                {
                        return MakeErrorResponse(TEXT("SC_OPERATION_FAILED"), AssetConversionError);
                }

                Files.Append(AssetFiles);
        }

        TMap<FString, bool> PerFileResults;
        FString OperationError;
        const bool bSucceeded = FSourceControlService::Submit(Files, Description, PerFileResults, OperationError);

        if (!bSucceeded)
        {
                if (!OperationError.IsEmpty())
                {
                        return MakeErrorResponse(TEXT("SC_SUBMIT_FAILED"), OperationError);
                }

                return MakeErrorResponse(TEXT("SC_SUBMIT_FAILED"), TEXT("Submit failed"));
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("perFileResults"), BuildPerFileResultArray(PerFileResults));
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

bool FUnrealMCPSourceControlCommands::ExtractTargets(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutFiles, TArray<FString>& OutAssets, FString& OutError) const
{
        OutFiles.Reset();
        OutAssets.Reset();
        OutError.Reset();

        if (!Params.IsValid())
        {
                OutError = TEXT("Missing parameters");
                return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* AssetsJson = nullptr;
        if (Params->TryGetArrayField(TEXT("assets"), AssetsJson))
        {
                for (const TSharedPtr<FJsonValue>& Value : *AssetsJson)
                {
                        if (Value->Type == EJson::String)
                        {
                                OutAssets.Add(Value->AsString());
                        }
                }
        }

        const TArray<TSharedPtr<FJsonValue>>* FilesJson = nullptr;
        if (Params->TryGetArrayField(TEXT("files"), FilesJson))
        {
                for (const TSharedPtr<FJsonValue>& Value : *FilesJson)
                {
                        if (Value->Type == EJson::String)
                        {
                                OutFiles.Add(Value->AsString());
                        }
                }
        }

        if (OutFiles.Num() == 0 && OutAssets.Num() == 0)
        {
                OutError = TEXT("Expected 'assets' or 'files' array");
                return false;
        }

        return true;
}

TArray<TSharedPtr<FJsonValue>> FUnrealMCPSourceControlCommands::BuildPerFileResultArray(const TMap<FString, bool>& PerFileResult) const
{
        TArray<TSharedPtr<FJsonValue>> Array;
        FSourceControlService::AppendResultArray(PerFileResult, Array);
        return Array;
}

TArray<TSharedPtr<FJsonValue>> FUnrealMCPSourceControlCommands::BuildPerFileStatusArray(const TMap<FString, FString>& PerFileStatus) const
{
        TArray<TSharedPtr<FJsonValue>> Array;
        FSourceControlService::AppendStatusArray(PerFileStatus, Array);
        return Array;
}

TSharedPtr<FJsonObject> FUnrealMCPSourceControlCommands::MakeErrorResponse(const FString& Code, const FString& Message) const
{
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetBoolField(TEXT("success"), false);
        Response->SetStringField(TEXT("error"), Message);
        Response->SetStringField(TEXT("errorCode"), Code);
        return Response;
}
