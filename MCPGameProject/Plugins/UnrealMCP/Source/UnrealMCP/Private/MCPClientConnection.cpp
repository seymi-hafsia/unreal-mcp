#include "MCPClientConnection.h"
#include "UnrealMCPModule.h"
#include "SocketSubsystem.h"

FMCPClientConnection::FMCPClientConnection(TSharedPtr<FSocket> InSocket, const FString& InClientName)
    : Socket(InSocket)
    , ClientName(InClientName)
    , ConnectTime(FPlatformTime::Seconds())
    , LastActivityTime(FPlatformTime::Seconds())
    , MessageCount(0)
    , CommandStartTime(0.0)
{
    UE_LOG(LogUnrealMCP, Log, TEXT("Client '%s' connected"), *ClientName);
}

FMCPClientConnection::~FMCPClientConnection()
{
    double ConnectionDuration = GetConnectionDuration();
    UE_LOG(LogUnrealMCP, Log, TEXT("Client '%s' disconnected after %.2f seconds, %d messages processed"),
        *ClientName, ConnectionDuration, MessageCount);
}

void FMCPClientConnection::UpdateLastActivity()
{
    LastActivityTime = FPlatformTime::Seconds();
}

double FMCPClientConnection::GetTimeSinceLastActivity() const
{
    return FPlatformTime::Seconds() - LastActivityTime;
}

bool FMCPClientConnection::HasTimedOut(double TimeoutSeconds) const
{
    if (TimeoutSeconds <= 0.0)
    {
        return false; // No timeout configured
    }

    return GetTimeSinceLastActivity() > TimeoutSeconds;
}

bool FMCPClientConnection::IsAlive() const
{
    if (!Socket.IsValid())
    {
        return false;
    }

    ESocketConnectionState State = Socket->GetConnectionState();
    return State == SCS_Connected;
}

double FMCPClientConnection::GetConnectionDuration() const
{
    return FPlatformTime::Seconds() - ConnectTime;
}

void FMCPClientConnection::SetPendingCommand(const FString& CommandName)
{
    PendingCommand = CommandName;
    CommandStartTime = FPlatformTime::Seconds();
    UE_LOG(LogUnrealMCP, Verbose, TEXT("Client '%s' started command: %s"), *ClientName, *CommandName);
}

void FMCPClientConnection::ClearPendingCommand()
{
    if (!PendingCommand.IsEmpty())
    {
        double ExecutionTime = GetCommandExecutionTime();
        UE_LOG(LogUnrealMCP, Verbose, TEXT("Client '%s' completed command: %s (%.3fs)"),
            *ClientName, *PendingCommand, ExecutionTime);
        PendingCommand.Empty();
        CommandStartTime = 0.0;
    }
}

double FMCPClientConnection::GetCommandExecutionTime() const
{
    if (CommandStartTime > 0.0)
    {
        return FPlatformTime::Seconds() - CommandStartTime;
    }
    return 0.0;
}
