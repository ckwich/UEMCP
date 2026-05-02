#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

struct FUEMCPOutputLogEntry
{
    uint64 Sequence = 0;
    double TimeSeconds = 0.0;
    FString TimestampUtc;
    FString Category;
    FString Verbosity;
    FString Message;
};

class FUEMCPOutputLogCapture final : public FOutputDevice
{
public:
    static FUEMCPOutputLogCapture& Get();

    void Register();
    void Unregister();

    int32 GetCapacity() const;
    int32 GetCapturedEntryCount() const;
    int32 Query(
        int32 Limit,
        const FString& Category,
        const FString& Verbosity,
        const FString& Contains,
        TArray<FUEMCPOutputLogEntry>& OutEntries
    ) const;

    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category, const double Time) override;
    virtual bool CanBeUsedOnAnyThread() const override;
    virtual bool CanBeUsedOnMultipleThreads() const override;

private:
    static constexpr int32 MaxEntries = 2048;

    mutable FCriticalSection RegistrationMutex;
    mutable FCriticalSection EntriesMutex;
    TArray<FUEMCPOutputLogEntry> Entries;
    uint64 NextSequence = 1;
    bool bRegistered = false;
};
