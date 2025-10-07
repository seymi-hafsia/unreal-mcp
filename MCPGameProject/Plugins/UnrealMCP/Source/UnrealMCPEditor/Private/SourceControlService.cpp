#include "SourceControlService.h"
#include "CoreMinimal.h"
#include "UnrealMCPSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#include "SourceControlOperationBase.h"
#endif

namespace
{
        static FString NormalizeFilePath(const FString& InPath)
        {
                FString Normalized = InPath;
                FPaths::NormalizeFilename(Normalized);
                return Normalized;
        }
}

bool FSourceControlService::IsEnabled()
{
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        return Settings && Settings->EnableSourceControl;
}

bool FSourceControlService::EnsureProviderReady(FString& OutError)
{
        OutError.Reset();

        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        if (!Settings || !Settings->EnableSourceControl)
        {
                OutError = TEXT("Source control integration is disabled");
                return false;
        }

        ISourceControlModule& Module = ISourceControlModule::Get();

        if (!Module.IsEnabled())
        {
                OutError = Settings->AutoConnectSourceControl
                        ? TEXT("Source control module must be enabled manually")
                        : TEXT("Source control module is disabled");
                return false;
        }

        if (!Settings->PreferredProvider.IsEmpty())
        {
                const FName PreferredProvider(*Settings->PreferredProvider);
                if (Module.GetProvider().GetName() != PreferredProvider)
                {
                        Module.SetProvider(PreferredProvider);
                }
        }

        ISourceControlProvider& Provider = Module.GetProvider();
        if (!Provider.IsEnabled())
        {
                Provider.Init();
        }

        if (!Provider.IsEnabled() || !Provider.IsAvailable())
        {
                OutError = TEXT("Source control provider is not available");
                return false;
        }

        return true;
}

bool FSourceControlService::UpdateStatus(const TArray<FString>& Files, TMap<FString, FString>& OutPerFileState, FString& OutError)
{
        OutPerFileState.Reset();
        OutError.Reset();

        FString ReadyError;
        if (!EnsureProviderReady(ReadyError))
        {
                OutError = ReadyError;
                return false;
        }

        if (Files.Num() == 0)
        {
                return true;
        }

        TArray<FString> ExistingFiles;
        CollectExistingFiles(Files, ExistingFiles);

        if (ExistingFiles.Num() == 0)
        {
                return true;
        }

        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

        TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FUpdateStatus>();
        Operation->SetUpdateHistory(true);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
        const ECommandResult::Type Result = Provider.Execute(Operation, ExistingFiles, EConcurrency::Synchronous, FSourceControlOperationComplete());
#else
        const ECommandResult::Type Result = Provider.Execute(Operation, ExistingFiles, EConcurrency::Synchronous);
#endif
        if (Result != ECommandResult::Succeeded)
        {
                OutError = TEXT("Failed to update source control status");
                return false;
        }

        for (const FString& File : ExistingFiles)
        {
                const FSourceControlStatePtr State = Provider.GetState(File, EStateCacheUsage::Use);
                if (State.IsValid())
                {
                        OutPerFileState.Add(File, DescribeState(*State));
                }
                else
                {
                        OutPerFileState.Add(File, TEXT("Unknown"));
                }
        }

        return true;
}

bool FSourceControlService::Checkout(const TArray<FString>& Files, TMap<FString, bool>& OutPerFileOk, FString& OutError)
{
        OutPerFileOk.Reset();
        OutError.Reset();

        FString ReadyError;
        if (!EnsureProviderReady(ReadyError))
        {
                OutError = ReadyError;
                return false;
        }

        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

        if (Provider.GetName() == TEXT("Git"))
        {
                for (const FString& File : Files)
                {
                        OutPerFileOk.Add(File, true);
                }
                return true;
        }

        return ExecuteSimpleOperation(Files, []()
        {
                return ISourceControlOperation::Create<FCheckOut>();
        }, OutPerFileOk, OutError);
}

