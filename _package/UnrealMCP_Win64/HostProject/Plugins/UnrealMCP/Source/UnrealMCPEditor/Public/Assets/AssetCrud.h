#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Utility functions implementing MCP-driven asset CRUD operations. */
class FAssetCrud
{
public:
    static TSharedPtr<FJsonObject> CreateFolder(const TSharedPtr<FJsonObject>& Params);
    static TSharedPtr<FJsonObject> Rename(const TSharedPtr<FJsonObject>& Params);
    static TSharedPtr<FJsonObject> Delete(const TSharedPtr<FJsonObject>& Params);
    static TSharedPtr<FJsonObject> FixRedirectors(const TSharedPtr<FJsonObject>& Params);
    static TSharedPtr<FJsonObject> SaveAll(const TSharedPtr<FJsonObject>& Params);
};

