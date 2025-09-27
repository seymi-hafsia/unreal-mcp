#include "Permissions/WriteGate.h"
#include "UnrealMCPSettings.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "SourceControlService.h"

namespace
{
        FCriticalSection GRemoteStateMutex;
        bool GRemoteAllowWrite = false;
        bool GRemoteDryRun = true;
        TArray<FString> GRemoteAllowedPaths;
        TArray<FString> GRemoteAllowedTools;
        TArray<FString> GRemoteDeniedTools;
        static constexpr const TCHAR* TransactionLabel = TEXT("MCP Mutation");

        FString ValueToAuditString(const TSharedPtr<FJsonValue>& Value)
        {
                if (!Value.IsValid())
                {
                        return TEXT("<null>");
                }

                switch (Value->Type)
                {
                case EJson::String:
                        return Value->AsString();
                case EJson::Number:
                        return FString::SanitizeFloat(Value->AsNumber());
                case EJson::Boolean:
                        return Value->AsBool() ? TEXT("true") : TEXT("false");
                case EJson::Array:
                        return TEXT("<array>");
                case EJson::Object:
                        return TEXT("<object>");
                case EJson::Null:
                default:
                        return TEXT("<null>");
                }
        }

        TArray<FString> ReadRemoteAllowedPaths()
        {
                FScopeLock Lock(&GRemoteStateMutex);
                return GRemoteAllowedPaths;
        }

        TArray<FString> ReadRemoteAllowedTools()
        {
                FScopeLock Lock(&GRemoteStateMutex);
                return GRemoteAllowedTools;
        }

        TArray<FString> ReadRemoteDeniedTools()
        {
                FScopeLock Lock(&GRemoteStateMutex);
                return GRemoteDeniedTools;
        }

        bool MatchesTool(const FString& Pattern, const FString& Command)
        {
                return Pattern.Equals(Command, ESearchCase::IgnoreCase);
        }
}

const UUnrealMCPSettings* FWriteGate::GetSettings()
{
        return GetDefault<UUnrealMCPSettings>();
}

bool FWriteGate::IsMutationCommand(const FString& CommandType)
{
        static const TSet<FString> MutatingCommands = {
                TEXT("spawn_actor"),
                TEXT("create_actor"),
                TEXT("delete_actor"),
                TEXT("set_actor_transform"),
                TEXT("set_actor_property"),
                TEXT("spawn_blueprint_actor"),
                TEXT("create_blueprint"),
                TEXT("add_component_to_blueprint"),
                TEXT("set_component_property"),
                TEXT("set_physics_properties"),
                TEXT("compile_blueprint"),
                TEXT("set_blueprint_property"),
                TEXT("set_static_mesh_properties"),
                TEXT("set_pawn_properties"),
                TEXT("connect_blueprint_nodes"),
                TEXT("add_blueprint_get_self_component_reference"),
                TEXT("add_blueprint_self_reference"),
                TEXT("add_blueprint_event_node"),
                TEXT("add_blueprint_input_action_node"),
                TEXT("add_blueprint_function_node"),
                TEXT("add_blueprint_get_component_node"),
                TEXT("add_blueprint_variable"),
                TEXT("create_input_mapping"),
                TEXT("create_umg_widget_blueprint"),
                TEXT("add_text_block_to_widget"),
                TEXT("add_button_to_widget"),
                TEXT("bind_widget_event"),
                TEXT("set_text_block_binding"),
                TEXT("add_widget_to_viewport"),
                TEXT("sc.status"),
                TEXT("sc.checkout"),
                TEXT("sc.add"),
                TEXT("sc.revert"),
                TEXT("sc.submit")
        };

        return MutatingCommands.Contains(CommandType);
}

bool FWriteGate::IsWriteAllowed()
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (!Settings)
        {
                return false;
        }

        bool bRemoteAllow = false;
        {
                FScopeLock Lock(&GRemoteStateMutex);
                bRemoteAllow = GRemoteAllowWrite;
        }

        return Settings->AllowWrite && bRemoteAllow;
}

bool FWriteGate::ShouldDryRun()
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (!Settings)
        {
                return true;
        }

        bool bRemoteDryRun = true;
        {
                FScopeLock Lock(&GRemoteStateMutex);
                bRemoteDryRun = GRemoteDryRun;
        }

        return Settings->DryRun || bRemoteDryRun;
}

