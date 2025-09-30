#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Placeholder MetaSound helpers exposed through the MCP bridge. */
class UNREALMCP_API FMetaSoundTools
{
public:
        /** Spawn a MetaSound-backed audio component (currently not implemented). */
        static TSharedPtr<FJsonObject> SpawnComponent(const TSharedPtr<FJsonObject>& Params);

        /** Set parameters on an AudioComponent that plays a MetaSound (currently not implemented). */
        static TSharedPtr<FJsonObject> SetParameters(const TSharedPtr<FJsonObject>& Params);

        /** Start playback on an AudioComponent (currently not implemented). */
        static TSharedPtr<FJsonObject> Play(const TSharedPtr<FJsonObject>& Params);

        /** Stop playback on an AudioComponent (currently not implemented). */
        static TSharedPtr<FJsonObject> Stop(const TSharedPtr<FJsonObject>& Params);

        /** Export information about a MetaSound source (currently not implemented). */
        static TSharedPtr<FJsonObject> ExportInfo(const TSharedPtr<FJsonObject>& Params);

        /** Create or update a MetaSound preset asset (currently not implemented). */
        static TSharedPtr<FJsonObject> PatchPreset(const TSharedPtr<FJsonObject>& Params);
};

