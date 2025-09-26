#include "MCPServerRunnable.h"

#include "UnrealMCPBridge.h"
#include "Protocol/Protocol.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Guid.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace
{
        constexpr double HandshakeTimeoutSeconds = 10.0;
        constexpr double PingIntervalSeconds = 15.0;
        constexpr double IdleTimeoutSeconds = 60.0;
        constexpr double ReceivePollSeconds = 1.0;
        const TCHAR* ProtocolPluginVersion = TEXT("1.0.0");

        FString GenerateSessionId()
        {
                return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
        }
}

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
        : Bridge(InBridge)
        , ListenerSocket(InListenerSocket)
        , bRunning(true)
{
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Created server runnable"));
}

FMCPServerRunnable::~FMCPServerRunnable()
{
}

bool FMCPServerRunnable::Init()
{
        return true;
}

uint32 FMCPServerRunnable::Run()
{
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread starting..."));

        while (bRunning)
        {
                bool bPending = false;
                if (ListenerSocket->HasPendingConnection(bPending) && bPending)
                {
                        ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")));
                        if (ClientSocket.IsValid())
                        {
                                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection accepted"));

                                ClientSocket->SetNoDelay(true);
                                ClientSocket->SetNonBlocking(false);
                                int32 SocketBufferSize = 65536;
                                ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
                                ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);

                                RunConnection(ClientSocket);
                        }
                }

                FPlatformProcess::Sleep(0.05f);
        }

        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread stopping"));
        return 0;
}

void FMCPServerRunnable::Stop()
{
        bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

void FMCPServerRunnable::RunConnection(const TSharedPtr<FSocket>& InClientSocket)
{
        using namespace UnrealMCP::Protocol;

        if (!InClientSocket.IsValid())
        {
                return;
        }

        const FString EngineVersionString = FEngineVersion::Current().ToString();
        const FString SessionId = GenerateSessionId();

        FProtocolClient ProtocolClient(InClientSocket);
        FString HandshakeError;
        if (!ProtocolClient.PerformHandshake(EngineVersionString, ProtocolPluginVersion, SessionId, HandshakeError, HandshakeTimeoutSeconds))
        {
                UE_LOG(LogTemp, Warning, TEXT("[Protocol] Handshake failed: %s"), *HandshakeError);
                InClientSocket->Close();
                return;
        }

        double LastPingTime = FPlatformTime::Seconds();

        while (bRunning && InClientSocket->GetConnectionState() == SCS_Connected)
        {
                const double Now = FPlatformTime::Seconds();
                const double SinceLastReceive = Now - ProtocolClient.GetLastReceivedTime();
                const double ReadTimeout = FMath::Min(ReceivePollSeconds, FMath::Max(0.0, IdleTimeoutSeconds - SinceLastReceive));

                FProtocolReadResult ReadResult = ProtocolClient.ReceiveMessage(ReadTimeout, false);
                if (ReadResult.bSuccess && ReadResult.Message.IsValid())
                {
                        if (!HandleProtocolMessage(ProtocolClient, ReadResult.Message.ToSharedRef()))
                        {
                                break;
                        }
                }
                else if (ReadResult.bTimeout)
                {
                        if ((Now - LastPingTime) >= PingIntervalSeconds)
                        {
                                FString PingError;
                                if (!ProtocolClient.SendPing(PingError))
                                {
                                        UE_LOG(LogTemp, Warning, TEXT("[Protocol] Failed to send ping: %s"), *PingError);
                                        break;
                                }

                                LastPingTime = FPlatformTime::Seconds();
                        }
                }
                else if (!ReadResult.Error.IsEmpty())
                {
                        UE_LOG(LogTemp, Warning, TEXT("[Protocol] Read error: %s"), *ReadResult.Error);
                        break;
                }

                if ((FPlatformTime::Seconds() - ProtocolClient.GetLastReceivedTime()) > IdleTimeoutSeconds)
                {
                        UE_LOG(LogTemp, Warning, TEXT("[Protocol] Inactivity timeout, closing connection"));
                        break;
                }
        }

        InClientSocket->Close();
        if (ClientSocket == InClientSocket)
        {
                ClientSocket.Reset();
        }
}

bool FMCPServerRunnable::HandleProtocolMessage(UnrealMCP::Protocol::FProtocolClient& ProtocolClient, const TSharedRef<FJsonObject>& Message)
{
        using namespace UnrealMCP::Protocol;

        FString MessageType;
        if (!Message->TryGetStringField(TEXT("type"), MessageType))
        {
                TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetStringField(TEXT("reason"), TEXT("Missing 'type' field"));
                TSharedRef<FJsonObject> ErrorResponse = MakeErrorResponse(EProtocolErrorCode::MalformedFrame, TEXT("Message missing 'type' field."), Details);

                FString WriteError;
                ProtocolClient.SendMessage(ErrorResponse, WriteError);
                return true;
        }

        if (MessageType.Equals(TEXT("ping"), ESearchCase::IgnoreCase))
        {
                double TimestampValue = 0.0;
                Message->TryGetNumberField(TEXT("ts"), TimestampValue);
                FString PongError;
                if (!ProtocolClient.SendPong(static_cast<int64>(TimestampValue), PongError))
                {
                        UE_LOG(LogTemp, Warning, TEXT("[Protocol] Failed to send pong: %s"), *PongError);
                        return false;
                }
                return true;
        }

        if (MessageType.Equals(TEXT("pong"), ESearchCase::IgnoreCase))
        {
                return true;
        }

        if (MessageType.Equals(TEXT("handshake"), ESearchCase::IgnoreCase))
        {
                UE_LOG(LogTemp, Warning, TEXT("[Protocol] Unexpected handshake message after initialization"));
                return true;
        }

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        if (Message->HasField(TEXT("params")) && Message->HasTypedField<EJson::Object>(TEXT("params")))
        {
                Params = Message->GetObjectField(TEXT("params"));
        }

        const FString ResponseString = Bridge->ExecuteCommand(MessageType, Params);
        TSharedPtr<FJsonObject> ResponseObject;
        {
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
                if (!FJsonSerializer::Deserialize(Reader, ResponseObject) || !ResponseObject.IsValid())
                {
                        TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
                        Details->SetStringField(TEXT("raw"), ResponseString);
                        TSharedRef<FJsonObject> ErrorResponse = MakeErrorResponse(EProtocolErrorCode::InternalError, TEXT("Failed to parse command response."), Details);

                        FString WriteError;
                        ProtocolClient.SendMessage(ErrorResponse, WriteError);
                        return true;
                }
        }

        FString SendError;
        if (!ProtocolClient.SendMessage(ResponseObject.ToSharedRef(), SendError))
        {
                UE_LOG(LogTemp, Warning, TEXT("[Protocol] Failed to send response: %s"), *SendError);
                return false;
        }

        return true;
}