TArray<FString> FWriteGate::GetEffectiveAllowedRoots()
{
        const UUnrealMCPSettings* Settings = GetSettings();
        TArray<FString> LocalRoots;
        if (Settings)
        {
                for (const FDirectoryPath& DirectoryPath : Settings->AllowedContentRoots)
                {
                        const FString Normalized = NormalizeContentPath(DirectoryPath.Path);
                        if (!Normalized.IsEmpty())
                        {
                                LocalRoots.AddUnique(Normalized);
                        }
                }
        }

        const TArray<FString> RemoteRoots = ReadRemoteAllowedPaths();

        if (LocalRoots.Num() == 0 || RemoteRoots.Num() == 0)
        {
                return {};
        }

        TArray<FString> Intersection;
        for (const FString& LocalRoot : LocalRoots)
        {
                for (const FString& RemoteRoot : RemoteRoots)
                {
                        if (LocalRoot.StartsWith(RemoteRoot))
                        {
                                Intersection.AddUnique(LocalRoot);
                        }
                        else if (RemoteRoot.StartsWith(LocalRoot))
                        {
                                Intersection.AddUnique(RemoteRoot);
                        }
                }
        }

        return Intersection;
}

bool FWriteGate::IsPathAllowed(const FString& ContentPath, FString& OutReason)
{
        if (ContentPath.IsEmpty())
        {
                return true;
        }

        const FString Normalized = NormalizeContentPath(ContentPath);
        if (Normalized.IsEmpty())
        {
                OutReason = TEXT("Invalid content path");
                return false;
        }

        const TArray<FString> AllowedRoots = GetEffectiveAllowedRoots();
        if (AllowedRoots.Num() == 0)
        {
                OutReason = TEXT("No allowed content roots configured");
                return false;
        }

        for (const FString& Root : AllowedRoots)
        {
                if (Normalized.StartsWith(Root))
                {
                        return true;
                }
        }

        OutReason = FString::Printf(TEXT("Path %s is not within allowed roots"), *Normalized);
        return false;
}

bool FWriteGate::CanMutate(const FString& CommandType, const FString& ContentPath, FString& OutReason)
{
        if (!IsToolAllowed(CommandType, OutReason))
        {
                return false;
        }

        if (!IsWriteAllowed() && CommandType != TEXT("sc.status"))
        {
                OutReason = TEXT("Write operations are disabled (allowWrite=false)");
                return false;
        }

        if (!IsPathAllowed(ContentPath, OutReason))
        {
                return false;
        }

        return true;
}

bool FWriteGate::IsToolAllowed(const FString& CommandType, FString& OutReason)
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (Settings)
        {
                for (const FString& DeniedTool : Settings->DeniedTools)
                {
                        FString Normalized = DeniedTool;
                        Normalized.TrimStartAndEndInline();
                        if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                        {
                                OutReason = TEXT("Tool denied by project settings");
                                return false;
                        }
                }

                if (Settings->AllowedTools.Num() > 0)
                {
                        bool bAllowed = false;
                        for (const FString& AllowedTool : Settings->AllowedTools)
                        {
                                FString Normalized = AllowedTool;
                                Normalized.TrimStartAndEndInline();
                                if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                                {
                                        bAllowed = true;
                                        break;
                                }
                        }

                        if (!bAllowed)
                        {
                                OutReason = TEXT("Tool not present in AllowedTools");
                                return false;
                        }
                }
        }

        const TArray<FString> RemoteDenied = ReadRemoteDeniedTools();
        for (const FString& DeniedTool : RemoteDenied)
        {
                FString Normalized = DeniedTool;
                Normalized.TrimStartAndEndInline();
                if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                {
                        OutReason = TEXT("Tool denied by remote enforcement");
                        return false;
                }
        }

        const TArray<FString> RemoteAllowed = ReadRemoteAllowedTools();
        if (RemoteAllowed.Num() > 0)
        {
                bool bAllowed = false;
                for (const FString& AllowedTool : RemoteAllowed)
                {
                        FString Normalized = AllowedTool;
                        Normalized.TrimStartAndEndInline();
                        if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                        {
                                bAllowed = true;
                                break;
                        }
                }

                if (!bAllowed)
                {
                        OutReason = TEXT("Tool not permitted by remote enforcement");
                        return false;
                }
        }

        OutReason.Reset();
        return true;
}

