#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Tools for inspecting and mutating existing Level Sequence bindings. */
class UNREALMCPEDITOR_API FSequenceBindings
{
public:
    /** Adds bindings for one or more actors in an existing sequence. */
    static TSharedPtr<FJsonObject> BindActors(const TSharedPtr<FJsonObject>& Params);

    /** Removes bindings by binding identifier or actor path. */
    static TSharedPtr<FJsonObject> Unbind(const TSharedPtr<FJsonObject>& Params);

    /** Lists the bindings currently present on a sequence. */
    static TSharedPtr<FJsonObject> List(const TSharedPtr<FJsonObject>& Params);
};
