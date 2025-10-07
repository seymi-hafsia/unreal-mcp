#include "Settings/UnrealMCPDiagnostics.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Protocol/Protocol.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "UnrealMCPSettings.h"
#include "String/LexToString.h"

namespace
{
        constexpr const TCHAR* EventsLogName = TEXT("UnrealMCP_events.jsonl");
        constexpr const TCHAR* MetricsLogName = TEXT("UnrealMCP_metrics.jsonl");

        FString FormatEndpointMessage(const UUnrealMCPSettings& Settings)
        {
                FString Host = Settings.ServerHost;
                Host.TrimStartAndEndInline();
                return FString::Printf(TEXT("%s:%d"), *Host, Settings.ServerPort);
        }

        TSharedPtr<FSocket> MakeManagedSocket(ISocketSubsystem& Subsystem)
        {
                FSocket* RawSocket = Subsystem.CreateSocket(NAME_Stream, TEXT("UnrealMCPDiagnostics"), false);
                if (!RawSocket)
                {
                        return nullptr;
                }

                return MakeShareable(RawSocket, [&Subsystem](FSocket* Socket)
                {
                        if (Socket)
                        {
                                Subsystem.DestroySocket(Socket);
                        }
                });
        }

        FString DescribeLastError(ISocketSubsystem& Subsystem)
        {
                const ESocketErrors Error = Subsystem.GetLastErrorCode();
                return FString::Printf(TEXT("%s (%d)"), *LexToString(Error), static_cast<int32>(Error));
        }
}

bool FUnrealMCPDiagnostics::TestConnection(FText& OutMessage)
{
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        if (!Settings)
        {
                OutMessage = FText::FromString(TEXT("Unable to load Unreal MCP settings."));
                return false;
        }

        TSharedPtr<FSocket> Socket;
        FString Error;
        if (!ConnectToServer(*Settings, Socket, Error))
        {
                OutMessage = FText::FromString(FString::Printf(TEXT("Failed to connect to %s: %s"), *FormatEndpointMessage(*Settings), *Error));
                return false;
        }

        if (!PerformHandshake(*Socket, *Settings, Error))
        {
                OutMessage = FText::FromString(FString::Printf(TEXT("Handshake failed: %s"), *Error));
                return false;
        }

        OutMessage = FText::FromString(FString::Printf(TEXT("Handshake succeeded with %s."), *FormatEndpointMessage(*Settings)));
        return true;
}

bool FUnrealMCPDiagnostics::SendPing(FText& OutMessage)
{
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        if (!Settings)
        {
                OutMessage = FText::FromString(TEXT("Unable to load Unreal MCP settings."));
                return false;
        }

        TSharedPtr<FSocket> Socket;
        FString Error;
        if (!ConnectToServer(*Settings, Socket, Error))
        {
                OutMessage = FText::FromString(FString::Printf(TEXT("Failed to connect to %s: %s"), *FormatEndpointMessage(*Settings), *Error));
                return false;
        }

        if (!PerformHandshake(*Socket, *Settings, Error))
        {
                OutMessage = FText::FromString(FString::Printf(TEXT("Handshake failed: %s"), *Error));
                return false;
        }

        const double StartTime = FPlatformTime::Seconds();
        TSharedPtr<FJsonObject> Response;
        if (!SendCommand(*Socket, TEXT("ping"), nullptr, *Settings, Response, Error, Settings->ReadTimeoutSec))
        {
                OutMessage = FText::FromString(FString::Printf(TEXT("Ping failed: %s"), *Error));
                return false;
        }

        bool bOk = false;
        if (Response.IsValid() && Response->TryGetBoolField(TEXT("ok"), bOk) && bOk)
        {
                const double RttMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
                OutMessage = FText::FromString(FString::Printf(TEXT("Ping succeeded (%.2f ms)."), RttMs));
                return true;
        }

        OutMessage = FText::FromString(TEXT("Ping returned an error response."));
        return false;
}

bool FUnrealMCPDiagnostics::OpenLogsFolder(FText& OutMessage)
{
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        if (!Settings)
        {
                OutMessage = FText::FromString(TEXT("Unable to load Unreal MCP settings."));
                return false;
        }

        const FString LogsPath = Settings->GetEffectiveLogsDirectory();
        if (LogsPath.IsEmpty())
        {
                OutMessage = FText::FromString(TEXT("Logs directory is not configured."));
                return false;
        }

        if (!FPaths::DirectoryExists(LogsPath))
        {
                IFileManager::Get().MakeDirectory(*LogsPath, true);
        }

        FPlatformProcess::ExploreFolder(*LogsPath);

        OutMessage = FText::FromString(FString::Printf(TEXT("Opened logs folder: %s"), *LogsPath));
        return true;
}

