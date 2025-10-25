#include "MCPDynamicBuffer.h"
#include "UnrealMCPModule.h"

FMCPDynamicBuffer::FMCPDynamicBuffer(int32 InInitialCapacity)
    : Buffer(nullptr)
    , DataSize(0)
    , Capacity(0)
    , InitialCapacity(InInitialCapacity)
{
    Reserve(InitialCapacity);
}

FMCPDynamicBuffer::~FMCPDynamicBuffer()
{
    if (Buffer)
    {
        FMemory::Free(Buffer);
        Buffer = nullptr;
    }
}

void FMCPDynamicBuffer::Append(const uint8* Data, int32 Size)
{
    if (!Data || Size <= 0)
    {
        return;
    }

    // Ensure we have enough space
    if (DataSize + Size > Capacity)
    {
        // Grow by 1.5x or enough to fit new data, whichever is larger
        int32 NewCapacity = FMath::Max(Capacity + (Capacity / 2), DataSize + Size);
        Resize(NewCapacity);
    }

    // Copy data
    FMemory::Memcpy(Buffer + DataSize, Data, Size);
    DataSize += Size;
}

void FMCPDynamicBuffer::AppendString(const FString& String)
{
    FTCHARToUTF8 Converted(*String);
    Append((const uint8*)Converted.Get(), Converted.Length());
}

FString FMCPDynamicBuffer::ToString() const
{
    if (DataSize == 0)
    {
        return FString();
    }

    // Create a null-terminated copy for conversion
    TArray<uint8> NullTerminated;
    NullTerminated.SetNum(DataSize + 1);
    FMemory::Memcpy(NullTerminated.GetData(), Buffer, DataSize);
    NullTerminated[DataSize] = 0;

    return UTF8_TO_TCHAR(NullTerminated.GetData());
}

void FMCPDynamicBuffer::Clear()
{
    DataSize = 0;
}

void FMCPDynamicBuffer::Reset()
{
    Clear();
    if (Capacity > InitialCapacity * 2)
    {
        // Shrink back to initial capacity if we grew too much
        Resize(InitialCapacity);
    }
}

bool FMCPDynamicBuffer::HasCompleteMessage() const
{
    if (DataSize == 0)
    {
        return false;
    }

    // Check if buffer contains a newline
    for (int32 i = 0; i < DataSize; ++i)
    {
        if (Buffer[i] == '\n')
        {
            return true;
        }
    }

    return false;
}

int32 FMCPDynamicBuffer::ExtractMessages(TArray<FString>& OutMessages)
{
    OutMessages.Reset();

    if (!HasCompleteMessage())
    {
        return 0;
    }

    // Convert buffer to string
    FString AllData = ToString();

    // Split by newlines (CullEmpty = false to preserve empty parts)
    TArray<FString> Parts;
    AllData.ParseIntoArray(Parts, TEXT("\n"), false);

    // Extract complete messages (all but the last part)
    // The last part is either empty (if message ended with \n) or incomplete
    int32 NumMessages = Parts.Num() - 1;
    for (int32 i = 0; i < NumMessages; ++i)
    {
        if (!Parts[i].IsEmpty())
        {
            OutMessages.Add(Parts[i]);
        }
    }

    // Keep the incomplete message in buffer
    Clear();
    if (Parts.Num() > 0 && !Parts.Last().IsEmpty())
    {
        AppendString(Parts.Last());
    }

    return OutMessages.Num();
}

void FMCPDynamicBuffer::Reserve(int32 NewCapacity)
{
    if (NewCapacity > Capacity)
    {
        Resize(NewCapacity);
    }
}

void FMCPDynamicBuffer::Resize(int32 NewCapacity)
{
    uint8* NewBuffer = (uint8*)FMemory::Malloc(NewCapacity);

    if (Buffer && DataSize > 0)
    {
        // Copy existing data
        FMemory::Memcpy(NewBuffer, Buffer, FMath::Min(DataSize, NewCapacity));
        FMemory::Free(Buffer);
    }

    Buffer = NewBuffer;
    Capacity = NewCapacity;

    // Clamp data size if we shrunk
    if (DataSize > Capacity)
    {
        DataSize = Capacity;
        UE_LOG(LogUnrealMCP, Warning, TEXT("Buffer data truncated during resize"));
    }
}
