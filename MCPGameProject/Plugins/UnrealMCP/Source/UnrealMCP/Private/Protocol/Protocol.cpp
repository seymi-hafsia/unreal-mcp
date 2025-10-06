#include "CoreMinimal.h"
#include "Protocol/Protocol.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "String/LexFromString.h"
#include "String/LexToString.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

namespace UnrealMCP
{
namespace Protocol
{
namespace
{
    constexpr uint32 MaxFrameSize = 4 * 1024 * 1024; // 4 MiB safety limit
    constexpr uint32 LegacyMaxSize = 512 * 1024;      // 512 KiB legacy payload guard

    bool ShouldEmitVerbose()
    {
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        return Settings && Settings->bEnableProtocolVerboseLogs;
    }

    FString GetSocketErrorMessage(const TCHAR* Operation)
    {
        ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        const int32 ErrorCode = SocketSubsystem ? SocketSubsystem->GetLastErrorCode() : 0;
        return FString::Printf(TEXT("%s failed (error %d)"), Operation, ErrorCode);
    }

    bool WaitForSocket(FSocket& Socket, ESocketWaitConditions::Type Condition, double TimeoutSeconds)
    {
        if (TimeoutSeconds < 0.0)
        {
            TimeoutSeconds = 0.0;
        }

        const FTimespan WaitTime = FTimespan::FromSeconds(TimeoutSeconds);
        return Socket.Wait(Condition, WaitTime);
    }

    bool ReadExact(FSocket& Socket, uint8* Buffer, int32 Length, double TimeoutSeconds, FString& OutError, bool& bOutTimeout, TArray<uint8>* OutAccumulated = nullptr)
    {
        int32 TotalRead = 0;
        const double StartTime = FPlatformTime::Seconds();
        bOutTimeout = false;

        while (TotalRead < Length)
        {
            int32 BytesRead = 0;
            if (Socket.Recv(Buffer + TotalRead, Length - TotalRead, BytesRead))
            {
                if (BytesRead == 0)
                {
                    OutError = TEXT("Socket closed by remote host");
                    return false;
                }

                if (OutAccumulated)
                {
                    OutAccumulated->Append(Buffer + TotalRead, BytesRead);
                }

                TotalRead += BytesRead;
                continue;
            }

            const int32 ErrorCode = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
            if (ErrorCode == SE_EWOULDBLOCK)
            {
                const double Elapsed = FPlatformTime::Seconds() - StartTime;
                const double Remaining = TimeoutSeconds - Elapsed;
                if (Remaining <= 0.0)
                {
                    bOutTimeout = true;
                    OutError = TEXT("Read timed out");
                    return false;
                }

                if (!WaitForSocket(Socket, ESocketWaitConditions::WaitForRead, Remaining))
                {
                    bOutTimeout = true;
                    OutError = TEXT("Read timed out");
                    return false;
                }

                continue;
            }

            OutError = GetSocketErrorMessage(TEXT("Recv"));
            return false;
        }

        return true;
    }

    bool WriteAll(FSocket& Socket, const uint8* Data, int32 Length, double TimeoutSeconds, FString& OutError, bool& bOutTimeout)
    {
        int32 TotalWritten = 0;
        const double StartTime = FPlatformTime::Seconds();
        bOutTimeout = false;

        while (TotalWritten < Length)
        {
            int32 BytesSent = 0;
            if (Socket.Send(Data + TotalWritten, Length - TotalWritten, BytesSent))
            {
                if (BytesSent == 0)
                {
                    OutError = TEXT("Socket closed while sending");
                    return false;
                }

                TotalWritten += BytesSent;
                continue;
            }

            const int32 ErrorCode = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
            if (ErrorCode == SE_EWOULDBLOCK)
            {
                const double Elapsed = FPlatformTime::Seconds() - StartTime;
                const double Remaining = TimeoutSeconds - Elapsed;
                if (Remaining <= 0.0)
                {
                    bOutTimeout = true;
                    OutError = TEXT("Write timed out");
                    return false;
                }

                if (!WaitForSocket(Socket, ESocketWaitConditions::WaitForWrite, Remaining))
                {
                    bOutTimeout = true;
                    OutError = TEXT("Write timed out");
                    return false;
                }

                continue;
            }

            OutError = GetSocketErrorMessage(TEXT("Send"));
            return false;
        }

        return true;
    }

    bool TryParseJson(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid())
        {
            return true;
        }

        OutObject.Reset();
        return false;
    }

