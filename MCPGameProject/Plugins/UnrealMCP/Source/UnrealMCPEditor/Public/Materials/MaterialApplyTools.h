#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Tools for applying material instances and remapping mesh material slots. */
class FMaterialApplyTools
{
public:
    /** Implements the `mi.batch_apply` tool. */
    static TSharedPtr<FJsonObject> BatchApply(const TSharedPtr<FJsonObject>& Params);

    /** Implements the `mesh.remap_material_slots` tool. */
    static TSharedPtr<FJsonObject> RemapMaterialSlots(const TSharedPtr<FJsonObject>& Params);
};