FMutationPlan FWriteGate::BuildPlan(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
        FMutationPlan Plan;
        Plan.bDryRun = ShouldDryRun();

        bool bAddedSpecificActions = false;

        if (CommandType.StartsWith(TEXT("sc.")) && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("assets"), AssetsArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *AssetsArray)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        FMutationAction AssetAction;
                                        AssetAction.Op = CommandType;
                                        AssetAction.Args.Add(TEXT("asset"), NormalizeContentPath(Value->AsString()));
                                        Plan.Actions.Add(AssetAction);
                                        bAddedSpecificActions = true;
                                }
                        }
                }

                const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
                if (Params->TryGetArrayField(TEXT("files"), FilesArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *FilesArray)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        FMutationAction FileAction;
                                        FileAction.Op = CommandType;
                                        FileAction.Args.Add(TEXT("file"), Value->AsString());
                                        Plan.Actions.Add(FileAction);
                                        bAddedSpecificActions = true;
                                }
                        }
                }
        }

        if (!bAddedSpecificActions)
        {
                FMutationAction Action;
                Action.Op = CommandType;

                if (Params.IsValid())
                {
                        for (const auto& Pair : Params->Values)
                        {
                                Action.Args.Add(Pair.Key, ValueToAuditString(Pair.Value));
                        }
                }

                Plan.Actions.Add(Action);
        }

        return Plan;
}

TSharedPtr<FJsonObject> FWriteGate::BuildAuditJson(const FMutationPlan& Plan, bool bExecuted)
{
        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("mutation"), true);
        Audit->SetBoolField(TEXT("dryRun"), Plan.bDryRun);
        Audit->SetBoolField(TEXT("executed"), bExecuted);
        Audit->SetStringField(TEXT("transaction"), GetTransactionName());
        Audit->SetBoolField(TEXT("undoAvailable"), bExecuted);

        TArray<TSharedPtr<FJsonValue>> Actions;
        for (const FMutationAction& Action : Plan.Actions)
        {
                        TSharedPtr<FJsonObject> ActionObject = MakeShared<FJsonObject>();
                        ActionObject->SetStringField(TEXT("op"), Action.Op);

                        TSharedPtr<FJsonObject> ArgsObject = MakeShared<FJsonObject>();
                        for (const auto& ArgPair : Action.Args)
                        {
                                ArgsObject->SetStringField(ArgPair.Key, ArgPair.Value);
                        }
                        ActionObject->SetObjectField(TEXT("args"), ArgsObject);
                        Actions.Add(MakeShared<FJsonValueObject>(ActionObject));
        }

        Audit->SetArrayField(TEXT("actions"), Actions);
        return Audit;
}

const FString& FWriteGate::GetTransactionName()
{
        static const FString TransactionName(TransactionLabel);
        return TransactionName;
}

bool FWriteGate::EnsureCheckoutForContentPath(const FString& ContentPath, TSharedPtr<FJsonObject>& OutError)
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (!Settings || !Settings->RequireCheckout)
        {
                return true;
        }

        if (ContentPath.IsEmpty())
        {
                return true;
        }

        FString PackagePath = ContentPath;
        if (PackagePath.Contains(TEXT(".")))
        {
                PackagePath = FPackageName::ObjectPathToPackageName(PackagePath);
        }

        if (!FPackageName::IsValidLongPackageName(PackagePath))
        {
                return true;
        }

        TArray<FString> AssetPaths;
        AssetPaths.Add(PackagePath);

        TArray<FString> Files;
        FString ConversionError;
        if (!FSourceControlService::AssetPathsToFiles(AssetPaths, Files, ConversionError))
        {
                OutError = MakeSourceControlRequiredError(ContentPath, ConversionError);
                return false;
        }

        if (Files.Num() == 0)
        {
                return true;
        }

        TMap<FString, bool> PerFileResult;
        FString CheckoutError;
        if (!FSourceControlService::Checkout(Files, PerFileResult, CheckoutError))
        {
                OutError = MakeSourceControlRequiredError(ContentPath, CheckoutError);
                return false;
        }

        for (const TPair<FString, bool>& Pair : PerFileResult)
        {
                if (!Pair.Value)
                {
                        OutError = MakeSourceControlRequiredError(ContentPath, CheckoutError);
                        return false;
                }
        }

        return true;
}