    bool ReadLegacyPayload(FSocket& Socket, double TimeoutSeconds, TArray<uint8>& Buffer, FString& OutError, TSharedPtr<FJsonObject>& OutObject)
    {
        const double StartTime = FPlatformTime::Seconds();
        while (Buffer.Num() < LegacyMaxSize)
        {
            // Attempt to parse with the bytes we have
            Buffer.Add(0);
            const FString Attempt = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Buffer.GetData())));
            Buffer.Pop(false);
            if (TryParseJson(Attempt, OutObject))
            {
                return true;
            }

            int32 BytesRead = 0;
            uint8 Temp[256];
            if (Socket.Recv(Temp, UE_ARRAY_COUNT(Temp), BytesRead))
            {
                if (BytesRead == 0)
                {
                    OutError = TEXT("Connection closed while reading legacy payload");
                    return false;
                }

                Buffer.Append(Temp, BytesRead);
                continue;
            }

            const int32 ErrorCode = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
            if (ErrorCode == SE_EWOULDBLOCK)
            {
                const double Elapsed = FPlatformTime::Seconds() - StartTime;
                const double Remaining = TimeoutSeconds - Elapsed;
                if (Remaining <= 0.0)
                {
                    OutError = TEXT("Legacy payload read timed out");
                    return false;
                }

                if (!WaitForSocket(Socket, ESocketWaitConditions::WaitForRead, Remaining))
                {
                    OutError = TEXT("Legacy payload read timed out");
                    return false;
                }

                continue;
            }

            OutError = GetSocketErrorMessage(TEXT("Recv"));
            return false;
        }

        OutError = TEXT("Legacy payload exceeded size limit");
        return false;
    }

    double NowSeconds()
    {
        return FPlatformTime::Seconds();
    }

    int64 UnixTimestampMillis()
    {
        const FDateTime Now = FDateTime::UtcNow();
        const int64 Seconds = Now.ToUnixTimestamp();
        return Seconds * 1000 + (Now.GetMillisecond());
    }
}

FString LexToString(EProtocolErrorCode Code)
{
    switch (Code)
    {
    case EProtocolErrorCode::ProtocolVersionMismatch:
        return TEXT("PROTOCOL_VERSION_MISMATCH");
    case EProtocolErrorCode::MalformedFrame:
        return TEXT("MALFORMED_FRAME");
    case EProtocolErrorCode::ReadTimeout:
        return TEXT("READ_TIMEOUT");
    case EProtocolErrorCode::WriteError:
        return TEXT("WRITE_ERROR");
    case EProtocolErrorCode::UnsupportedMessage:
        return TEXT("UNSUPPORTED_MESSAGE");
    case EProtocolErrorCode::InternalError:
        return TEXT("INTERNAL_ERROR");
    default:
        return TEXT("INTERNAL_ERROR");
    }
}

TSharedRef<FJsonObject> MakeErrorResponse(EProtocolErrorCode Code, const FString& Message, const TSharedPtr<FJsonObject>& Details)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("ok"), false);

    TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
    ErrorObject->SetStringField(TEXT("code"), LexToString(Code));
    ErrorObject->SetStringField(TEXT("message"), Message);

    if (Details.IsValid())
    {
        ErrorObject->SetObjectField(TEXT("details"), Details.ToSharedRef());
    }

    Root->SetObjectField(TEXT("error"), ErrorObject);
    return Root;
}

bool WriteFramedJson(FSocket& Socket, const TSharedRef<FJsonObject>& Message, FString& OutError, double TimeoutSeconds)
{
    FString Serialized;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    if (!FJsonSerializer::Serialize(Message, Writer, /*bCloseWriter=*/true))
    {
        OutError = TEXT("Failed to serialize JSON message");
        return false;
    }

    FTCHARToUTF8 Converter(*Serialized);
    const uint8* PayloadData = reinterpret_cast<const uint8*>(Converter.Get());
    const int32 PayloadSize = Converter.Length();

    if (PayloadSize > static_cast<int32>(MaxFrameSize))
    {
        OutError = TEXT("Payload exceeds maximum frame size");
        return false;
    }

    uint8 Header[sizeof(uint32)];
    const uint32 Length = static_cast<uint32>(PayloadSize);
    FMemory::Memcpy(Header, &Length, sizeof(uint32));

    bool bTimedOut = false;
    if (!WriteAll(Socket, Header, sizeof(Header), TimeoutSeconds, OutError, bTimedOut))
    {
        if (bTimedOut)
        {
            OutError = TEXT("Timed out while writing frame header");
        }
        return false;
    }

    if (!WriteAll(Socket, PayloadData, PayloadSize, TimeoutSeconds, OutError, bTimedOut))
    {
        if (bTimedOut)
        {
            OutError = TEXT("Timed out while writing frame payload");
        }
        return false;
    }

    return true;
}

