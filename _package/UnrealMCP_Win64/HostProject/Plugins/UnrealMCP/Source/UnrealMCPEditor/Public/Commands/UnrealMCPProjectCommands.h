#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Handler class for Project-wide MCP commands
 */
class UNREALMCPEDITOR_API FUnrealMCPProjectCommands
{
public:
    FUnrealMCPProjectCommands();

    // Handle project commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Specific project command handlers
    TSharedPtr<FJsonObject> HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params);
}; 