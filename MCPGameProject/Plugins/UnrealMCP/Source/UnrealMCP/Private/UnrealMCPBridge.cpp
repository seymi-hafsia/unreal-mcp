#include "UnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include <stdexcept>
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Include our new command handler classes
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPSourceControlCommands.h"
#include "Content/ContentTools.h"
#include "Assets/AssetCrud.h"
#include "Assets/AssetImport.h"
#include "Assets/AssetQuery.h"
#include "Niagara/NiagaraTools.h"
#include "MetaSounds/MetaSoundTools.h"
#include "Actors/ActorTools.h"
#include "EditorNav/EditorNavTools.h"
#include "Levels/LevelTools.h"
#include "Sequencer/SequenceBindings.h"
#include "Sequencer/SequenceExport.h"
#include "Sequencer/SequenceTools.h"
#include "Sequencer/SequenceTracks.h"
#include "Materials/MaterialApplyTools.h"
#include "Materials/MaterialInstanceTools.h"
#include "Permissions/WriteGate.h"
#include "Transactions/TransactionManager.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

#include "Misc/ScopeExit.h"

UUnrealMCPBridge::UUnrealMCPBridge()
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();
    SourceControlCommands = MakeShared<FUnrealMCPSourceControlCommands>();
    ContentTools = MakeShared<FContentTools>();

    FWriteGate::UpdateRemoteEnforcement(false, true, TArray<FString>(), TArray<FString>(), TArray<FString>());
}

UUnrealMCPBridge::~UUnrealMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintNodeCommands.Reset();
    ProjectCommands.Reset();
    UMGCommands.Reset();
    SourceControlCommands.Reset();
    ContentTools.Reset();
}

// Initialize subsystem
void UUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Initializing"));

    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerThread = nullptr;

    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
    if (Settings)
    {
        FString Host = Settings->ServerHost;
        Host.TrimStartAndEndInline();

        FIPv4Address ParsedAddress;
        if (!FIPv4Address::Parse(Host, ParsedAddress))
        {
            if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
            {
                ParsedAddress = FIPv4Address::InternalLoopback;
            }
            else
            {
                ParsedAddress = FIPv4Address::Any;
                UE_LOG(LogUnrealMCP, Warning, TEXT("UnrealMCPBridge: Invalid ServerHost '%s', defaulting to %s"), *Host, *ParsedAddress.ToString());
            }
        }

        ServerAddress = ParsedAddress;
        Port = static_cast<uint16>(FMath::Clamp(Settings->ServerPort, 1, 65535));

        if (Settings->bAutoConnectOnEditorStartup)
        {
            StartServer();
        }
    }
    else
    {
        ServerAddress = FIPv4Address::InternalLoopback;
        Port = 12029;
        UE_LOG(LogUnrealMCP, Warning, TEXT("UnrealMCPBridge: Failed to load settings, using defaults"));
        StartServer();
    }
}

// Clean up resources when subsystem is destroyed
void UUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Shutting down"));
    StopServer();
}

// Start the MCP server
void UUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
    if (!Settings)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Cannot start server without settings"));
        return;
    }

    FString Host = Settings->ServerHost;
    Host.TrimStartAndEndInline();

    FIPv4Address BindAddress;
    if (!FIPv4Address::Parse(Host, BindAddress))
    {
        if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
        {
            BindAddress = FIPv4Address::InternalLoopback;
        }
        else if (Host.Equals(TEXT("0.0.0.0"), ESearchCase::IgnoreCase))
        {
            BindAddress = FIPv4Address::Any;
        }
        else
        {
            BindAddress = ServerAddress;
            UE_LOG(LogUnrealMCP, Warning, TEXT("UnrealMCPBridge: Unable to parse ServerHost '%s', using %s"), *Host, *BindAddress.ToString());
        }
    }
    ServerAddress = BindAddress;

    Port = static_cast<uint16>(FMath::Clamp(Settings->ServerPort, 1, 65535));

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(BindAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to bind listener socket to %s:%d"), *BindAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *BindAddress.ToString(), Port);

    // Start server thread
    FMCPServerConfig ServerConfig;
    ServerConfig.HandshakeTimeoutSeconds = Settings->ConnectTimeoutSec;
    ServerConfig.ReadTimeoutSeconds = Settings->ReadTimeoutSec;
    ServerConfig.HeartbeatIntervalSeconds = Settings->HeartbeatIntervalSec;

    ServerThread = FRunnableThread::Create(
        new FMCPServerRunnable(this, ListenerSocket, ServerConfig),
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
        StopServer();
        return;
    }
}

