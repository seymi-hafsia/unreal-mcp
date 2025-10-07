#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Tools for creating and initializing Level Sequence assets via the MCP bridge. */
class UNREALMCPEDITOR_API FSequenceTools
{
public:
    /** Creates or overwrites a Level Sequence asset with optional camera and bindings. */
    static TSharedPtr<FJsonObject> Create(const TSharedPtr<FJsonObject>& Params);
};

