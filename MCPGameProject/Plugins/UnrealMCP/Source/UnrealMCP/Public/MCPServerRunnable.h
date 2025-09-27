#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Sockets.h"
#include "Interfaces/IPv4/IPv4Address.h"

class UUnrealMCPBridge;
class FJsonObject;

namespace UnrealMCP
{
namespace Protocol
{
        class FProtocolClient;
}
}

struct FMCPServerConfig
{
        double HandshakeTimeoutSeconds = 5.0;
        double ReadTimeoutSeconds = 60.0;
        double HeartbeatIntervalSeconds = 15.0;
};

/**
 * Runnable class for the MCP server thread
 */
class FMCPServerRunnable : public FRunnable
{
public:
        FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket, const FMCPServerConfig& InConfig);
        virtual ~FMCPServerRunnable();

	// FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
        UUnrealMCPBridge* Bridge;
        TSharedPtr<FSocket> ListenerSocket;
        TSharedPtr<FSocket> ClientSocket;
        bool bRunning;
        FMCPServerConfig Config;

        void RunConnection(const TSharedPtr<FSocket>& InClientSocket);
        bool HandleProtocolMessage(class UnrealMCP::Protocol::FProtocolClient& ProtocolClient, const TSharedRef<class FJsonObject>& Message);
};