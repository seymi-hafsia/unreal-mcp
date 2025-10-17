#pragma once

#include "CoreMinimal.h"
#include "Sockets.h"
#include "HAL/PlatformTime.h"

/**
 * Represents a client connection with heartbeat and timeout tracking
 */
class UNREALMCP_API FMCPClientConnection
{
public:
    FMCPClientConnection(TSharedPtr<FSocket> InSocket, const FString& InClientName);
    ~FMCPClientConnection();

    /** Get the client socket */
    TSharedPtr<FSocket> GetSocket() const { return Socket; }

    /** Get client name/identifier */
    FString GetClientName() const { return ClientName; }

    /** Update last activity timestamp */
    void UpdateLastActivity();

    /** Get time since last activity in seconds */
    double GetTimeSinceLastActivity() const;

    /** Check if connection has timed out */
    bool HasTimedOut(double TimeoutSeconds) const;

    /** Check if connection is alive */
    bool IsAlive() const;

    /** Get connection duration in seconds */
    double GetConnectionDuration() const;

    /** Increment message count */
    void IncrementMessageCount() { ++MessageCount; }

    /** Get total messages received */
    int32 GetMessageCount() const { return MessageCount; }

    /** Set pending command (for timeout tracking) */
    void SetPendingCommand(const FString& CommandName);

    /** Clear pending command */
    void ClearPendingCommand();

    /** Get pending command name */
    FString GetPendingCommand() const { return PendingCommand; }

    /** Check if a command is currently executing */
    bool HasPendingCommand() const { return !PendingCommand.IsEmpty(); }

    /** Get time since command started in seconds */
    double GetCommandExecutionTime() const;

private:
    TSharedPtr<FSocket> Socket;
    FString ClientName;
    double ConnectTime;
    double LastActivityTime;
    int32 MessageCount;

    // Command execution tracking
    FString PendingCommand;
    double CommandStartTime;
};
