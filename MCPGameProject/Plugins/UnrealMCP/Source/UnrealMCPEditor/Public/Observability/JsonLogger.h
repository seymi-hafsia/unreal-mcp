#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** Structure describing a structured log event. */
struct FLogEvent
{
    FString Level;
    FString Category;
    FString RequestId;
    FString SessionId;
    FString Message;
    TSharedPtr<FJsonObject> Fields;
    double TsUnixMs = 0.0;
};

/** Simple JSON lines logger for events and metrics. */
class UNREALMCPEDITOR_API FJsonLogger
{
public:
    /** Initialise the logger with the directory for log files. */
    static void Init(const FString& Directory, bool bEnable);

    /** Emit a structured log event. */
    static void Log(const FLogEvent& Event);

    /** Emit a structured metric entry. */
    static void Metric(const FString& Name, const TSharedPtr<FJsonObject>& Fields);

private:
    static FCriticalSection CriticalSection;
    static FString EventsPath;
    static FString MetricsPath;
    static bool bIsEnabled;

    static void EnsureDirectory(const FString& Directory);
    static void WriteLine(const FString& Path, const TSharedRef<FJsonObject>& Payload);
    static void RotateIfNeeded(const FString& Path);
    static TSharedPtr<FJsonObject> CloneJson(const TSharedPtr<FJsonObject>& Source);
    static double NowUnixMs();
};
