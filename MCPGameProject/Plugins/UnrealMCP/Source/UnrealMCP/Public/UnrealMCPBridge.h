#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "UnrealMCPBridge.generated.h"

class FMCPServerRunnable;
class FInternetAddr;
class FSocket;
class FUnrealMCPEditorCommands;
class FUnrealMCPBlueprintCommands;
class FUnrealMCPBlueprintNodeCommands;
class FUnrealMCPProjectCommands;
class FUnrealMCPUMGCommands;
class FUnrealMCPSourceControlCommands;
class FContentTools;

/**
 * Editor subsystem for MCP Bridge
 * Handles communication between external tools and the Unreal Editor
 * through a TCP socket connection. Commands are received as JSON and
 * routed to appropriate command handlers.
 */
UCLASS()
class UNREALMCP_API UUnrealMCPBridge : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UUnrealMCPBridge();
	virtual ~UUnrealMCPBridge();

	// UEditorSubsystem implementation
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Server functions
	void StartServer();
	void StopServer();
	bool IsRunning() const { return bIsRunning; }

	// Command execution
        FString ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FString& RequestId);

private:
	// Server state
	bool bIsRunning;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ConnectionSocket;
	FRunnableThread* ServerThread;

	// Server configuration
	FIPv4Address ServerAddress;
	uint16 Port;

	// Command handler instances
	TSharedPtr<FUnrealMCPEditorCommands> EditorCommands;
	TSharedPtr<FUnrealMCPBlueprintCommands> BlueprintCommands;
        TSharedPtr<FUnrealMCPBlueprintNodeCommands> BlueprintNodeCommands;
        TSharedPtr<FUnrealMCPProjectCommands> ProjectCommands;
        TSharedPtr<FUnrealMCPUMGCommands> UMGCommands;
        TSharedPtr<FUnrealMCPSourceControlCommands> SourceControlCommands;
        TSharedPtr<FContentTools> ContentTools;
};
