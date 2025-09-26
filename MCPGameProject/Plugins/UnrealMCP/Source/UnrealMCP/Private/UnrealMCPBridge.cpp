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
#include "Permissions/WriteGate.h"
#include "Transactions/TransactionManager.h"

#include "Misc/ScopeExit.h"

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

UUnrealMCPBridge::UUnrealMCPBridge()
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();

    FWriteGate::UpdateRemoteEnforcement(false, true, TArray<FString>());
}

UUnrealMCPBridge::~UUnrealMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintNodeCommands.Reset();
    ProjectCommands.Reset();
    UMGCommands.Reset();
}

// Initialize subsystem
void UUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Initializing"));
    
    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // Start the server automatically
    StartServer();
}

// Clean up resources when subsystem is destroyed
void UUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Shutting down"));
    StopServer();
}

// Start the MCP server
void UUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to bind listener socket to %s:%d"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerThread = FRunnableThread::Create(
        new FMCPServerRunnable(this, ListenerSocket),
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
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

    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

// Execute a command received from a client
FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Executing command: %s"), *CommandType);

    // Create a promise to wait for the result
    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();

    // Queue execution on Game Thread
    AsyncTask(ENamedThreads::GameThread, [this, CommandType, Params, Promise = MoveTemp(Promise)]() mutable
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();

        try
        {
            TSharedPtr<FJsonObject> ResultJson;
            const bool bIsMutation = FWriteGate::IsMutationCommand(CommandType);
            const FString TargetPath = FWriteGate::ResolvePathForCommand(CommandType, Params);
            FMutationPlan MutationPlan;
            bool bSkipExecution = false;
            TSharedPtr<FJsonObject> AuditJson;

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
                if (bIsMutation)
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
                        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
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
                    FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                    Promise.SetValue(ResultString);
                    return;
                }

                // Check if the result contains an error
                bool bSuccess = true;
                FString ErrorMessage;

                if (ResultJson.IsValid() && ResultJson->HasField(TEXT("success")))
                {
                    bSuccess = ResultJson->GetBoolField(TEXT("success"));
                    if (!bSuccess && ResultJson->HasField(TEXT("error")))
                    {
                        ErrorMessage = ResultJson->GetStringField(TEXT("error"));
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
                    ErrorObject->SetStringField(TEXT("code"), TEXT("COMMAND_FAILED"));
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
                FJsonSerializer::Serialize(AuditJson.ToSharedRef(), AuditWriter);
                UE_LOG(LogTemp, Display, TEXT("[AUDIT] %s"), *AuditString);
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
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        Promise.SetValue(ResultString);
    });
    
    return Future.Get();
}