#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Editor-only navigation helpers exposed through the MCP bridge.
 */
class UNREALMCPEDITOR_API FEditorNavTools
{
public:
        /** Applies selection filters on level actors. */
        static TSharedPtr<FJsonObject> LevelSelect(const TSharedPtr<FJsonObject>& Params);

        /** Moves the active editor viewport towards actors or coordinates. */
        static TSharedPtr<FJsonObject> ViewportFocus(const TSharedPtr<FJsonObject>& Params);

        /** Manages editor camera bookmarks (session & persistent). */
        static TSharedPtr<FJsonObject> CameraBookmark(const TSharedPtr<FJsonObject>& Params);
};

