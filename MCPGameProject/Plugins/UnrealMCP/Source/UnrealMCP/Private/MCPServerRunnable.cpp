#include "MCPServerRunnable.h"
#include "UnrealMCPBridge.h"
#include "UnrealMCPModule.h"
#include "MCPJsonHelpers.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h"

// Buffer size for receiving data (use internal linkage and unique name to avoid symbol clashes)
namespace { constexpr int32 MCPReceiveBufferSize = 65536; } // Increased from 8192 to 64KB

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge)
    , ListenerSocket(InListenerSocket)
    , bRunning(true)
{
    UE_LOG(LogUnrealMCP, Log, TEXT("MCP Server runnable created"));
}

FMCPServerRunnable::~FMCPServerRunnable()
{
    // Note: We don't delete the sockets here as they're owned by the bridge
}

bool FMCPServerRunnable::Init()
{
    return true;
}

uint32 FMCPServerRunnable::Run()
{
    UE_LOG(LogUnrealMCP, Log, TEXT("MCP Server thread started"));

    while (bRunning)
    {
        bool bPending = false;
        if (ListenerSocket->HasPendingConnection(bPending) && bPending)
        {
            ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")));
            if (ClientSocket.IsValid())
            {
                UE_LOG(LogUnrealMCP, Log, TEXT("Client connected"));

                // Configure socket for optimal performance
                ClientSocket->SetNoDelay(true);
                int32 SocketBufferSize = 65536;  // 64KB buffer
                ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
                ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);
                ClientSocket->SetNonBlocking(false);

                // Handle client connection
                HandleClientConnection(ClientSocket);

                UE_LOG(LogUnrealMCP, Log, TEXT("Client disconnected"));
            }
            else
            {
                UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to accept client connection"));
            }
        }

        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.1f);
    }

    UE_LOG(LogUnrealMCP, Log, TEXT("MCP Server thread stopped"));
    return 0;
}

void FMCPServerRunnable::Stop()
{
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

void FMCPServerRunnable::HandleClientConnection(TSharedPtr<FSocket> InClientSocket)
{
    if (!InClientSocket.IsValid())
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Invalid client socket"));
        return;
    }

    uint8 Buffer[MCPReceiveBufferSize + 1];
    FString MessageBuffer;

    while (bRunning && InClientSocket.IsValid() && InClientSocket->GetConnectionState() == SCS_Connected)
    {
        int32 BytesRead = 0;
        bool bReadSuccess = InClientSocket->Recv(Buffer, MCPReceiveBufferSize, BytesRead, ESocketReceiveFlags::None);

        if (BytesRead > 0)
        {
            // Convert received data to string
            Buffer[BytesRead] = 0; // Null terminate
            FString ReceivedData = UTF8_TO_TCHAR(Buffer);
            MessageBuffer.Append(ReceivedData);

            // Process complete messages (messages are terminated with newline)
            if (MessageBuffer.Contains(TEXT("\n")))
            {
                TArray<FString> Messages;
                MessageBuffer.ParseIntoArray(Messages, TEXT("\n"), true);

                // Process all complete messages
                for (int32 i = 0; i < Messages.Num() - 1; ++i)
                {
                    ProcessMessage(InClientSocket, Messages[i]);
                }

                // Keep any incomplete message in the buffer
                MessageBuffer = Messages.Last();
            }
        }
        else if (!bReadSuccess)
        {
            int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
            if (LastError != SE_EWOULDBLOCK)
            {
                UE_LOG(LogUnrealMCP, Warning, TEXT("Connection error: %d"), LastError);
                break;
            }
        }

        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.01f);
    }
}

void FMCPServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message)
{
    // Parse message as JSON
    TSharedPtr<FJsonObject> JsonMessage;
    FString ErrorMessage;

    if (!FMCPJsonHelpers::ParseJson(Message, JsonMessage, ErrorMessage))
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("JSON parse error: %s"), *ErrorMessage);

        // Send error response
        TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonHelpers::CreateErrorResponse(ErrorMessage);
        FString Response = FMCPJsonHelpers::SerializeJson(ErrorResponse) + TEXT("\n");
        int32 BytesSent = 0;
        Client->Send((uint8*)TCHAR_TO_UTF8(*Response), Response.Len(), BytesSent);
        return;
    }

    // Validate required fields
    TArray<FString> RequiredFields = { TEXT("command") };
    if (!FMCPJsonHelpers::ValidateRequiredFields(JsonMessage, RequiredFields, ErrorMessage))
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("Validation error: %s"), *ErrorMessage);

        // Send error response
        TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonHelpers::CreateErrorResponse(ErrorMessage);
        FString Response = FMCPJsonHelpers::SerializeJson(ErrorResponse) + TEXT("\n");
        int32 BytesSent = 0;
        Client->Send((uint8*)TCHAR_TO_UTF8(*Response), Response.Len(), BytesSent);
        return;
    }

    // Extract command type and parameters
    FString CommandType = FMCPJsonHelpers::GetStringField(JsonMessage, TEXT("command"));
    TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());

    // Parameters are optional
    if (JsonMessage->HasField(TEXT("params")))
    {
        TSharedPtr<FJsonValue> ParamsValue = JsonMessage->TryGetField(TEXT("params"));
        if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
        {
            Params = ParamsValue->AsObject();
        }
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("Executing command: %s"), *CommandType);

    // Execute command with error handling
    FString Response;
    try
    {
        Response = Bridge->ExecuteCommand(CommandType, Params);
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Exception during command execution: %s"), UTF8_TO_TCHAR(e.what()));
        TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonHelpers::CreateErrorResponse(
            FString::Printf(TEXT("Internal error: %s"), UTF8_TO_TCHAR(e.what()))
        );
        Response = FMCPJsonHelpers::SerializeJson(ErrorResponse);
    }

    // Send response with newline terminator
    Response += TEXT("\n");
    int32 BytesSent = 0;

    if (!Client->Send((uint8*)TCHAR_TO_UTF8(*Response), Response.Len(), BytesSent))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Failed to send response for command: %s"), *CommandType);
    }
} 