bool FUnrealMCPDiagnostics::OpenEventsLog(FText& OutMessage)
{
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        if (!Settings)
        {
                OutMessage = FText::FromString(TEXT("Unable to load Unreal MCP settings."));
                return false;
        }

        const FString FilePath = GetEventsLogPath(*Settings);
        if (FilePath.IsEmpty())
        {
                OutMessage = FText::FromString(TEXT("Events log path is not available."));
                return false;
        }

        if (!FPaths::FileExists(FilePath))
        {
                FFileHelper::SaveStringToFile(TEXT(""), *FilePath);
        }

        if (!FPlatformProcess::LaunchFileInDefaultExternalApplication(*FilePath))
        {
                OutMessage = FText::FromString(FString::Printf(TEXT("Failed to open events log: %s"), *FilePath));
                return false;
        }

        OutMessage = FText::FromString(FString::Printf(TEXT("Opened events log: %s"), *FilePath));
        return true;
}

bool FUnrealMCPDiagnostics::OpenMetricsLog(FText& OutMessage)
{
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        if (!Settings)
        {
                OutMessage = FText::FromString(TEXT("Unable to load Unreal MCP settings."));
                return false;
        }

        const FString FilePath = GetMetricsLogPath(*Settings);
        if (FilePath.IsEmpty())
        {
                OutMessage = FText::FromString(TEXT("Metrics log path is not available."));
                return false;
        }

        if (!FPaths::FileExists(FilePath))
        {
                FFileHelper::SaveStringToFile(TEXT(""), *FilePath);
        }

        if (!FPlatformProcess::LaunchFileInDefaultExternalApplication(*FilePath))
        {
                OutMessage = FText::FromString(FString::Printf(TEXT("Failed to open metrics log: %s"), *FilePath));
                return false;
        }

        OutMessage = FText::FromString(FString::Printf(TEXT("Opened metrics log: %s"), *FilePath));
        return true;
}

bool FUnrealMCPDiagnostics::TailLogs(FText& OutMessage)
{
        const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
        if (!Settings)
        {
                OutMessage = FText::FromString(TEXT("Unable to load Unreal MCP settings."));
                return false;
        }

        const FString FilePath = GetEventsLogPath(*Settings);
        if (FilePath.IsEmpty())
        {
                OutMessage = FText::FromString(TEXT("Events log path is not available."));
                return false;
        }

        if (!FPaths::FileExists(FilePath))
        {
                FFileHelper::SaveStringToFile(TEXT(""), *FilePath);
        }

#if PLATFORM_WINDOWS
        const FString Command = TEXT("powershell.exe");
        const FString Escaped = FilePath.Replace(TEXT("\""), TEXT("\\\""));
        const FString Args = FString::Printf(TEXT("-NoExit -Command \"Get-Content -Path \"\"%s\"\" -Wait\""), *Escaped);
#elif PLATFORM_LINUX || PLATFORM_MAC
        const FString Command = TEXT("/usr/bin/env");
        const FString Escaped = FilePath.Replace(TEXT("\""), TEXT("\\\""));
        const FString Args = FString::Printf(TEXT("tail -f \"%s\""), *Escaped);
#else
        OutMessage = FText::FromString(TEXT("Tailing logs is not supported on this platform."));
        return false;
#endif

        FProcHandle Handle = FPlatformProcess::CreateProc(*Command, *Args, true, false, false, nullptr, 0, nullptr, nullptr);
        if (!Handle.IsValid())
        {
                OutMessage = FText::FromString(TEXT("Unable to start tail process. Ensure tail/powershell is available."));
                return false;
        }

        OutMessage = FText::FromString(TEXT("Started tail process. Close the spawned console to stop."));
        return true;
}

bool FUnrealMCPDiagnostics::ConnectToServer(const UUnrealMCPSettings& Settings, TSharedPtr<FSocket>& OutSocket, FString& OutError)
{
        ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!SocketSubsystem)
        {
                OutError = TEXT("Socket subsystem unavailable");
                return false;
        }

        TSharedPtr<FSocket> Socket = MakeManagedSocket(*SocketSubsystem);
        if (!Socket.IsValid())
        {
                OutError = TEXT("Failed to create diagnostics socket");
                return false;
        }

        const FString ResolvedHost = ResolveHostForConnection(Settings.ServerHost);
        TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
        bool bIsValid = false;
        Address->SetIp(*ResolvedHost, bIsValid);
        if (!bIsValid)
        {
                const FAddressInfoResult AddressInfo = SocketSubsystem->GetAddressInfo(*ResolvedHost, nullptr, EAddressInfoFlags::Default, NAME_None);
                if (AddressInfo.Results.Num() > 0 && AddressInfo.Results[0].Address.IsValid())
                {
                        Address = AddressInfo.Results[0].Address.ToSharedRef();
                        bIsValid = true;
                }
        }

        if (!bIsValid)
        {
                OutError = FString::Printf(TEXT("Unable to resolve host '%s'"), *ResolvedHost);
                return false;
        }
        Address->SetPort(FMath::Clamp(Settings.ServerPort, 1, 65535));

        Socket->SetReuseAddr(true);
        Socket->SetNonBlocking(true);

        const double Timeout = FMath::Max(0.1, static_cast<double>(Settings.ConnectTimeoutSec));
        const double StartTime = FPlatformTime::Seconds();

        bool bInitiated = Socket->Connect(*Address);
        if (!bInitiated)
        {
                const int32 ErrorCode = SocketSubsystem->GetLastErrorCode();
                if (ErrorCode != SE_EWOULDBLOCK && ErrorCode != SE_EINPROGRESS)
                {
                        OutError = DescribeLastError(*SocketSubsystem);
                        return false;
                }
        }

        ESocketConnectionState ConnectionState = Socket->GetConnectionState();
        while (ConnectionState == SCS_NotConnected || ConnectionState == SCS_ConnectionPending)
        {
                if ((FPlatformTime::Seconds() - StartTime) >= Timeout)
                {
                        OutError = TEXT("Connection attempt timed out");
                        return false;
                }

                FPlatformProcess::Sleep(0.01f);
                ConnectionState = Socket->GetConnectionState();
        }

        if (ConnectionState != SCS_Connected)
        {
                OutError = TEXT("Unable to establish TCP connection");
                return false;
        }

        Socket->SetNonBlocking(false);
        OutSocket = Socket;
        return true;
}

