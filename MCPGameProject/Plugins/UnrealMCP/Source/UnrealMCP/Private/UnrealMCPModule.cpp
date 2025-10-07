#include "UnrealMCPModule.h"
#include "UnrealMCPBridge.h"
#include "Modules/ModuleManager.h"
#include "EditorSubsystem.h"
#include "Editor.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"
#include "Observability/JsonLogger.h"
#include "Settings/UnrealMCPSettingsCustomization.h"

#include "ISettingsModule.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "FUnrealMCPModule"

DEFINE_LOG_CATEGORY(LogUnrealMCP);

namespace
{
ELogVerbosity::Type ToVerbosity(EUnrealMCPLogLevel Level)
{
        switch (Level)
        {
        case EUnrealMCPLogLevel::Error:
                return ELogVerbosity::Error;
        case EUnrealMCPLogLevel::Warning:
                return ELogVerbosity::Warning;
        case EUnrealMCPLogLevel::Display:
                return ELogVerbosity::Display;
        case EUnrealMCPLogLevel::Verbose:
                return ELogVerbosity::Verbose;
        case EUnrealMCPLogLevel::VeryVerbose:
                return ELogVerbosity::VeryVerbose;
        case EUnrealMCPLogLevel::Debug:
                return ELogVerbosity::Log;
        case EUnrealMCPLogLevel::Trace:
                return ELogVerbosity::VeryVerbose;
        default:
                return ELogVerbosity::Display;
        }
}
}

void FUnrealMCPModule::StartupModule()
{
        if (const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>())
        {
                LogUnrealMCP.SetVerbosity(ToVerbosity(Settings->LogLevel));
                FJsonLogger::Init(Settings->GetEffectiveLogsDirectory(), Settings->bEnableJsonLogs);
        }

        UE_LOG(LogUnrealMCP, Display, TEXT("Unreal MCP Module has started"));

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
}

void FUnrealMCPModule::ShutdownModule()
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

        UE_LOG(LogUnrealMCP, Display, TEXT("Unreal MCP Module has shut down"));
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP) 