// Stop the MCP server
void UUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerThread)
    {
        ServerThread->Kill(true);
        delete ServerThread;
        ServerThread = nullptr;
    }

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket.Get());
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket.Get());
        ListenerSocket.Reset();
    }

    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

// Execute a command received from a client
FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FString& RequestId)
{
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Executing command: %s (requestId=%s)"), *CommandType, *RequestId);

    // Create a promise to wait for the result
    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();

    // Queue execution on Game Thread
    AsyncTask(ENamedThreads::GameThread, [this, CommandType, Params, Promise = MoveTemp(Promise)]() mutable
    {
        TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();

        try
        {
            TSharedPtr<FJsonObject> ResultJson;
            const bool bIsMutation = FWriteGate::IsMutationCommand(CommandType, Params);
            const FString TargetPath = FWriteGate::ResolvePathForCommand(CommandType, Params);
            FMutationPlan MutationPlan;
            bool bSkipExecution = false;
            TSharedPtr<FJsonObject> AuditJson;

            FString ToolReason;
            if (!FWriteGate::IsToolAllowed(CommandType, ToolReason))
            {
                ResponseJson->SetBoolField(TEXT("ok"), false);
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetObjectField(TEXT("error"), FWriteGate::MakeToolNotAllowedError(CommandType, ToolReason));

                if (bIsMutation)
                {
                    MutationPlan = FWriteGate::BuildPlan(CommandType, Params);
                    MutationPlan.bDryRun = true;
                    AuditJson = FWriteGate::BuildAuditJson(MutationPlan, false);
                    ResponseJson->SetObjectField(TEXT("audit"), AuditJson);
                }

                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson, Writer, /*bCloseWriter=*/true);
                Promise.SetValue(ResultString);
                return;
            }

            if (bIsMutation)
            {
                MutationPlan = FWriteGate::BuildPlan(CommandType, Params);

                FString GateReason;
                if (!FWriteGate::CanMutate(CommandType, TargetPath, GateReason))
                {
                    TSharedPtr<FJsonObject> ErrorObject;
                    if (!FWriteGate::IsWriteAllowed())
                    {
                        ErrorObject = FWriteGate::MakeWriteNotAllowedError(CommandType);
                    }
                    else
                    {
                        ErrorObject = FWriteGate::MakePathNotAllowedError(TargetPath, GateReason);
                    }

                    ResponseJson->SetBoolField(TEXT("ok"), false);
                    ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                    ResponseJson->SetObjectField(TEXT("error"), ErrorObject);

                    MutationPlan.bDryRun = true;
                    AuditJson = FWriteGate::BuildAuditJson(MutationPlan, false);
                    bSkipExecution = true;
                }
                else if (FWriteGate::ShouldDryRun())
                {
                    MutationPlan.bDryRun = true;
                    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
                    ResultPayload->SetBoolField(TEXT("planned"), true);

                    ResponseJson->SetBoolField(TEXT("ok"), true);
                    ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                    ResponseJson->SetObjectField(TEXT("result"), ResultPayload);

                    AuditJson = FWriteGate::BuildAuditJson(MutationPlan, false);
                    bSkipExecution = true;
                }
            }

            if (!bSkipExecution)
            {
                if (bIsMutation && !CommandType.StartsWith(TEXT("sc.")))
                {
                    TSharedPtr<FJsonObject> CheckoutError;
                    if (!FWriteGate::EnsureCheckoutForContentPath(TargetPath, CheckoutError))
                    {
                        ResponseJson->SetBoolField(TEXT("ok"), false);
                        ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                        ResponseJson->SetObjectField(TEXT("error"), CheckoutError);

                        MutationPlan.bDryRun = false;
                        AuditJson = FWriteGate::BuildAuditJson(MutationPlan, false);

                        FString ResultString;
                        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                        FJsonSerializer::Serialize(ResponseJson, Writer, /*bCloseWriter=*/true);
                        Promise.SetValue(ResultString);
                        return;
                    }
                }

                bool bTransactionActive = false;
                if (bIsMutation)
                {
                    FTransactionManager::Begin(FWriteGate::GetTransactionName());
                    bTransactionActive = true;
                }

                ON_SCOPE_EXIT
                {
                    if (bTransactionActive)
                    {
                        FTransactionManager::End();
                    }
                };

                if (CommandType == TEXT("ping"))
                {
                    ResultJson = MakeShared<FJsonObject>();
                    ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
                }
                // Editor Commands (including actor manipulation)
                else if (CommandType == TEXT("get_actors_in_level") ||
                         CommandType == TEXT("find_actors_by_name") ||
                         CommandType == TEXT("spawn_actor") ||
                         CommandType == TEXT("create_actor") ||
                         CommandType == TEXT("delete_actor") ||
                         CommandType == TEXT("set_actor_transform") ||
                         CommandType == TEXT("get_actor_properties") ||
                         CommandType == TEXT("set_actor_property") ||
                         CommandType == TEXT("spawn_blueprint_actor") ||
                         CommandType == TEXT("focus_viewport") ||
                         CommandType == TEXT("take_screenshot"))
                {
                    ResultJson = EditorCommands->HandleCommand(CommandType, Params);
                }
                // Blueprint Commands
                else if (CommandType == TEXT("create_blueprint") ||
                         CommandType == TEXT("add_component_to_blueprint") ||
                         CommandType == TEXT("set_component_property") ||
                         CommandType == TEXT("set_physics_properties") ||
                         CommandType == TEXT("compile_blueprint") ||
                         CommandType == TEXT("set_blueprint_property") ||
                         CommandType == TEXT("set_static_mesh_properties") ||
                         CommandType == TEXT("set_pawn_properties"))
                {
                    ResultJson = BlueprintCommands->HandleCommand(CommandType, Params);
                }
                // Blueprint Node Commands
                else if (CommandType == TEXT("connect_blueprint_nodes") ||
                         CommandType == TEXT("add_blueprint_get_self_component_reference") ||
                         CommandType == TEXT("add_blueprint_self_reference") ||
                         CommandType == TEXT("find_blueprint_nodes") ||
                         CommandType == TEXT("add_blueprint_event_node") ||
                         CommandType == TEXT("add_blueprint_input_action_node") ||
                         CommandType == TEXT("add_blueprint_function_node") ||
                         CommandType == TEXT("add_blueprint_get_component_node") ||
                         CommandType == TEXT("add_blueprint_variable"))
                {
                    ResultJson = BlueprintNodeCommands->HandleCommand(CommandType, Params);
                }
                // Project Commands
                else if (CommandType == TEXT("create_input_mapping"))
                {
                    ResultJson = ProjectCommands->HandleCommand(CommandType, Params);
                }
                // UMG Commands
                else if (CommandType == TEXT("create_umg_widget_blueprint") ||
                         CommandType == TEXT("add_text_block_to_widget") ||
                         CommandType == TEXT("add_button_to_widget") ||
                         CommandType == TEXT("bind_widget_event") ||
                         CommandType == TEXT("set_text_block_binding") ||
                         CommandType == TEXT("add_widget_to_viewport"))
                {
                    ResultJson = UMGCommands->HandleCommand(CommandType, Params);
                }
                else if (CommandType == TEXT("asset.find") ||
                         CommandType == TEXT("asset.exists") ||
                         CommandType == TEXT("asset.metadata"))
                {
                    if (CommandType == TEXT("asset.find"))
                    {
                        FAssetFindParams QueryParams;
                        if (Params.IsValid())
                        {
                            const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
                            if (Params->TryGetArrayField(TEXT("paths"), PathsArray))
                            {
                                for (const TSharedPtr<FJsonValue>& Value : *PathsArray)
                                {
                                    if (Value.IsValid() && Value->Type == EJson::String)
                                    {
                                        FString PathValue = Value->AsString();
                                        PathValue.TrimStartAndEndInline();
                                        if (!PathValue.IsEmpty())
                                        {
                                            QueryParams.Paths.Add(MoveTemp(PathValue));
                                        }
                                    }
                                }
                            }

                            const TArray<TSharedPtr<FJsonValue>>* ClassArray = nullptr;
                            if (Params->TryGetArrayField(TEXT("classNames"), ClassArray))
                            {
                                for (const TSharedPtr<FJsonValue>& Value : *ClassArray)
                                {
                                    if (Value.IsValid() && Value->Type == EJson::String)
                                    {
                                        FString ClassName = Value->AsString();
                                        ClassName.TrimStartAndEndInline();
                                        if (!ClassName.IsEmpty())
                                        {
                                            QueryParams.ClassNames.Add(MoveTemp(ClassName));
                                        }
                                    }
                                }
                            }

                            FString NameContains;
                            if (Params->TryGetStringField(TEXT("nameContains"), NameContains))
                            {
                                NameContains.TrimStartAndEndInline();
                                if (!NameContains.IsEmpty())
                                {
                                    QueryParams.NameContains = NameContains;
                                }
                            }

                            const TSharedPtr<FJsonObject>* TagQueryObject = nullptr;
                            if (Params->TryGetObjectField(TEXT("tagQuery"), TagQueryObject))
                            {
                                for (const auto& TagPair : (*TagQueryObject)->Values)
                                {
                                    TArray<FString> TagValues;
                                    if (TagPair.Value->Type == EJson::Array)
                                    {
                                        for (const TSharedPtr<FJsonValue>& TagValue : TagPair.Value->AsArray())
                                        {
                                            if (TagValue->Type == EJson::String)
                                            {
                                                TagValues.Add(TagValue->AsString());
                                            }
                                            else if (TagValue->Type == EJson::Number)
                                            {
                                                TagValues.Add(FString::SanitizeFloat(TagValue->AsNumber()));
                                            }
                                            else if (TagValue->Type == EJson::Boolean)
                                            {
                                                TagValues.Add(TagValue->AsBool() ? TEXT("true") : TEXT("false"));
                                            }
                                        }
                                    }
                                    else if (TagPair.Value->Type == EJson::String)
                                    {
                                        TagValues.Add(TagPair.Value->AsString());
                                    }
                                    else if (TagPair.Value->Type == EJson::Number)
                                    {
                                        TagValues.Add(FString::SanitizeFloat(TagPair.Value->AsNumber()));
                                    }
                                    else if (TagPair.Value->Type == EJson::Boolean)
                                    {
                                        TagValues.Add(TagPair.Value->AsBool() ? TEXT("true") : TEXT("false"));
                                    }

                                    if (TagValues.Num() > 0)
                                    {
                                        QueryParams.TagQuery.Add(FName(*TagPair.Key), MoveTemp(TagValues));
                                    }
                                }
                            }

                            if (Params->HasTypedField<EJson::Boolean>(TEXT("recursive")))
                            {
                                QueryParams.bRecursive = Params->GetBoolField(TEXT("recursive"));
                            }

                            if (Params->HasTypedField<EJson::Number>(TEXT("limit")))
                            {
                                QueryParams.Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
                            }

                            if (Params->HasTypedField<EJson::Number>(TEXT("offset")))
                            {
                                QueryParams.Offset = static_cast<int32>(Params->GetNumberField(TEXT("offset")));
                            }

                            const TSharedPtr<FJsonObject>* SortObject = nullptr;
                            if (Params->TryGetObjectField(TEXT("sort"), SortObject) && SortObject->IsValid())
                            {
                                FString SortBy;
                                if ((*SortObject)->TryGetStringField(TEXT("by"), SortBy))
                                {
                                    if (SortBy.Equals(TEXT("class"), ESearchCase::IgnoreCase))
                                    {
                                        QueryParams.SortBy = FAssetFindParams::ESortBy::Class;
                                    }
                                    else if (SortBy.Equals(TEXT("path"), ESearchCase::IgnoreCase))
                                    {
                                        QueryParams.SortBy = FAssetFindParams::ESortBy::Path;
                                    }
                                    else
                                    {
                                        QueryParams.SortBy = FAssetFindParams::ESortBy::Name;
                                    }
                                }

                                FString SortOrder;
                                if ((*SortObject)->TryGetStringField(TEXT("order"), SortOrder))
                                {
                                    QueryParams.bSortAscending = !SortOrder.Equals(TEXT("desc"), ESearchCase::IgnoreCase);
                                }
                            }
                        }

                        if (QueryParams.Paths.Num() == 0 &&
                            QueryParams.ClassNames.Num() == 0 &&
                            !QueryParams.NameContains.IsSet() &&
                            QueryParams.TagQuery.Num() == 0)
                        {
                            ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Provide at least one filter (paths, classNames, nameContains, or tagQuery)"));
                            ResultJson->SetStringField(TEXT("errorCode"), TEXT("ASSET_FIND_INVALID_FILTER"));
                        }
                        else
                        {
                            int32 Total = 0;
                            TArray<FAssetLite> Items;
                            FString QueryError;
                            if (!FAssetQuery::Find(QueryParams, Total, Items, QueryError))
                            {
                                ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(QueryError);
                                ResultJson->SetStringField(TEXT("errorCode"), TEXT("ASSET_FIND_FAILED"));
                            }
                            else
                            {
                                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                                Data->SetNumberField(TEXT("total"), Total);

                                TArray<TSharedPtr<FJsonValue>> ItemsArray;
                                for (const FAssetLite& Item : Items)
                                {
                                    TSharedPtr<FJsonObject> ItemObject = MakeShared<FJsonObject>();
                                    ItemObject->SetStringField(TEXT("objectPath"), Item.ObjectPath);
                                    ItemObject->SetStringField(TEXT("packagePath"), Item.PackagePath);
                                    ItemObject->SetStringField(TEXT("assetName"), Item.AssetName);
                                    ItemObject->SetStringField(TEXT("class"), Item.ClassName);

                                    TSharedPtr<FJsonObject> TagsJson = MakeShared<FJsonObject>();
                                    for (const TPair<FString, TArray<FString>>& TagPair : Item.Tags)
                                    {
                                        TArray<TSharedPtr<FJsonValue>> TagValues;
                                        for (const FString& TagValue : TagPair.Value)
                                        {
                                            TagValues.Add(MakeShared<FJsonValueString>(TagValue));
                                        }
                                        TagsJson->SetArrayField(TagPair.Key, TagValues);
                                    }

                                    ItemObject->SetObjectField(TEXT("tags"), TagsJson);
                                    ItemsArray.Add(MakeShared<FJsonValueObject>(ItemObject));
                                }

                                Data->SetArrayField(TEXT("items"), ItemsArray);
                                ResultJson = FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
                            }
                        }
                    }
                    else if (CommandType == TEXT("asset.exists"))
                    {
                        FString ObjectPath;
                        if (Params.IsValid() && Params->TryGetStringField(TEXT("objectPath"), ObjectPath))
                        {
                            ObjectPath.TrimStartAndEndInline();
                            bool bExists = false;
                            FString ClassName;
                            FString ExistsError;
                            if (!FAssetQuery::Exists(ObjectPath, bExists, ClassName, ExistsError))
                            {
                                ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(ExistsError);
                                ResultJson->SetStringField(TEXT("errorCode"), TEXT("ASSET_EXISTS_FAILED"));
                            }
                            else
                            {
                                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                                Data->SetBoolField(TEXT("exists"), bExists);
                                if (!ClassName.IsEmpty())
                                {
                                    Data->SetStringField(TEXT("class"), ClassName);
                                }
                                ResultJson = FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
                            }
                        }
                        else
                        {
                            ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing objectPath parameter"));
                            ResultJson->SetStringField(TEXT("errorCode"), TEXT("ASSET_EXISTS_FAILED"));
                        }
                    }
                    else if (CommandType == TEXT("asset.metadata"))
                    {
                        FString ObjectPath;
                        if (Params.IsValid() && Params->TryGetStringField(TEXT("objectPath"), ObjectPath))
                        {
                            ObjectPath.TrimStartAndEndInline();
                            TSharedPtr<FJsonObject> MetadataJson;
                            FString MetadataError;
                            if (!FAssetQuery::Metadata(ObjectPath, MetadataJson, MetadataError))
                            {
                                ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(MetadataError);
                                ResultJson->SetStringField(TEXT("errorCode"), TEXT("ASSET_METADATA_FAILED"));
                            }
                            else
                            {
                                ResultJson = FUnrealMCPCommonUtils::CreateSuccessResponse(MetadataJson);
                            }
                        }
                        else
                        {
                            ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing objectPath parameter"));
                            ResultJson->SetStringField(TEXT("errorCode"), TEXT("ASSET_METADATA_FAILED"));
                        }
                    }
                }
                else if (CommandType == TEXT("actor.spawn"))
                {
                    ResultJson = FActorTools::Spawn(Params);
                }
                else if (CommandType == TEXT("actor.destroy"))
                {
                    ResultJson = FActorTools::Destroy(Params);
                }
                else if (CommandType == TEXT("actor.attach"))
                {
                    ResultJson = FActorTools::Attach(Params);
                }
                else if (CommandType == TEXT("actor.transform"))
                {
                    ResultJson = FActorTools::Transform(Params);
                }
                else if (CommandType == TEXT("actor.tag"))
                {
                    ResultJson = FActorTools::Tag(Params);
                }
                else if (CommandType == TEXT("level.save_open"))
                {
                    ResultJson = FLevelTools::SaveOpen(Params);
                }
                else if (CommandType == TEXT("level.load"))
                {
                    ResultJson = FLevelTools::Load(Params);
                }
                else if (CommandType == TEXT("level.unload"))
                {
                    ResultJson = FLevelTools::Unload(Params);
                }
                else if (CommandType == TEXT("level.stream_sublevel"))
                {
                    ResultJson = FLevelTools::StreamSublevel(Params);
                }
                else if (CommandType == TEXT("level.select"))
                {
                    ResultJson = FEditorNavTools::LevelSelect(Params);
                }
                else if (CommandType == TEXT("viewport.focus"))
                {
                    ResultJson = FEditorNavTools::ViewportFocus(Params);
                }
                else if (CommandType == TEXT("camera.bookmark"))
                {
                    ResultJson = FEditorNavTools::CameraBookmark(Params);
                }
                else if (CommandType == TEXT("asset.create_folder"))
                {
                    ResultJson = FAssetCrud::CreateFolder(Params);
                }
                else if (CommandType == TEXT("asset.rename"))
                {
                    ResultJson = FAssetCrud::Rename(Params);
                }
                else if (CommandType == TEXT("asset.delete"))
                {
                    ResultJson = FAssetCrud::Delete(Params);
                }
                else if (CommandType == TEXT("niagara.spawn_component"))
                {
                    ResultJson = FNiagaraTools::SpawnComponent(Params);
                }
                else if (CommandType == TEXT("niagara.set_user_params"))
                {
                    ResultJson = FNiagaraTools::SetUserParameters(Params);
                }
                else if (CommandType == TEXT("niagara.activate"))
                {
                    ResultJson = FNiagaraTools::Activate(Params);
                }
                else if (CommandType == TEXT("niagara.deactivate"))
                {
                    ResultJson = FNiagaraTools::Deactivate(Params);
                }
                else if (CommandType == TEXT("metasound.spawn_component"))
                {
                    ResultJson = FMetaSoundTools::SpawnComponent(Params);
                }
                else if (CommandType == TEXT("metasound.set_params"))
                {
                    ResultJson = FMetaSoundTools::SetParameters(Params);
                }
                else if (CommandType == TEXT("metasound.play"))
                {
                    ResultJson = FMetaSoundTools::Play(Params);
                }
                else if (CommandType == TEXT("metasound.stop"))
                {
                    ResultJson = FMetaSoundTools::Stop(Params);
                }
                else if (CommandType == TEXT("metasound.export_info"))
                {
                    ResultJson = FMetaSoundTools::ExportInfo(Params);
                }
                else if (CommandType == TEXT("metasound.patch_preset"))
                {
                    ResultJson = FMetaSoundTools::PatchPreset(Params);
                }
                else if (CommandType == TEXT("asset.fix_redirectors"))
                {
                    ResultJson = FAssetCrud::FixRedirectors(Params);
                }
                else if (CommandType == TEXT("asset.save_all"))
                {
                    ResultJson = FAssetCrud::SaveAll(Params);
                }
                else if (CommandType == TEXT("asset.batch_import"))
                {
                    ResultJson = FAssetImport::BatchImport(Params);
                }
                else if (CommandType == TEXT("content.scan") ||
                         CommandType == TEXT("content.validate") ||
                         CommandType == TEXT("content.fix_missing") ||
                         CommandType == TEXT("content.generate_thumbnails"))
                {
                    ResultJson = ContentTools->HandleCommand(CommandType, Params);
                }
                else if (CommandType == TEXT("mi.create"))
                {
                    ResultJson = FMaterialInstanceTools::Create(Params);
                }
                else if (CommandType == TEXT("mi.set_params"))
                {
                    ResultJson = FMaterialInstanceTools::SetParameters(Params);
                }
                else if (CommandType == TEXT("mi.batch_apply"))
                {
                    ResultJson = FMaterialApplyTools::BatchApply(Params);
                }
                else if (CommandType == TEXT("mesh.remap_material_slots"))
                {
                    ResultJson = FMaterialApplyTools::RemapMaterialSlots(Params);
                }
                else if (CommandType == TEXT("sequence.create"))
                {
                    ResultJson = FSequenceTools::Create(Params);
                }
                else if (CommandType == TEXT("sequence.bind_actors"))
                {
                    ResultJson = FSequenceBindings::BindActors(Params);
                }
                else if (CommandType == TEXT("sequence.unbind"))
                {
                    ResultJson = FSequenceBindings::Unbind(Params);
                }
                else if (CommandType == TEXT("sequence.list_bindings"))
                {
                    ResultJson = FSequenceBindings::List(Params);
                }
                else if (CommandType == TEXT("sequence.add_tracks"))
                {
                    ResultJson = FSequenceTracks::AddTracks(Params);
                }
                else if (CommandType == TEXT("sequence.export"))
                {
                    ResultJson = FSequenceExport::Export(Params);
                }
                else if (CommandType.StartsWith(TEXT("sc.")))
                {
                    ResultJson = SourceControlCommands->HandleCommand(CommandType, Params);
                }
                else
                {
                    ResponseJson->SetBoolField(TEXT("ok"), false);
                    ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                    TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
                    ErrorObject->SetStringField(TEXT("code"), TEXT("UNKNOWN_COMMAND"));
                    ErrorObject->SetStringField(TEXT("message"), FString::Printf(TEXT("Unknown command: %s"), *CommandType));
                    ResponseJson->SetObjectField(TEXT("error"), ErrorObject);

                    if (bIsMutation)
                    {
                        MutationPlan.bDryRun = true;
                        AuditJson = FWriteGate::BuildAuditJson(MutationPlan, false);
                    }

                    FString ResultString;
                    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                    FJsonSerializer::Serialize(ResponseJson, Writer, /*bCloseWriter=*/true);
                    Promise.SetValue(ResultString);
                    return;
                }

                // Check if the result contains an error
                bool bSuccess = true;
                FString ErrorMessage;

                FString ErrorCode = TEXT("COMMAND_FAILED");
                if (ResultJson.IsValid() && ResultJson->HasField(TEXT("success")))
                {
                    bSuccess = ResultJson->GetBoolField(TEXT("success"));
                    if (!bSuccess)
                    {
                        if (ResultJson->HasField(TEXT("error")))
                        {
                            ErrorMessage = ResultJson->GetStringField(TEXT("error"));
                        }
                        if (ResultJson->HasField(TEXT("errorCode")))
                        {
                            ErrorCode = ResultJson->GetStringField(TEXT("errorCode"));
                        }
                    }
                }

                if (bSuccess)
                {
                    ResponseJson->SetBoolField(TEXT("ok"), true);
                    ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                    if (ResultJson.IsValid())
                    {
                        ResponseJson->SetObjectField(TEXT("result"), ResultJson);
                    }

                    if (bIsMutation)
                    {
                        MutationPlan.bDryRun = false;
                        AuditJson = FWriteGate::BuildAuditJson(MutationPlan, true);
                    }
                }
                else
                {
                    ResponseJson->SetBoolField(TEXT("ok"), false);
                    ResponseJson->SetStringField(TEXT("status"), TEXT("error"));

                    TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
                    if (ErrorMessage.IsEmpty())
                    {
                        ErrorMessage = TEXT("Command failed");
                    }

                    ErrorObject->SetStringField(TEXT("code"), ErrorCode);
                    ErrorObject->SetStringField(TEXT("message"), ErrorMessage);
                    ResponseJson->SetObjectField(TEXT("error"), ErrorObject);

                    if (bIsMutation)
                    {
                        MutationPlan.bDryRun = false;
                        AuditJson = FWriteGate::BuildAuditJson(MutationPlan, false);
                    }
                }
            }

            if (AuditJson.IsValid())
            {
                ResponseJson->SetObjectField(TEXT("audit"), AuditJson);

                FString AuditString;
                TSharedRef<TJsonWriter<>> AuditWriter = TJsonWriterFactory<>::Create(&AuditString);
                FJsonSerializer::Serialize(AuditJson.ToSharedRef(), AuditWriter, /*bCloseWriter=*/true);
                UE_LOG(LogUnrealMCP, Display, TEXT("[AUDIT] %s"), *AuditString);
            }
        }
        catch (const std::exception& e)
        {
            ResponseJson->SetBoolField(TEXT("ok"), false);
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
            ErrorObject->SetStringField(TEXT("code"), TEXT("EXCEPTION"));
            ErrorObject->SetStringField(TEXT("message"), UTF8_TO_TCHAR(e.what()));
            ResponseJson->SetObjectField(TEXT("error"), ErrorObject);
        }

        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson, Writer, /*bCloseWriter=*/true);
        Promise.SetValue(ResultString);
    });
    
    return Future.Get();
}