TSharedPtr<FJsonObject> FWriteGate::MakeSourceControlRequiredError(const FString& AssetPath, const FString& FailureMessage)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("SOURCE_CONTROL_REQUIRED"));
        Error->SetStringField(TEXT("message"), TEXT("Asset must be checked out before mutation"));

        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("asset"), NormalizeContentPath(AssetPath));
        if (!FailureMessage.IsEmpty())
        {
                Details->SetStringField(TEXT("reason"), FailureMessage);
        }

        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

TSharedPtr<FJsonObject> FWriteGate::MakeWriteNotAllowedError(const FString& CommandType)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("WRITE_NOT_ALLOWED"));
        Error->SetStringField(TEXT("message"), TEXT("Write operations are disabled (allowWrite=false)"));
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("tool"), CommandType);
        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

TSharedPtr<FJsonObject> FWriteGate::MakePathNotAllowedError(const FString& Path, const FString& Reason)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("PATH_NOT_ALLOWED"));
        Error->SetStringField(TEXT("message"), Reason);
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("path"), NormalizeContentPath(Path));
        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

TSharedPtr<FJsonObject> FWriteGate::MakeToolNotAllowedError(const FString& CommandType, const FString& Reason)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("TOOL_DENIED"));
        Error->SetStringField(TEXT("message"), Reason);

        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("tool"), CommandType);
        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

void FWriteGate::UpdateRemoteEnforcement(bool bAllowWrite, bool bDryRun, const TArray<FString>& AllowedPaths, const TArray<FString>& AllowedTools, const TArray<FString>& DeniedTools)
{
        FScopeLock Lock(&GRemoteStateMutex);
        GRemoteAllowWrite = bAllowWrite;
        GRemoteDryRun = bDryRun;

        GRemoteAllowedPaths.Reset();
        for (const FString& Path : AllowedPaths)
        {
                const FString Normalized = NormalizeContentPath(Path);
                if (!Normalized.IsEmpty())
                {
                        GRemoteAllowedPaths.AddUnique(Normalized);
                }
        }

        GRemoteAllowedTools.Reset();
        for (const FString& Tool : AllowedTools)
        {
                FString Normalized = Tool;
                Normalized.TrimStartAndEndInline();
                if (!Normalized.IsEmpty())
                {
                        GRemoteAllowedTools.AddUnique(Normalized);
                }
        }

        GRemoteDeniedTools.Reset();
        for (const FString& Tool : DeniedTools)
        {
                FString Normalized = Tool;
                Normalized.TrimStartAndEndInline();
                if (!Normalized.IsEmpty())
                {
                        GRemoteDeniedTools.AddUnique(Normalized);
                }
        }
}

FString FWriteGate::ResolvePathForCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return FString();
        }

        if (CommandType.StartsWith(TEXT("sc.")))
        {
                const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("assets"), AssetsArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *AssetsArray)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        return NormalizeContentPath(Value->AsString());
                                }
                        }
                }
        }

        static const TArray<FString> CandidateKeys = {
                TEXT("path"),
                TEXT("asset_path"),
                TEXT("asset"),
                TEXT("blueprint_path"),
                TEXT("content_path"),
                TEXT("target_path"),
                TEXT("parent_path"),
                TEXT("widget_path"),
                TEXT("source_path"),
                TEXT("package_path")
        };

        for (const FString& Key : CandidateKeys)
        {
                if (Params->HasField(Key) && Params->HasTypedField<EJson::String>(Key))
                {
                        return NormalizeContentPath(Params->GetStringField(Key));
                }
        }

        // Some commands use "name" to represent a content path (e.g. blueprint creation)
        if (CommandType.Contains(TEXT("blueprint")) && Params->HasTypedField<EJson::String>(TEXT("name")))
        {
                return NormalizeContentPath(Params->GetStringField(TEXT("name")));
        }

        if (Params->HasTypedField<EJson::String>(TEXT("widget_name")))
        {
                return NormalizeContentPath(Params->GetStringField(TEXT("widget_name")));
        }

        return FString();
}

FString FWriteGate::NormalizeContentPath(const FString& InPath)
{
        FString Trimmed = InPath;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
                return FString();
        }

        if (!Trimmed.StartsWith(TEXT("/")))
        {
                Trimmed = FString::Printf(TEXT("/Game/%s"), *Trimmed);
        }

        return Trimmed;
}
