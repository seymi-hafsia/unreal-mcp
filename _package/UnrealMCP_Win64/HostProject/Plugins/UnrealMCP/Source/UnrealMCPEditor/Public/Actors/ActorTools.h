#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Editor-only actor manipulation tools exposed via the MCP bridge. */
class UNREALMCPEDITOR_API FActorTools
{
public:
        /** Spawns an actor instance in the current editor world. */
        static TSharedPtr<FJsonObject> Spawn(const TSharedPtr<FJsonObject>& Params);

        /** Destroys actors by path/name. */
        static TSharedPtr<FJsonObject> Destroy(const TSharedPtr<FJsonObject>& Params);

        /** Attaches a child actor to a parent actor. */
        static TSharedPtr<FJsonObject> Attach(const TSharedPtr<FJsonObject>& Params);

        /** Applies absolute and/or additive transforms to an actor. */
        static TSharedPtr<FJsonObject> Transform(const TSharedPtr<FJsonObject>& Params);

        /** Mutates the Actor Tags array for an actor. */
        static TSharedPtr<FJsonObject> Tag(const TSharedPtr<FJsonObject>& Params);
};
