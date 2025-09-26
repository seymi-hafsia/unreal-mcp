#include "UnrealMCPSettings.h"

UUnrealMCPSettings::UUnrealMCPSettings()
{
        AllowWrite = false;
        DryRun = true;
        RequireCheckout = false;
}

FName UUnrealMCPSettings::GetCategoryName() const
{
        return TEXT("Plugins");
}