FString FUnrealMCPDiagnostics::GetEventsLogPath(const UUnrealMCPSettings& Settings)
{
        const FString Directory = Settings.GetEffectiveLogsDirectory();
        if (Directory.IsEmpty())
        {
                return FString();
        }
        return FPaths::Combine(Directory, EventsLogName);
}

FString FUnrealMCPDiagnostics::GetMetricsLogPath(const UUnrealMCPSettings& Settings)
{
        const FString Directory = Settings.GetEffectiveLogsDirectory();
        if (Directory.IsEmpty())
        {
                return FString();
        }
        return FPaths::Combine(Directory, MetricsLogName);
}

bool FUnrealMCPDiagnostics::PerformHandshake(FSocket& Socket, const UUnrealMCPSettings& Settings, FString& OutError)
{
        using namespace UnrealMCP::Protocol;

        TSharedRef<FJsonObject> Handshake = MakeShared<FJsonObject>();
        Handshake->SetStringField(TEXT("type"), TEXT("handshake"));
        Handshake->SetNumberField(TEXT("protocolVersion"), 1);
        Handshake->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
        Handshake->SetStringField(TEXT("pluginVersion"), TEXT("UnrealMCPDiagnostics/1.0"));
        Handshake->SetStringField(TEXT("sessionId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));

        FString WriteError;
        if (!WriteFramedJson(Socket, Handshake, WriteError, Settings.ConnectTimeoutSec))
        {
                OutError = WriteError;
                return false;
        }

        const FProtocolReadResult Result = ReadFramedJson(Socket, Settings.ConnectTimeoutSec, false);
        if (!Result.bSuccess || !Result.Message.IsValid())
        {
                OutError = Result.Error.IsEmpty() ? TEXT("No handshake acknowledgment received") : Result.Error;
                return false;
        }

        FString Type;
        if (!Result.Message->TryGetStringField(TEXT("type"), Type) || !Type.Equals(TEXT("handshake/ack"), ESearchCase::IgnoreCase))
        {
                OutError = TEXT("Unexpected handshake acknowledgment");
                return false;
        }

        bool bOk = false;
        Result.Message->TryGetBoolField(TEXT("ok"), bOk);
        if (!bOk)
        {
                OutError = TEXT("Handshake acknowledgement reported failure");
                return false;
        }

        return true;
}

bool FUnrealMCPDiagnostics::SendCommand(FSocket& Socket, const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const UUnrealMCPSettings& Settings, TSharedPtr<FJsonObject>& OutResponse, FString& OutError, double TimeoutSeconds)
{
        using namespace UnrealMCP::Protocol;

        TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
        Message->SetStringField(TEXT("type"), CommandType);
        if (Params.IsValid())
        {
                Message->SetObjectField(TEXT("params"), Params);
        }

        FString SendError;
        if (!WriteFramedJson(Socket, Message, SendError, Settings.ConnectTimeoutSec))
        {
                OutError = SendError;
                return false;
        }

        const FProtocolReadResult Result = ReadFramedJson(Socket, TimeoutSeconds, false);
        if (!Result.bSuccess || !Result.Message.IsValid())
        {
                OutError = Result.Error.IsEmpty() ? TEXT("No response from server") : Result.Error;
                return false;
        }

        OutResponse = Result.Message;
        return true;
}

FString FUnrealMCPDiagnostics::ResolveHostForConnection(const FString& Host)
{
        FString Trimmed = Host;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("0.0.0.0"), ESearchCase::IgnoreCase))
        {
                return TEXT("127.0.0.1");
        }

        if (Trimmed.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
        {
                return TEXT("127.0.0.1");
        }

        return Trimmed;
}