bool FSourceControlService::MarkForAdd(const TArray<FString>& Files, TMap<FString, bool>& OutPerFileOk, FString& OutError)
{
        OutPerFileOk.Reset();
        OutError.Reset();

        FString ReadyError;
        if (!EnsureProviderReady(ReadyError))
        {
                OutError = ReadyError;
                return false;
        }

        return ExecuteSimpleOperation(Files, []()
        {
                return ISourceControlOperation::Create<FMarkForAdd>();
        }, OutPerFileOk, OutError);
}

bool FSourceControlService::Revert(const TArray<FString>& Files, TMap<FString, bool>& OutPerFileOk, FString& OutError)
{
        OutPerFileOk.Reset();
        OutError.Reset();

        FString ReadyError;
        if (!EnsureProviderReady(ReadyError))
        {
                OutError = ReadyError;
                return false;
        }

        return ExecuteSimpleOperation(Files, []()
        {
                return ISourceControlOperation::Create<FRevert>();
        }, OutPerFileOk, OutError);
}

bool FSourceControlService::Submit(const TArray<FString>& Files, const FString& Description, TMap<FString, bool>& OutPerFileOk, FString& OutError)
{
        OutPerFileOk.Reset();
        OutError.Reset();

        FString ReadyError;
        if (!EnsureProviderReady(ReadyError))
        {
                OutError = ReadyError;
                return false;
        }

        if (Description.IsEmpty())
        {
                OutError = TEXT("Description is required for submit");
                return false;
        }

        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

        TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe> Operation =
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
                [&Description]()
                {
                        TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOperation = ISourceControlOperation::Create<FCheckIn>();
                        CheckInOperation->SetDescription(FText::FromString(Description));
                        return StaticCastSharedRef<ISourceControlOperation>(CheckInOperation);
                }();
#else
                [&Description]()
                {
                        TSharedRef<FSubmit, ESPMode::ThreadSafe> SubmitOperation = ISourceControlOperation::Create<FSubmit>();
                        SubmitOperation->SetDescription(Description);
                        return StaticCastSharedRef<ISourceControlOperation>(SubmitOperation);
                }();
#endif

        TArray<FString> ExistingFiles;
        CollectExistingFiles(Files, ExistingFiles);

        const ECommandResult::Type Result =
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
                Provider.Execute(Operation, ExistingFiles, EConcurrency::Synchronous, FSourceControlOperationComplete());
#else
                Provider.Execute(Operation, ExistingFiles, EConcurrency::Synchronous);
#endif
        if (Result != ECommandResult::Succeeded)
        {
                OutError = TEXT("Failed to submit to source control");
                return false;
        }

        for (const FString& File : ExistingFiles)
        {
                OutPerFileOk.Add(File, true);
        }

        return true;
}

bool FSourceControlService::AssetPathsToFiles(const TArray<FString>& AssetPaths, TArray<FString>& OutFiles, FString& OutError)
{
        OutFiles.Reset();
        OutError.Reset();

        for (const FString& LongPackageName : AssetPaths)
        {
                FString AssetPath = LongPackageName;
                if (AssetPath.Contains(TEXT(".")))
                {
                        AssetPath = FPackageName::ObjectPathToPackageName(AssetPath);
                }

                if (!FPackageName::IsValidLongPackageName(AssetPath))
                {
                        OutError = FString::Printf(TEXT("Invalid asset path: %s"), *LongPackageName);
                        return false;
                }

                FString AssetFilename;
                if (!FPackageName::TryConvertLongPackageNameToFilename(AssetPath, AssetFilename, FPackageName::GetAssetPackageExtension()))
                {
                        OutError = FString::Printf(TEXT("Cannot convert asset path: %s"), *LongPackageName);
                        return false;
                }

                FString MapFilename;
                if (FPackageName::TryConvertLongPackageNameToFilename(AssetPath, MapFilename, FPackageName::GetMapPackageExtension()))
                {
                        if (FPaths::FileExists(MapFilename))
                        {
                                AssetFilename = MapFilename;
                        }
                }

                OutFiles.AddUnique(NormalizeFilePath(AssetFilename));
        }

        return true;
}

