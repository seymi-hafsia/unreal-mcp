#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MCPSettings.generated.h"

/**
 * Settings for the Unreal MCP Server
 */
UCLASS(Config=Editor, DefaultConfig, meta = (DisplayName = "Unreal MCP"))
class UNREALMCP_API UMCPSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMCPSettings();

	/** Server host address (default: 127.0.0.1) */
	UPROPERTY(Config, EditAnywhere, Category = "Server", meta = (DisplayName = "Server Host"))
	FString ServerHost;

	/** Server port (default: 55557) */
	UPROPERTY(Config, EditAnywhere, Category = "Server", meta = (DisplayName = "Server Port", ClampMin = "1024", ClampMax = "65535"))
	int32 ServerPort;

	/** Auto-start server on editor startup */
	UPROPERTY(Config, EditAnywhere, Category = "Server", meta = (DisplayName = "Auto-Start Server"))
	bool bAutoStartServer;

	/** Maximum buffer size for receiving data in bytes (default: 65536) */
	UPROPERTY(Config, EditAnywhere, Category = "Performance", meta = (DisplayName = "Receive Buffer Size", ClampMin = "4096", ClampMax = "1048576"))
	int32 ReceiveBufferSize;

	/** Command execution timeout in seconds (0 = no timeout) */
	UPROPERTY(Config, EditAnywhere, Category = "Performance", meta = (DisplayName = "Command Timeout (seconds)", ClampMin = "0", ClampMax = "300"))
	float CommandTimeout;

	/** Client inactivity timeout in seconds (0 = no timeout) */
	UPROPERTY(Config, EditAnywhere, Category = "Performance", meta = (DisplayName = "Client Timeout (seconds)", ClampMin = "0", ClampMax = "3600"))
	float ClientTimeout;

	/** Heartbeat interval in seconds (0 = disabled) */
	UPROPERTY(Config, EditAnywhere, Category = "Performance", meta = (DisplayName = "Heartbeat Interval (seconds)", ClampMin = "0", ClampMax = "60"))
	float HeartbeatInterval;

	/** Maximum message size in MB (clients sending larger messages will be disconnected) */
	UPROPERTY(Config, EditAnywhere, Category = "Performance", meta = (DisplayName = "Max Message Size (MB)", ClampMin = "1", ClampMax = "100"))
	int32 MaxMessageSizeMB;

	/** Enable verbose logging */
	UPROPERTY(Config, EditAnywhere, Category = "Logging", meta = (DisplayName = "Verbose Logging"))
	bool bVerboseLogging;

	/** Log command execution */
	UPROPERTY(Config, EditAnywhere, Category = "Logging", meta = (DisplayName = "Log Commands"))
	bool bLogCommands;

	/** Log command responses */
	UPROPERTY(Config, EditAnywhere, Category = "Logging", meta = (DisplayName = "Log Responses"))
	bool bLogResponses;

	// UDeveloperSettings interface
	virtual FName GetCategoryName() const override;

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
};