bool WriteLegacyJson(FSocket& Socket, const TSharedRef<FJsonObject>& Message, FString& OutError)
{
    FString Serialized;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    if (!FJsonSerializer::Serialize(Message, Writer, /*bCloseWriter=*/true))
    {
        OutError = TEXT("Failed to serialize legacy JSON message");
        return false;
    }

    FTCHARToUTF8 Converter(*Serialized);
    int32 BytesSent = 0;
    if (!Socket.Send(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length(), BytesSent))
    {
        OutError = GetSocketErrorMessage(TEXT("Send"));
        return false;
    }

    return true;
}

FProtocolReadResult ReadFramedJson(FSocket& Socket, double TimeoutSeconds, bool bAllowLegacyFallback)
{
    FProtocolReadResult Result;

    uint8 Header[sizeof(uint32)];
    FString Error;
    bool bTimedOut = false;
    TArray<uint8> LegacyBuffer;
    if (bAllowLegacyFallback)
    {
        LegacyBuffer.Reserve(256);
    }

    if (!ReadExact(Socket, Header, sizeof(Header), TimeoutSeconds, Error, bTimedOut, bAllowLegacyFallback ? &LegacyBuffer : nullptr))
    {
        Result.Error = Error;
        Result.bTimeout = bTimedOut;
        Result.bSuccess = false;
        Result.bConnectionClosed = !bTimedOut && Error.Contains(TEXT("closed"));
        return Result;
    }

    uint32 PayloadLength = 0;
    FMemory::Memcpy(&PayloadLength, Header, sizeof(uint32));

    if (PayloadLength > MaxFrameSize)
    {
        if (bAllowLegacyFallback)
        {
            Result.bLegacyFallback = true;
            TSharedPtr<FJsonObject> LegacyObject;
            if (ReadLegacyPayload(Socket, TimeoutSeconds, LegacyBuffer, Error, LegacyObject))
            {
                Result.Message = LegacyObject;
                Result.bSuccess = true;
                return Result;
            }

            Result.Error = Error;
            Result.bSuccess = false;
            return Result;
        }

        Result.Error = TEXT("Frame payload length exceeds maximum size");
        Result.bSuccess = false;
        return Result;
    }

    if (PayloadLength == 0)
    {
        Result.Error = TEXT("Received empty frame");
        Result.bSuccess = false;
        return Result;
    }

    TArray<uint8> Payload;
    Payload.SetNumUninitialized(PayloadLength + 1);
    if (!ReadExact(Socket, Payload.GetData(), PayloadLength, TimeoutSeconds, Error, bTimedOut))
    {
        Result.Error = Error;
        Result.bTimeout = bTimedOut;
        Result.bConnectionClosed = !bTimedOut && Error.Contains(TEXT("closed"));
        Result.bSuccess = false;
        return Result;
    }

    Payload[PayloadLength] = 0; // null terminate for safe conversion
    const FString PayloadString = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Payload.GetData())));

    TSharedPtr<FJsonObject> JsonObject;
    if (!TryParseJson(PayloadString, JsonObject))
    {
        Result.Error = TEXT("Failed to parse JSON payload");
        Result.bSuccess = false;
        return Result;
    }

    Result.Message = JsonObject;
    Result.bSuccess = true;
    return Result;
}

FProtocolClient::FProtocolClient(const TSharedPtr<FSocket>& InSocket)
    : Socket(InSocket)
    , LastReceivedTime(NowSeconds())
    , LastSentTime(NowSeconds())
    , bHandshakeCompleted(false)
    , bLegacyDetected(false)
{
}

