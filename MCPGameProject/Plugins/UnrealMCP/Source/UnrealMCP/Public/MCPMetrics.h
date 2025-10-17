#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

/**
 * Simple metrics tracker for MCP server
 */
class UNREALMCP_API FMCPMetrics
{
public:
	FMCPMetrics();

	/** Record a command execution */
	void RecordCommand(const FString& CommandName, double ExecutionTimeSeconds, bool bSuccess);

	/** Get total commands executed */
	int32 GetTotalCommands() const { return TotalCommands; }

	/** Get total successful commands */
	int32 GetSuccessfulCommands() const { return SuccessfulCommands; }

	/** Get total failed commands */
	int32 GetFailedCommands() const { return FailedCommands; }

	/** Get average execution time in seconds */
	double GetAverageExecutionTime() const;

	/** Get metrics for a specific command */
	bool GetCommandMetrics(const FString& CommandName, int32& OutCount, double& OutAvgTime) const;

	/** Reset all metrics */
	void Reset();

	/** Log current metrics */
	void LogMetrics() const;

private:
	struct FCommandMetrics
	{
		int32 Count = 0;
		double TotalTime = 0.0;
		int32 SuccessCount = 0;
		int32 FailCount = 0;

		double GetAverageTime() const
		{
			return Count > 0 ? TotalTime / Count : 0.0;
		}
	};

	int32 TotalCommands;
	int32 SuccessfulCommands;
	int32 FailedCommands;
	double TotalExecutionTime;

	TMap<FString, FCommandMetrics> CommandMetricsMap;

	mutable FCriticalSection MetricsLock;
};
