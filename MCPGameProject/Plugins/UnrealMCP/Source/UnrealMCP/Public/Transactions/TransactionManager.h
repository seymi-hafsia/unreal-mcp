#pragma once

#include "CoreMinimal.h"

/** Lightweight helper around editor transactions for MCP-driven mutations. */
class UNREALMCP_API FTransactionManager
{
public:
        /** Begins a new transaction if the editor is available. */
        static void Begin(const FString& TransactionName);

        /** Ends the active transaction if the editor is available. */
        static void End();
};
