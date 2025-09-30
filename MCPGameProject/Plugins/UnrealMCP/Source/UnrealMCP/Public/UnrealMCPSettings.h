#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "UnrealMCPSettings.generated.h"

UENUM()
enum class EUnrealMCPLogLevel : uint8
{
        Error,
        Warning,
        Display,
        Verbose,
        VeryVerbose,
        Debug,
        Trace
};

/**
 * Project-wide settings for the Unreal MCP plugin.
 */
UCLASS(config=Editor, defaultconfig, meta=(DisplayName="Unreal MCP"))
class UNREALMCP_API UUnrealMCPSettings : public UDeveloperSettings
{
        GENERATED_BODY()

public:
        UUnrealMCPSettings();

        // === Network ===
        /** Host interface where the MCP socket server binds. */
        UPROPERTY(EditAnywhere, config, Category="Network")
        FString ServerHost = TEXT("127.0.0.1");

        /** Port used by the MCP socket server. */
        UPROPERTY(EditAnywhere, config, Category="Network", meta=(ClampMin="1", ClampMax="65535"))
        int32 ServerPort = 12029;

        /** Maximum number of seconds to wait for a client handshake. */
        UPROPERTY(EditAnywhere, config, Category="Network", meta=(ClampMin="1.0", ToolTip="Seconds"))
        float ConnectTimeoutSec = 5.0f;

        /** Socket read timeout in seconds before declaring the connection idle. */
        UPROPERTY(EditAnywhere, config, Category="Network", meta=(ClampMin="1.0", ToolTip="Seconds"))
        float ReadTimeoutSec = 60.0f;

        /** Automatically start the MCP server when the editor boots. */
        UPROPERTY(EditAnywhere, config, Category="Network")
        bool bAutoConnectOnEditorStartup = false;

        /** Interval in seconds for heartbeat pings to remote clients. */
        UPROPERTY(EditAnywhere, config, Category="Network", meta=(ClampMin="0.1", ClampMax="60.0"))
        float HeartbeatIntervalSec = 15.0f;

        // === Security ===
        UPROPERTY(EditAnywhere, config, Category="Security")
        bool AllowWrite = false;

        UPROPERTY(EditAnywhere, config, Category="Security")
        bool DryRun = true;

        UPROPERTY(EditAnywhere, config, Category="Security")
        bool RequireCheckout = false;

        UPROPERTY(EditAnywhere, config, Category="Security")
        TArray<FDirectoryPath> AllowedContentRoots;

        UPROPERTY(EditAnywhere, config, Category="Security")
        TArray<FString> AllowedTools;

        UPROPERTY(EditAnywhere, config, Category="Security")
        TArray<FString> DeniedTools;

        // === Source Control ===
        UPROPERTY(EditAnywhere, config, Category="Source Control")
        bool EnableSourceControl = true;

        UPROPERTY(EditAnywhere, config, Category="Source Control")
        bool AutoConnectSourceControl = true;

        UPROPERTY(EditAnywhere, config, Category="Source Control")
        FString PreferredProvider;

        // === Logging ===
        UPROPERTY(EditAnywhere, config, Category="Logging")
        bool bEnableProtocolVerboseLogs = false;

        /** Minimum verbosity for LogUnrealMCP category. */
        UPROPERTY(EditAnywhere, config, Category="Logging", meta=(DisplayName="Log Level"))
        EUnrealMCPLogLevel LogLevel = EUnrealMCPLogLevel::Display;

        /** Enable additional structured JSON logging (events + metrics). */
        UPROPERTY(EditAnywhere, config, Category="Logging", meta=(DisplayName="Enable JSON Logs"))
        bool bEnableJsonLogs = true;

        UPROPERTY(EditAnywhere, config, Category="Logging")
        FDirectoryPath LogsDirectory;

        virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
        virtual FText GetSectionText() const override { return NSLOCTEXT("UnrealMCP", "SettingsSection", "Unreal MCP"); }

        /** Resolves the log directory from settings or defaults. */
        FString GetEffectiveLogsDirectory() const;
};