void FSourceControlService::AppendResultArray(const TMap<FString, bool>& PerFileResult, TArray<TSharedPtr<FJsonValue>>& OutArray)
{
        for (const TPair<FString, bool>& Pair : PerFileResult)
        {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("file"), Pair.Key);
                Entry->SetBoolField(TEXT("ok"), Pair.Value);
                OutArray.Add(MakeShared<FJsonValueObject>(Entry));
        }
}

void FSourceControlService::AppendStatusArray(const TMap<FString, FString>& PerFileStatus, TArray<TSharedPtr<FJsonValue>>& OutArray)
{
        for (const TPair<FString, FString>& Pair : PerFileStatus)
        {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("file"), Pair.Key);
                Entry->SetStringField(TEXT("state"), Pair.Value);
                OutArray.Add(MakeShared<FJsonValueObject>(Entry));
        }
}

bool FSourceControlService::ExecuteSimpleOperation(const TArray<FString>& Files, const TFunctionRef<TSharedRef<ISourceControlOperation>(void)>& OperationFactory, TMap<FString, bool>& OutPerFileOk, FString& OutError)
{
        TArray<FString> ExistingFiles;
        CollectExistingFiles(Files, ExistingFiles);

        if (ExistingFiles.Num() == 0)
        {
                return true;
        }

        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

        bool bAllSucceeded = true;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
        const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe> Operation = OperationFactory();
        const ECommandResult::Type Result = Provider.Execute(Operation, ExistingFiles, EConcurrency::Synchronous, FSourceControlOperationComplete());
        const bool bSucceeded = Result == ECommandResult::Succeeded;
        for (const FString& File : ExistingFiles)
        {
                OutPerFileOk.Add(File, bSucceeded);
        }
        if (!bSucceeded)
        {
                bAllSucceeded = false;
                if (OutError.IsEmpty())
                {
                        OutError = TEXT("Source control operation failed");
                }
        }
#else
        for (const FString& File : ExistingFiles)
        {
                const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe> Operation = OperationFactory();
                const ECommandResult::Type Result = Provider.Execute(Operation, File);
                const bool bSucceeded = Result == ECommandResult::Succeeded;
                OutPerFileOk.Add(File, bSucceeded);

                if (!bSucceeded)
                {
                        bAllSucceeded = false;
                        if (OutError.IsEmpty())
                        {
                                OutError = TEXT("Source control operation failed");
                        }
                }
        }
#endif

        return bAllSucceeded;
}

FString FSourceControlService::DescribeState(const ISourceControlState& State)
{
        if (State.IsDeleted())
        {
                return TEXT("Deleted");
        }

        if (State.IsAdded())
        {
                return TEXT("Added");
        }

        if (State.IsCheckedOut())
        {
                return TEXT("CheckedOut");
        }

        if (State.IsModified())
        {
                return TEXT("Modified");
        }

        if (State.IsIgnored())
        {
                return TEXT("Ignored");
        }

        if (!State.IsSourceControlled())
        {
                return TEXT("Untracked");
        }

        if (State.IsSourceControlled() && !State.IsCheckedOut() && !State.IsCheckedOutOther())
        {
                return TEXT("Available");
        }

        return TEXT("Unknown");
}

bool FSourceControlService::CollectExistingFiles(const TArray<FString>& Files, TArray<FString>& OutExistingFiles)
{
        OutExistingFiles.Reset();
        for (const FString& File : Files)
        {
                if (File.IsEmpty())
                {
                        continue;
                }

                const FString Normalized = NormalizeFilePath(File);
                if (FPaths::FileExists(Normalized))
                {
                        OutExistingFiles.AddUnique(Normalized);
                }
        }

        return OutExistingFiles.Num() > 0;
}
