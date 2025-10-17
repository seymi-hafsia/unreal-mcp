#include "MCPSettings.h"

#define LOCTEXT_NAMESPACE "MCPSettings"

UMCPSettings::UMCPSettings()
	: ServerHost(TEXT("127.0.0.1"))
	, ServerPort(55557)
	, bAutoStartServer(true)
	, ReceiveBufferSize(65536)
	, CommandTimeout(30.0f)
	, bVerboseLogging(false)
	, bLogCommands(true)
	, bLogResponses(false)
{
}

FName UMCPSettings::GetCategoryName() const
{
	return FName(TEXT("Plugins"));
}

#if WITH_EDITOR
FText UMCPSettings::GetSectionText() const
{
	return LOCTEXT("SettingsDisplayName", "Unreal MCP");
}

FText UMCPSettings::GetSectionDescription() const
{
	return LOCTEXT("SettingsDescription", "Configure the Unreal Model Context Protocol server settings");
}
#endif

#undef LOCTEXT_NAMESPACE
