#include "MCPServerRunnable.h"

#include "UnrealMCPBridge.h"
#include "Protocol/Protocol.h"
#include "Permissions/WriteGate.h"
#include "UnrealMCPLog.h"
#include "Observability/JsonLogger.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace
{
        constexpr double ReceivePollSeconds = 1.0;
        const TCHAR* ProtocolPluginVersion = TEXT("1.0.0");

        FString GenerateSessionId()
        {
                return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
        }
}

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket, const FMCPServerConfig& InConfig)
        : Bridge(InBridge)
        , ListenerSocket(InListenerSocket)
        , bRunning(true)
        , Config(InConfig)
{
        UE_LOG(LogUnrealMCP, Display, TEXT("MCPServerRunnable: Created server runnable"));
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
        UE_LOG(LogUnrealMCP, Display, TEXT("MCPServerRunnable: Server thread starting..."));

        while (bRunning)
        {
                bool bPending = false;
                if (ListenerSocket->HasPendingConnection(bPending) && bPending)
                {
                        ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")));
                        if (ClientSocket.IsValid())
                        {
                                UE_LOG(LogUnrealMCP, Display, TEXT("MCPServerRunnable: Client connection accepted"));

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

        UE_LOG(LogUnrealMCP, Display, TEXT("MCPServerRunnable: Server thread stopping"));
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
        const double HandshakeTimeoutSeconds = FMath::Max(1.0, Config.HandshakeTimeoutSeconds);
        const double IdleTimeoutSeconds = FMath::Max(1.0, Config.ReadTimeoutSeconds);
        const double PingIntervalSeconds = FMath::Max(0.1, Config.HeartbeatIntervalSeconds);

        if (!ProtocolClient.PerformHandshake(EngineVersionString, ProtocolPluginVersion, SessionId, HandshakeError, HandshakeTimeoutSeconds))
        {
                UE_LOG(LogUnrealMCP, Warning, TEXT("[Protocol] Handshake failed: %s"), *HandshakeError);
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
                                        UE_LOG(LogUnrealMCP, Warning, TEXT("[Protocol] Failed to send ping: %s"), *PingError);
                                        break;
                                }

                                LastPingTime = FPlatformTime::Seconds();
                        }
                }
                else if (!ReadResult.Error.IsEmpty())
                {
                        UE_LOG(LogUnrealMCP, Warning, TEXT("[Protocol] Read error: %s"), *ReadResult.Error);
                        break;
                }

                if ((FPlatformTime::Seconds() - ProtocolClient.GetLastReceivedTime()) > IdleTimeoutSeconds)
                {
                        UE_LOG(LogUnrealMCP, Warning, TEXT("[Protocol] Inactivity timeout, closing connection"));
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
                        UE_LOG(LogUnrealMCP, Warning, TEXT("[Protocol] Failed to send pong: %s"), *PongError);
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
                UE_LOG(LogUnrealMCP, Warning, TEXT("[Protocol] Unexpected handshake message after initialization"));
                return true;
        }

        if (MessageType.Equals(TEXT("capabilities"), ESearchCase::IgnoreCase))
        {
                bool bOk = false;
                Message->TryGetBoolField(TEXT("ok"), bOk);
                if (bOk && Message->HasTypedField<EJson::Object>(TEXT("enforcement")))
                {
                        const TSharedPtr<FJsonObject> Enforcement = Message->GetObjectField(TEXT("enforcement"));

                        bool bAllowWrite = false;
                        Enforcement->TryGetBoolField(TEXT("allowWrite"), bAllowWrite);

                        bool bDryRun = true;
                        Enforcement->TryGetBoolField(TEXT("dryRun"), bDryRun);

                        TArray<FString> AllowedPaths;
                        if (Enforcement->HasTypedField<EJson::Array>(TEXT("allowedPaths")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>& PathValues = Enforcement->GetArrayField(TEXT("allowedPaths"));
                                for (const TSharedPtr<FJsonValue>& Value : PathValues)
                                {
                                        if (Value.IsValid() && Value->Type == EJson::String)
                                        {
                                                AllowedPaths.Add(Value->AsString());
                                        }
                                }
                        }

                        TArray<FString> AllowedTools;
                        if (Enforcement->HasTypedField<EJson::Array>(TEXT("allowedTools")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>& ToolValues = Enforcement->GetArrayField(TEXT("allowedTools"));
                                for (const TSharedPtr<FJsonValue>& Value : ToolValues)
                                {
                                        if (Value.IsValid() && Value->Type == EJson::String)
                                        {
                                                AllowedTools.Add(Value->AsString());
                                        }
                                }
                        }

                        TArray<FString> DeniedTools;
                        if (Enforcement->HasTypedField<EJson::Array>(TEXT("deniedTools")))
                        {
                                const TArray<TSharedPtr<FJsonValue>>& ToolValues = Enforcement->GetArrayField(TEXT("deniedTools"));
                                for (const TSharedPtr<FJsonValue>& Value : ToolValues)
                                {
                                        if (Value.IsValid() && Value->Type == EJson::String)
                                        {
                                                DeniedTools.Add(Value->AsString());
                                        }
                                }
                        }

                        FWriteGate::UpdateRemoteEnforcement(bAllowWrite, bDryRun, AllowedPaths, AllowedTools, DeniedTools);

                        UE_LOG(LogUnrealMCP, Display, TEXT("[Protocol] Remote enforcement updated (allowWrite=%s, dryRun=%s, paths=%d, allowedTools=%d, deniedTools=%d)"),
                                bAllowWrite ? TEXT("true") : TEXT("false"),
                                bDryRun ? TEXT("true") : TEXT("false"),
                                AllowedPaths.Num(),
                                AllowedTools.Num(),
                                DeniedTools.Num());
                }

                return true;
        }

        FString RequestId;
        if (!Message->TryGetStringField(TEXT("requestId"), RequestId))
        {
                if (Message->HasTypedField<EJson::Object>(TEXT("meta")))
                {
                        const TSharedPtr<FJsonObject> MetaObject = Message->GetObjectField(TEXT("meta"));
                        if (MetaObject.IsValid())
                        {
                                MetaObject->TryGetStringField(TEXT("requestId"), RequestId);
                        }
                }
        }

        if (RequestId.IsEmpty())
        {
                RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
        }

        const FDateTime StartUtc = FDateTime::UtcNow();
        const double StartSeconds = FPlatformTime::Seconds();
        const double StartTsMs = static_cast<double>(StartUtc.ToUnixTimestamp()) * 1000.0 + StartUtc.GetMillisecond();

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        if (Message->HasField(TEXT("params")) && Message->HasTypedField<EJson::Object>(TEXT("params")))
        {
                Params = Message->GetObjectField(TEXT("params"));
        }

        const FString ResponseString = Bridge->ExecuteCommand(MessageType, Params, RequestId);
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

        const double DurationMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
        TSharedPtr<FJsonObject> Meta = ResponseObject->HasTypedField<EJson::Object>(TEXT("meta"))
                ? ResponseObject->GetObjectField(TEXT("meta"))
                : MakeShared<FJsonObject>();
        if (Meta.IsValid())
        {
                Meta->SetStringField(TEXT("requestId"), RequestId);
                Meta->SetNumberField(TEXT("ts"), StartTsMs);
                Meta->SetNumberField(TEXT("durMs"), DurationMs);
                ResponseObject->SetObjectField(TEXT("meta"), Meta);
        }

        const bool bOk = ResponseObject->HasField(TEXT("ok")) ? ResponseObject->GetBoolField(TEXT("ok")) : false;
        FString ErrorCode;
        if (ResponseObject->HasField(TEXT("error")))
        {
                const TSharedPtr<FJsonObject> Error = ResponseObject->GetObjectField(TEXT("error"));
                if (Error.IsValid())
                {
                        Error->TryGetStringField(TEXT("code"), ErrorCode);
                }
        }

        TSharedPtr<FJsonObject> MetricFields = MakeShared<FJsonObject>();
        MetricFields->SetStringField(TEXT("tool"), MessageType);
        MetricFields->SetBoolField(TEXT("ok"), bOk);
        MetricFields->SetNumberField(TEXT("durMs"), DurationMs);
        if (!ErrorCode.IsEmpty())
        {
                MetricFields->SetStringField(TEXT("errorCode"), ErrorCode);
        }
        FJsonLogger::Metric(TEXT("tool_duration_ms"), MetricFields);

        TSharedPtr<FJsonObject> CounterFields = MakeShared<FJsonObject>();
        CounterFields->SetStringField(TEXT("tool"), MessageType);
        CounterFields->SetBoolField(TEXT("ok"), bOk);
        if (!ErrorCode.IsEmpty())
        {
                CounterFields->SetStringField(TEXT("errorCode"), ErrorCode);
        }
        FJsonLogger::Metric(TEXT("tool_calls_total"), CounterFields);

        TSharedPtr<FJsonObject> EventFields = MakeShared<FJsonObject>();
        EventFields->SetBoolField(TEXT("ok"), bOk);
        EventFields->SetNumberField(TEXT("durMs"), DurationMs);
        if (!ErrorCode.IsEmpty())
        {
                EventFields->SetStringField(TEXT("code"), ErrorCode);
        }
        FLogEvent Event;
        Event.Level = bOk ? TEXT("info") : TEXT("error");
        Event.Category = FString::Printf(TEXT("tool.%s"), *MessageType);
        Event.RequestId = RequestId;
        Event.Message = FString::Printf(TEXT("Command %s completed"), *MessageType);
        Event.Fields = EventFields;
        Event.TsUnixMs = StartTsMs;
        FJsonLogger::Log(Event);

        FString SendError;
        if (!ProtocolClient.SendMessage(ResponseObject.ToSharedRef(), SendError))
        {
                UE_LOG(LogUnrealMCP, Warning, TEXT("[Protocol] Failed to send response: %s"), *SendError);
                return false;
        }

        return true;
}
