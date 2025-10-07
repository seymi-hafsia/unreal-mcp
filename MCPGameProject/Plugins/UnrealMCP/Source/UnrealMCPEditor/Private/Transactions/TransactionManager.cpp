#include "Transactions/TransactionManager.h"
#include "CoreMinimal.h"

#include "Editor.h"

void FTransactionManager::Begin(const FString& TransactionName)
{
        if (GEditor)
        {
                const FText TransactionText = FText::FromString(TransactionName);
                GEditor->BeginTransaction(TransactionText);
        }
}

void FTransactionManager::End()
{
        if (GEditor)
        {
                GEditor->EndTransaction();
        }
}
