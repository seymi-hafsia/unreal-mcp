#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UUnrealMCPSettings;

struct FMutationAction
{
        FString Op;
        TMap<FString, FString> Args;
};

struct FMutationPlan
{
        bool bDryRun = true;
        TArray<FMutationAction> Actions;
};

/** Centralized enforcement for write operations issued via the MCP bridge. */
class UNREALMCP_API FWriteGate
{
public:
        static bool IsMutationCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

        /** Returns true when writes are allowed after considering plugin and remote settings. */
        static bool IsWriteAllowed();

        /** Returns true when operations must run in dry-run mode. */
        static bool ShouldDryRun();

        /** Validates that a content path is permitted. Empty paths are treated as editor-only mutations. */
        static bool IsPathAllowed(const FString& ContentPath, FString& OutReason);

        /** Validates the write gate for a command and path, returning false if blocked. */
        static bool CanMutate(const FString& CommandType, const FString& ContentPath, FString& OutReason);

        /** Validates the tool allow/deny lists for a command. */
        static bool IsToolAllowed(const FString& CommandType, FString& OutReason);

        /** Builds a simple mutation plan for auditing. */
        static FMutationPlan BuildPlan(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

        /** Builds an audit JSON object from a plan. */
        static TSharedPtr<FJsonObject> BuildAuditJson(const FMutationPlan& Plan, bool bExecuted);

        /** Shared transaction label used for MCP-driven mutations. */
        static const FString& GetTransactionName();

        /** Ensures the target content path is checked out when required by settings. */
        static bool EnsureCheckoutForContentPath(const FString& ContentPath, TSharedPtr<FJsonObject>& OutError);

        /** Creates a structured error payload for missing source control checkout. */
        static TSharedPtr<FJsonObject> MakeSourceControlRequiredError(const FString& AssetPath, const FString& FailureMessage = FString());

        /** Creates an error payload for write-not-allowed responses. */
        static TSharedPtr<FJsonObject> MakeWriteNotAllowedError(const FString& CommandType);

        /** Creates an error payload when a path is outside of the allowlist. */
        static TSharedPtr<FJsonObject> MakePathNotAllowedError(const FString& Path, const FString& Reason);

        /** Creates an error payload when a tool is blocked by policy. */
        static TSharedPtr<FJsonObject> MakeToolNotAllowedError(const FString& CommandType, const FString& Reason);

        /** Updates remote enforcement flags received from the Python server. */
        static void UpdateRemoteEnforcement(bool bAllowWrite, bool bDryRun, const TArray<FString>& AllowedPaths, const TArray<FString>& AllowedTools, const TArray<FString>& DeniedTools);

        /** Resolves a potential content path from known command parameters. */
        static FString ResolvePathForCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

        /** Produces a merged list of allowed content roots. */
        static TArray<FString> GetEffectiveAllowedRoots();

private:
        static const UUnrealMCPSettings* GetSettings();
        static FString NormalizeContentPath(const FString& InPath);
};
