#include "UnrealMCPModule.h"
#include "CoreMinimal.h"

#include "Modules/ModuleManager.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

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
    }

    UE_LOG(LogUnrealMCP, Display, TEXT("Unreal MCP runtime module started"));
}

void FUnrealMCPModule::ShutdownModule()
{
    UE_LOG(LogUnrealMCP, Display, TEXT("Unreal MCP runtime module shut down"));
}

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)
