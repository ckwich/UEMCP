#include "Observability/UEMCPOutputLogCapture.h"

#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogVerbosity.h"
#include "Misc/DateTime.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

namespace
{
    ELogVerbosity::Type CleanVerbosity(ELogVerbosity::Type Verbosity)
    {
        return static_cast<ELogVerbosity::Type>(static_cast<uint8>(Verbosity) & ELogVerbosity::VerbosityMask);
    }

    bool MatchesLogFilters(
        const FUEMCPOutputLogEntry& Entry,
        const FString& Category,
        const FString& Verbosity,
        const FString& Contains
    )
    {
        if (!Category.IsEmpty() && !Entry.Category.Equals(Category, ESearchCase::IgnoreCase))
        {
            return false;
        }

        if (!Verbosity.IsEmpty() && !Entry.Verbosity.Equals(Verbosity, ESearchCase::IgnoreCase))
        {
            return false;
        }

        if (!Contains.IsEmpty() && !Entry.Message.Contains(Contains, ESearchCase::IgnoreCase))
        {
            return false;
        }

        return true;
    }
}

FUEMCPOutputLogCapture& FUEMCPOutputLogCapture::Get()
{
    static FUEMCPOutputLogCapture Capture;
    return Capture;
}

void FUEMCPOutputLogCapture::Register()
{
    bool bShouldRegister = false;
    {
        FScopeLock Lock(&RegistrationMutex);
        if (!bRegistered && GLog)
        {
            bRegistered = true;
            bShouldRegister = true;
        }
    }

    if (bShouldRegister)
    {
        GLog->AddOutputDevice(this);
    }
}

void FUEMCPOutputLogCapture::Unregister()
{
    bool bShouldUnregister = false;
    {
        FScopeLock Lock(&RegistrationMutex);
        if (bRegistered)
        {
            bRegistered = false;
            bShouldUnregister = true;
        }
    }

    if (bShouldUnregister && GLog)
    {
        GLog->RemoveOutputDevice(this);
    }
}

int32 FUEMCPOutputLogCapture::GetCapacity() const
{
    return MaxEntries;
}

int32 FUEMCPOutputLogCapture::GetCapturedEntryCount() const
{
    FScopeLock Lock(&EntriesMutex);
    return Entries.Num();
}

int32 FUEMCPOutputLogCapture::Query(
    int32 Limit,
    const FString& Category,
    const FString& Verbosity,
    const FString& Contains,
    TArray<FUEMCPOutputLogEntry>& OutEntries
) const
{
    OutEntries.Reset();
    const int32 ClampedLimit = FMath::Clamp(Limit, 1, 1000);

    TArray<FUEMCPOutputLogEntry> NewestFirstEntries;
    int32 MatchedCount = 0;

    {
        FScopeLock Lock(&EntriesMutex);
        for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
        {
            const FUEMCPOutputLogEntry& Entry = Entries[Index];
            if (!MatchesLogFilters(Entry, Category, Verbosity, Contains))
            {
                continue;
            }

            ++MatchedCount;
            if (NewestFirstEntries.Num() < ClampedLimit)
            {
                NewestFirstEntries.Add(Entry);
            }
        }
    }

    for (int32 Index = NewestFirstEntries.Num() - 1; Index >= 0; --Index)
    {
        OutEntries.Add(NewestFirstEntries[Index]);
    }

    return MatchedCount;
}

void FUEMCPOutputLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
    Serialize(V, Verbosity, Category, FPlatformTime::Seconds() - GStartTime);
}

void FUEMCPOutputLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category, const double Time)
{
    const ELogVerbosity::Type NormalizedVerbosity = CleanVerbosity(Verbosity);
    if (NormalizedVerbosity == ELogVerbosity::NoLogging)
    {
        return;
    }

    FUEMCPOutputLogEntry Entry;
    Entry.TimeSeconds = Time;
    Entry.TimestampUtc = FDateTime::UtcNow().ToIso8601();
    Entry.Category = Category.ToString();
    Entry.Verbosity = ToString(NormalizedVerbosity);
    Entry.Message = V ? FString(V) : FString();

    FScopeLock Lock(&EntriesMutex);
    Entry.Sequence = NextSequence++;
    if (Entries.Num() >= MaxEntries)
    {
        Entries.RemoveAt(0, Entries.Num() - MaxEntries + 1, EAllowShrinking::No);
    }
    Entries.Add(MoveTemp(Entry));
}

bool FUEMCPOutputLogCapture::CanBeUsedOnAnyThread() const
{
    return true;
}

bool FUEMCPOutputLogCapture::CanBeUsedOnMultipleThreads() const
{
    return true;
}
