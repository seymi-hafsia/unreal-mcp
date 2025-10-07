#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Utilities to create and edit material instances via MCP tools. */
class FMaterialInstanceTools
{
public:
    /** Implements the `mi.create` tool. */
    static TSharedPtr<FJsonObject> Create(const TSharedPtr<FJsonObject>& Params);

    /** Implements the `mi.set_params` tool. */
    static TSharedPtr<FJsonObject> SetParameters(const TSharedPtr<FJsonObject>& Params);
};
