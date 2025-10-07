#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Read-only tool for exporting Level Sequence structure to JSON or CSV. */
class UNREALMCPEDITOR_API FSequenceExport
{
public:
    /** Exports the structure of a sequence in either JSON or CSV form. */
    static TSharedPtr<FJsonObject> Export(const TSharedPtr<FJsonObject>& Params);
};
