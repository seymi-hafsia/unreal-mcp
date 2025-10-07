#include "CoreMinimal.h"
#include "UnrealMCPSettings.h"

#include "Misc/Paths.h"

namespace
{
        FString SanitizeHost(const FString& InHost)
        {
                FString Host = InHost;
                Host.TrimStartAndEndInline();
                if (Host.IsEmpty())
                {
                        Host = TEXT("127.0.0.1");
                }
                return Host;
        }

        FString ResolveLogsPath(const FDirectoryPath& Directory)
        {
                if (!Directory.Path.IsEmpty())
                {
                        FString AbsolutePath = Directory.Path;
                        AbsolutePath.TrimStartAndEndInline();
                        if (AbsolutePath.Contains(TEXT("$(ProjectDir)")))
                        {
                                AbsolutePath.ReplaceInline(TEXT("$(ProjectDir)"), *FPaths::ProjectDir());
                        }
                        if (FPaths::IsRelative(AbsolutePath))
                        {
                                AbsolutePath = FPaths::ConvertRelativePathToFull(AbsolutePath);
                        }
                        return AbsolutePath;
                }

                return FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
        }
}

UUnrealMCPSettings::UUnrealMCPSettings()
{
        ServerHost = SanitizeHost(ServerHost);
        ServerPort = FMath::Clamp(ServerPort, 1, 65535);
        ConnectTimeoutSec = FMath::Max(1.0f, ConnectTimeoutSec);
        ReadTimeoutSec = FMath::Max(1.0f, ReadTimeoutSec);
        HeartbeatIntervalSec = FMath::Clamp(HeartbeatIntervalSec, 0.1f, 60.0f);
        LogsDirectory.Path = ResolveLogsPath(LogsDirectory);
}

FString UUnrealMCPSettings::GetEffectiveLogsDirectory() const
{
        return ResolveLogsPath(LogsDirectory);
}
