#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Tools for mutating movie scene tracks on existing level sequences. */
class UNREALMCPEDITOR_API FSequenceTracks
{
public:
    /** Adds one or more tracks (transform/visibility/property) and optional camera cuts to a sequence. */
    static TSharedPtr<FJsonObject> AddTracks(const TSharedPtr<FJsonObject>& Params);
};
