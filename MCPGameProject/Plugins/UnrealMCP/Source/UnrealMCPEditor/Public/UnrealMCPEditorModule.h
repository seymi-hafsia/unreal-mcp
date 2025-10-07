#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FUnrealMCPEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    bool bSettingsRegistered = false;
    bool bCustomizationRegistered = false;
};