bool FProtocolClient::PerformHandshake(const FString& EngineVersion, const FString& PluginVersion, const FString& SessionId, FString& OutError, double TimeoutSeconds)
{
    if (!Socket.IsValid())
    {
        OutError = TEXT("Invalid socket for handshake");
        return false;
    }

    FProtocolReadResult ReadResult = ReceiveMessage(TimeoutSeconds, true);
    if (!ReadResult.bSuccess)
    {
        if (ReadResult.bLegacyFallback)
        {
            bLegacyDetected = true;
            TSharedRef<FJsonObject> ErrorDetails = MakeShared<FJsonObject>();
            ErrorDetails->SetNumberField(TEXT("expected"), 1);
            ErrorDetails->SetStringField(TEXT("sessionId"), SessionId);
            TSharedRef<FJsonObject> ErrorResponse = MakeErrorResponse(EProtocolErrorCode::ProtocolVersionMismatch, TEXT("Protocol version 1 required."), ErrorDetails);
            FString LegacyError;
            WriteLegacyJson(*Socket, ErrorResponse, LegacyError);
        }
        OutError = ReadResult.Error.IsEmpty() ? TEXT("Failed to read handshake message") : ReadResult.Error;
        return false;
    }

    if (!ReadResult.Message.IsValid())
    {
        OutError = TEXT("Handshake message missing payload");
        return false;
    }

    const TSharedPtr<FJsonObject> Handshake = ReadResult.Message;
    FString Type;
    if (!Handshake->TryGetStringField(TEXT("type"), Type) || !Type.Equals(TEXT("handshake")))
    {
        OutError = TEXT("Unexpected message type during handshake");
        return false;
    }

    double ProtocolVersionValue = 0.0;
    if (!Handshake->TryGetNumberField(TEXT("protocolVersion"), ProtocolVersionValue))
    {
        OutError = TEXT("Handshake missing protocolVersion");
        return false;
    }

    const int32 ProtocolVersion = static_cast<int32>(ProtocolVersionValue);
    if (ProtocolVersion != 1)
    {
        TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetNumberField(TEXT("got"), ProtocolVersion);
        TSharedRef<FJsonObject> ErrorResponse = MakeErrorResponse(EProtocolErrorCode::ProtocolVersionMismatch, TEXT("Protocol version 1 required."), Details);
        FString WriteError;
        WriteFramedJson(*Socket, ErrorResponse, WriteError);
        OutError = TEXT("Protocol version mismatch");
        return false;
    }

    FString RemoteEngineVersion;
    Handshake->TryGetStringField(TEXT("engineVersion"), RemoteEngineVersion);
    FString RemotePluginVersion;
    Handshake->TryGetStringField(TEXT("pluginVersion"), RemotePluginVersion);
    FString RemoteSessionId;
    Handshake->TryGetStringField(TEXT("sessionId"), RemoteSessionId);

    if (ShouldEmitVerbose())
    {
        UE_LOG(LogUnrealMCP, Verbose, TEXT("[Protocol] Handshake received - Engine: %s, Plugin: %s, Session: %s"), *RemoteEngineVersion, *RemotePluginVersion, *RemoteSessionId);
    }

    TSharedRef<FJsonObject> Ack = MakeShared<FJsonObject>();
    Ack->SetStringField(TEXT("type"), TEXT("handshake/ack"));
    Ack->SetBoolField(TEXT("ok"), true);
    Ack->SetStringField(TEXT("serverVersion"), PluginVersion);

    TArray<TSharedPtr<FJsonValue>> Capabilities;
    Capabilities.Add(MakeShared<FJsonValueString>(TEXT("framed-json")));
    Capabilities.Add(MakeShared<FJsonValueString>(TEXT("heartbeat")));
    Capabilities.Add(MakeShared<FJsonValueString>(TEXT("error-schema")));
    Ack->SetArrayField(TEXT("capabilities"), Capabilities);

    FString WriteError;
    if (!WriteFramedJson(*Socket, Ack, WriteError))
    {
        OutError = WriteError;
        return false;
    }

    LastSentTime = NowSeconds();
    bHandshakeCompleted = true;
    return true;
}

bool FProtocolClient::SendMessage(const TSharedRef<FJsonObject>& Message, FString& OutError, double TimeoutSeconds)
{
    if (!Socket.IsValid())
    {
        OutError = TEXT("Invalid socket");
        return false;
    }

    if (!WriteFramedJson(*Socket, Message, OutError, TimeoutSeconds))
    {
        return false;
    }

    LastSentTime = NowSeconds();
    return true;
}

FProtocolReadResult FProtocolClient::ReceiveMessage(double TimeoutSeconds, bool bAllowLegacyFallback)
{
    FProtocolReadResult Result;
    if (!Socket.IsValid())
    {
        Result.Error = TEXT("Invalid socket");
        return Result;
    }

    Result = ReadFramedJson(*Socket, TimeoutSeconds, bAllowLegacyFallback && !bHandshakeCompleted);
    if (Result.bSuccess)
    {
        LastReceivedTime = NowSeconds();
        if (Result.bLegacyFallback)
        {
            bLegacyDetected = true;
        }
    }

    return Result;
}

bool FProtocolClient::SendHeartbeatMessage(const FString& Type, int64 Timestamp, FString& OutError)
{
    TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
    Message->SetStringField(TEXT("type"), Type);
    Message->SetNumberField(TEXT("ts"), static_cast<double>(Timestamp));
    return SendMessage(Message, OutError, 5.0);
}

bool FProtocolClient::SendPing(FString& OutError)
{
    return SendHeartbeatMessage(TEXT("ping"), UnixTimestampMillis(), OutError);
}

bool FProtocolClient::SendPong(int64 Timestamp, FString& OutError)
{
    return SendHeartbeatMessage(TEXT("pong"), Timestamp, OutError);
}

}
}

