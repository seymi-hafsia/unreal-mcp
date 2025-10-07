#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Editor-focused helpers for loading, saving, and streaming levels.
 */
class UNREALMCPEDITOR_API FLevelTools
{
public:
    static TSharedPtr<FJsonObject> SaveOpen(const TSharedPtr<FJsonObject>& Params);
    static TSharedPtr<FJsonObject> Load(const TSharedPtr<FJsonObject>& Params);
    static TSharedPtr<FJsonObject> Unload(const TSharedPtr<FJsonObject>& Params);
    static TSharedPtr<FJsonObject> StreamSublevel(const TSharedPtr<FJsonObject>& Params);
};
