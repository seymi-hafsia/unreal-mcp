#pragma once

#include "CoreMinimal.h"

class FSocket;
class UUnrealMCPSettings;

/** Utility helpers for diagnostics actions triggered from the settings panel. */
class FUnrealMCPDiagnostics
{
public:
        /** Attempts to connect and perform a handshake using the current settings. */
        static bool TestConnection(FText& OutMessage);

        /** Sends a ping command and reports the round-trip time. */
        static bool SendPing(FText& OutMessage);

        /** Opens the configured logs directory in the platform file explorer. */
        static bool OpenLogsFolder(FText& OutMessage);

private:
        static bool ConnectToServer(const UUnrealMCPSettings& Settings, TSharedPtr<FSocket>& OutSocket, FString& OutError);
        static bool PerformHandshake(FSocket& Socket, const UUnrealMCPSettings& Settings, FString& OutError);
        static bool SendCommand(FSocket& Socket, const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const UUnrealMCPSettings& Settings, TSharedPtr<FJsonObject>& OutResponse, FString& OutError, double TimeoutSeconds);
        static FString ResolveHostForConnection(const FString& Host);
};
