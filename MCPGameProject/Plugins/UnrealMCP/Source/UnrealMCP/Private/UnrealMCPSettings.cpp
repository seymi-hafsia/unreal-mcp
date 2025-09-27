#include "UnrealMCPSettings.h"

UUnrealMCPSettings::UUnrealMCPSettings()
{
        AllowWrite = false;
        DryRun = true;
        RequireCheckout = false;
        EnableSourceControl = true;
        AutoConnectSourceControl = true;
        PreferredProvider = TEXT("");
}

FName UUnrealMCPSettings::GetCategoryName() const
{
        return TEXT("Plugins");
}
