#pragma once

#include "CoreMinimal.h"

/**
 * Dynamic buffer for handling large payloads with automatic resizing
 */
class UNREALMCP_API FMCPDynamicBuffer
{
public:
    FMCPDynamicBuffer(int32 InitialCapacity = 65536);
    ~FMCPDynamicBuffer();

    /**
     * Append data to the buffer
     * @param Data The data to append
     * @param Size Size of the data in bytes
     */
    void Append(const uint8* Data, int32 Size);

    /**
     * Append string to the buffer
     * @param String The string to append
     */
    void AppendString(const FString& String);

    /**
     * Get the raw buffer data
     */
    const uint8* GetData() const { return Buffer; }

    /**
     * Get current size of data in buffer
     */
    int32 GetSize() const { return DataSize; }

    /**
     * Get buffer capacity
     */
    int32 GetCapacity() const { return Capacity; }

    /**
     * Convert buffer to string
     */
    FString ToString() const;

    /**
     * Clear the buffer
     */
    void Clear();

    /**
     * Reset the buffer (clears and resets to initial capacity)
     */
    void Reset();

    /**
     * Check if buffer contains complete message (ends with newline)
     */
    bool HasCompleteMessage() const;

    /**
     * Extract complete messages from buffer (separated by newlines)
     * @param OutMessages Array to store extracted messages
     * @return Number of messages extracted
     */
    int32 ExtractMessages(TArray<FString>& OutMessages);

    /**
     * Reserve space in buffer
     * @param NewCapacity Minimum capacity to reserve
     */
    void Reserve(int32 NewCapacity);

private:
    uint8* Buffer;
    int32 DataSize;
    int32 Capacity;
    int32 InitialCapacity;

    void Resize(int32 NewCapacity);
};
