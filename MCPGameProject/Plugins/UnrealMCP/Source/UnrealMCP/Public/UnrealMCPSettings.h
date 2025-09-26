#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealMCPSettings.generated.h"

/**
 * Project-wide settings for Unreal MCP permissions and enforcement.
 */
UCLASS(config=Game, defaultconfig, meta=(DisplayName="Unreal MCP"))
class UNREALMCP_API UUnrealMCPSettings : public UDeveloperSettings
{
        GENERATED_BODY()

public:
        UUnrealMCPSettings();

        /** When false, all write operations are rejected regardless of dry-run state. */
        UPROPERTY(EditAnywhere, config, Category="Permissions")
        bool AllowWrite;

        /** When true, mutations are planned but not executed. */
        UPROPERTY(EditAnywhere, config, Category="Permissions")
        bool DryRun;

        /** Whitelisted content root folders (e.g. /Game/Core). Empty means no paths are allowed. */
        UPROPERTY(EditAnywhere, config, Category="Permissions")
        TArray<FDirectoryPath> AllowedContentRoots;

        /** Require assets to be checked out in source control before mutation. */
        UPROPERTY(EditAnywhere, config, Category="Permissions")
        bool RequireCheckout;

        virtual FName GetCategoryName() const override;
};
