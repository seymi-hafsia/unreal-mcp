#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FSocket;
class FJsonObject;

namespace UnrealMCP
{
namespace Protocol
{
    enum class EProtocolErrorCode
    {
        ProtocolVersionMismatch,
        MalformedFrame,
        ReadTimeout,
        WriteError,
        UnsupportedMessage,
        InternalError
    };

    FString LexToString(EProtocolErrorCode Code);

    TSharedRef<FJsonObject> MakeErrorResponse(EProtocolErrorCode Code, const FString& Message, const TSharedPtr<FJsonObject>& Details = nullptr);

    struct FProtocolReadResult
    {
        TSharedPtr<FJsonObject> Message;
        bool bSuccess = false;
        bool bTimeout = false;
        bool bConnectionClosed = false;
        bool bLegacyFallback = false;
        FString Error;
    };

    bool WriteFramedJson(FSocket& Socket, const TSharedRef<FJsonObject>& Message, FString& OutError, double TimeoutSeconds = 10.0);
    bool WriteLegacyJson(FSocket& Socket, const TSharedRef<FJsonObject>& Message, FString& OutError);

    FProtocolReadResult ReadFramedJson(FSocket& Socket, double TimeoutSeconds, bool bAllowLegacyFallback);

    class UNREALMCPEDITOR_API FProtocolClient
    {
    public:
        explicit FProtocolClient(const TSharedPtr<FSocket>& InSocket);

        bool IsValid() const { return Socket.IsValid(); }

        bool PerformHandshake(const FString& EngineVersion, const FString& PluginVersion, const FString& SessionId, FString& OutError, double TimeoutSeconds = 10.0);

        bool SendMessage(const TSharedPtr<FJsonObject>& Message, FString& OutError, double TimeoutSeconds = 10.0);
        bool SendMessage(const TSharedRef<FJsonObject>& Message, FString& OutError, double TimeoutSeconds = 10.0)
        {
            return SendMessage(TSharedPtr<FJsonObject>(Message), OutError, TimeoutSeconds);
        }
        FProtocolReadResult ReceiveMessage(double TimeoutSeconds, bool bAllowLegacyFallback = false);

        bool SendPing(FString& OutError);
        bool SendPong(int64 Timestamp, FString& OutError);

        double GetLastReceivedTime() const { return LastReceivedTime; }
        double GetLastSentTime() const { return LastSentTime; }

    private:
        bool SendHeartbeatMessage(const FString& Type, int64 Timestamp, FString& OutError);

        TSharedPtr<FSocket> Socket;
        double LastReceivedTime;
        double LastSentTime;
        bool bHandshakeCompleted;
        bool bLegacyDetected;
    };
}
}

