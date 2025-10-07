#include "CoreMinimal.h"
#include "Observability/JsonLogger.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

FCriticalSection FJsonLogger::CriticalSection;
FString FJsonLogger::EventsPath;
FString FJsonLogger::MetricsPath;
bool FJsonLogger::bIsEnabled = false;

namespace
{
    constexpr int64 MaxFileBytes = 20 * 1024 * 1024; // 20 MiB
    constexpr int32 MaxFileGenerations = 3;

    FString BuildMetricPath(const FString& Directory)
    {
        return FPaths::Combine(Directory, TEXT("UnrealMCP_metrics.jsonl"));
    }

    FString BuildEventsPath(const FString& Directory)
    {
        return FPaths::Combine(Directory, TEXT("UnrealMCP_events.jsonl"));
    }

    void RotateFile(const FString& Path)
    {
        if (Path.IsEmpty())
        {
            return;
        }

        IFileManager& FileManager = IFileManager::Get();
        for (int32 Index = MaxFileGenerations - 1; Index >= 0; --Index)
        {
            const FString Source = Index == 0 ? Path : FString::Printf(TEXT("%s.%d"), *Path, Index);
            if (!FileManager.FileExists(*Source))
            {
                continue;
            }

            if (Index >= MaxFileGenerations - 1)
            {
                FileManager.Delete(*Source, false, true);
                continue;
            }

            const FString Destination = FString::Printf(TEXT("%s.%d"), *Path, Index + 1);
            FileManager.Move(*Destination, *Source, true, true, true);
        }
    }
}

void FJsonLogger::Init(const FString& Directory, bool bEnable)
{
    FScopeLock Lock(&CriticalSection);
    bIsEnabled = bEnable;
    if (!bIsEnabled)
    {
        EventsPath.Reset();
        MetricsPath.Reset();
        return;
    }

    FString NormalizedDir = Directory;
    if (NormalizedDir.IsEmpty())
    {
        NormalizedDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"));
    }

    EnsureDirectory(NormalizedDir);

    EventsPath = BuildEventsPath(NormalizedDir);
    MetricsPath = BuildMetricPath(NormalizedDir);
}

void FJsonLogger::Log(const FLogEvent& Event)
{
    if (!bIsEnabled)
    {
        return;
    }

    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("level"), Event.Level.IsEmpty() ? TEXT("info") : Event.Level.ToLower());
    if (!Event.Category.IsEmpty())
    {
        Payload->SetStringField(TEXT("category"), Event.Category);
    }
    if (!Event.RequestId.IsEmpty())
    {
        Payload->SetStringField(TEXT("requestId"), Event.RequestId);
    }
    if (!Event.SessionId.IsEmpty())
    {
        Payload->SetStringField(TEXT("sessionId"), Event.SessionId);
    }
    if (!Event.Message.IsEmpty())
    {
        Payload->SetStringField(TEXT("message"), Event.Message);
    }

    Payload->SetNumberField(TEXT("ts"), Event.TsUnixMs > 0.0 ? Event.TsUnixMs : NowUnixMs());

    if (Event.Fields.IsValid())
    {
        Payload->SetObjectField(TEXT("fields"), CloneJson(Event.Fields));
    }

    WriteLine(EventsPath, Payload);
}

void FJsonLogger::Metric(const FString& Name, const TSharedPtr<FJsonObject>& Fields)
{
    if (!bIsEnabled)
    {
        return;
    }

    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("metric"), Name);
    Payload->SetNumberField(TEXT("ts"), NowUnixMs());
    if (Fields.IsValid())
    {
        Payload->SetObjectField(TEXT("fields"), CloneJson(Fields));
    }

    WriteLine(MetricsPath, Payload);
}

void FJsonLogger::EnsureDirectory(const FString& Directory)
{
    if (Directory.IsEmpty())
    {
        return;
    }

    IFileManager::Get().MakeDirectory(*Directory, true);
}

void FJsonLogger::WriteLine(const FString& Path, const TSharedRef<FJsonObject>& Payload)
{
    if (Path.IsEmpty())
    {
        return;
    }

    FScopeLock Lock(&CriticalSection);
    RotateIfNeeded(Path);

    FString Serialized;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Payload, Writer, true);
    Serialized.AppendChar(TEXT('\n'));

    FFileHelper::SaveStringToFile(Serialized, *Path, FFileHelper::EEncodingOptions::ForceUTF8, &IFileManager::Get(), FILEWRITE_Append);
}

void FJsonLogger::RotateIfNeeded(const FString& Path)
{
    if (Path.IsEmpty())
    {
        return;
    }

    const int64 CurrentSize = IFileManager::Get().FileSize(*Path);
    if (CurrentSize >= MaxFileBytes)
    {
        RotateFile(Path);
    }
}

TSharedPtr<FJsonObject> FJsonLogger::CloneJson(const TSharedPtr<FJsonObject>& Source)
{
    if (!Source.IsValid())
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
    for (const auto& Pair : Source->Values)
    {
        Copy->SetField(Pair.Key, Pair.Value);
    }
    return Copy;
}

double FJsonLogger::NowUnixMs()
{
    const FDateTime Now = FDateTime::UtcNow();
    return static_cast<double>(Now.ToUnixTimestamp()) * 1000.0 + static_cast<double>(Now.GetMillisecond());
}
