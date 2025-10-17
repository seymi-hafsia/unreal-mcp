#include "MCPMetrics.h"
#include "UnrealMCPModule.h"

FMCPMetrics::FMCPMetrics()
	: TotalCommands(0)
	, SuccessfulCommands(0)
	, FailedCommands(0)
	, TotalExecutionTime(0.0)
{
}

void FMCPMetrics::RecordCommand(const FString& CommandName, double ExecutionTimeSeconds, bool bSuccess)
{
	FScopeLock Lock(&MetricsLock);

	TotalCommands++;
	TotalExecutionTime += ExecutionTimeSeconds;

	if (bSuccess)
	{
		SuccessfulCommands++;
	}
	else
	{
		FailedCommands++;
	}

	// Update per-command metrics
	FCommandMetrics& Metrics = CommandMetricsMap.FindOrAdd(CommandName);
	Metrics.Count++;
	Metrics.TotalTime += ExecutionTimeSeconds;

	if (bSuccess)
	{
		Metrics.SuccessCount++;
	}
	else
	{
		Metrics.FailCount++;
	}
}

double FMCPMetrics::GetAverageExecutionTime() const
{
	FScopeLock Lock(&MetricsLock);
	return TotalCommands > 0 ? TotalExecutionTime / TotalCommands : 0.0;
}

bool FMCPMetrics::GetCommandMetrics(const FString& CommandName, int32& OutCount, double& OutAvgTime) const
{
	FScopeLock Lock(&MetricsLock);

	const FCommandMetrics* Metrics = CommandMetricsMap.Find(CommandName);
	if (Metrics)
	{
		OutCount = Metrics->Count;
		OutAvgTime = Metrics->GetAverageTime();
		return true;
	}

	return false;
}

void FMCPMetrics::Reset()
{
	FScopeLock Lock(&MetricsLock);

	TotalCommands = 0;
	SuccessfulCommands = 0;
	FailedCommands = 0;
	TotalExecutionTime = 0.0;
	CommandMetricsMap.Reset();

	UE_LOG(LogUnrealMCP, Log, TEXT("Metrics reset"));
}

void FMCPMetrics::LogMetrics() const
{
	FScopeLock Lock(&MetricsLock);

	UE_LOG(LogUnrealMCP, Log, TEXT("=== MCP Server Metrics ==="));
	UE_LOG(LogUnrealMCP, Log, TEXT("Total Commands: %d"), TotalCommands);
	UE_LOG(LogUnrealMCP, Log, TEXT("Successful: %d (%.1f%%)"), SuccessfulCommands,
		TotalCommands > 0 ? (SuccessfulCommands * 100.0f / TotalCommands) : 0.0f);
	UE_LOG(LogUnrealMCP, Log, TEXT("Failed: %d (%.1f%%)"), FailedCommands,
		TotalCommands > 0 ? (FailedCommands * 100.0f / TotalCommands) : 0.0f);
	UE_LOG(LogUnrealMCP, Log, TEXT("Average Execution Time: %.3f seconds"), GetAverageExecutionTime());

	if (CommandMetricsMap.Num() > 0)
	{
		UE_LOG(LogUnrealMCP, Log, TEXT("Per-Command Metrics:"));

		// Sort by count descending
		TArray<TPair<FString, FCommandMetrics>> SortedMetrics;
		for (const auto& Pair : CommandMetricsMap)
		{
			SortedMetrics.Add(TPair<FString, FCommandMetrics>(Pair.Key, Pair.Value));
		}
		SortedMetrics.Sort([](const TPair<FString, FCommandMetrics>& A, const TPair<FString, FCommandMetrics>& B)
		{
			return A.Value.Count > B.Value.Count;
		});

		for (const auto& Pair : SortedMetrics)
		{
			const FString& CommandName = Pair.Key;
			const FCommandMetrics& Metrics = Pair.Value;

			UE_LOG(LogUnrealMCP, Log, TEXT("  %s: %d calls, %.3fs avg, %d success, %d failed"),
				*CommandName, Metrics.Count, Metrics.GetAverageTime(), Metrics.SuccessCount, Metrics.FailCount);
		}
	}

	UE_LOG(LogUnrealMCP, Log, TEXT("========================="));
}
