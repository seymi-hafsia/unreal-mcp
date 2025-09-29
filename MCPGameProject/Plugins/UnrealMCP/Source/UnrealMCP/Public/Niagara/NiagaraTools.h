#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Editor-facing Niagara helpers exposed through the MCP bridge. */
class UNREALMCP_API FNiagaraTools
{
public:
        /** Spawns a Niagara component in the current editor world. */
        static TSharedPtr<FJsonObject> SpawnComponent(const TSharedPtr<FJsonObject>& Params);

        /** Sets user parameters on an existing Niagara component. */
        static TSharedPtr<FJsonObject> SetUserParameters(const TSharedPtr<FJsonObject>& Params);

        /** Activates a Niagara component. */
        static TSharedPtr<FJsonObject> Activate(const TSharedPtr<FJsonObject>& Params);

        /** Deactivates a Niagara component. */
        static TSharedPtr<FJsonObject> Deactivate(const TSharedPtr<FJsonObject>& Params);
};
