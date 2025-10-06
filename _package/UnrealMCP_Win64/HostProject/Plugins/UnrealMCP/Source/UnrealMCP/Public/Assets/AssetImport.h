#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Implements the asset.batch_import mutation. */
class FAssetImport
{
public:
    static TSharedPtr<FJsonObject> BatchImport(const TSharedPtr<FJsonObject>& Params);
};

