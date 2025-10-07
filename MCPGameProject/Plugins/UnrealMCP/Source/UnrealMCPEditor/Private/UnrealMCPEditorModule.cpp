#include "UnrealMCPEditorModule.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "Observability/JsonLogger.h"
#include "PropertyEditorModule.h"
#include "Settings/UnrealMCPSettingsCustomization.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

#define LOCTEXT_NAMESPACE "FUnrealMCPEditorModule"

void FUnrealMCPEditorModule::StartupModule()
{
    if (const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>())
    {
        FJsonLogger::Init(Settings->GetEffectiveLogsDirectory(), Settings->bEnableJsonLogs);
    }

    if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
    {
        SettingsModule->RegisterSettings(
            TEXT("Project"),
            TEXT("Plugins"),
            TEXT("Unreal MCP"),
            LOCTEXT("UnrealMCPSettingsName", "Unreal MCP"),
            LOCTEXT("UnrealMCPSettingsDescription", "Configure network, security, logging, and diagnostics for the Unreal MCP plugin."),
            GetMutableDefault<UUnrealMCPSettings>());
        bSettingsRegistered = true;
    }

    if (FPropertyEditorModule* PropertyEditorModule = FModuleManager::LoadModulePtr<FPropertyEditorModule>("PropertyEditor"))
    {
        PropertyEditorModule->RegisterCustomClassLayout(
            UUnrealMCPSettings::StaticClass()->GetFName(),
            FOnGetDetailCustomizationInstance::CreateStatic(&FUnrealMCPSettingsCustomization::MakeInstance));
        PropertyEditorModule->NotifyCustomizationModuleChanged();
        bCustomizationRegistered = true;
    }

    UE_LOG(LogUnrealMCP, Display, TEXT("Unreal MCP editor module started"));
}

void FUnrealMCPEditorModule::ShutdownModule()
{
    if (bCustomizationRegistered)
    {
        if (FPropertyEditorModule* PropertyEditorModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
        {
            PropertyEditorModule->UnregisterCustomClassLayout(UUnrealMCPSettings::StaticClass()->GetFName());
            PropertyEditorModule->NotifyCustomizationModuleChanged();
        }
        bCustomizationRegistered = false;
    }

    if (bSettingsRegistered)
    {
        if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
        {
            SettingsModule->UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("Unreal MCP"));
        }
        bSettingsRegistered = false;
    }

    UE_LOG(LogUnrealMCP, Display, TEXT("Unreal MCP editor module shut down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealMCPEditorModule, UnrealMCPEditor)
