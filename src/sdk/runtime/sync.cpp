// Synchronisation: the runtime's lock, and the SfMutex and SfEvent objects
// programs get from SfSync.

#include "runtime.h"

// --- SdkLock ---------------------------------------------------------------
// Count is how many threads hold or want the lock. The one that takes it
// from 0 to 1 holds it; everybody after sleeps on the event. Letting go
// with others still counted sets the event, which wakes exactly one of
// them (auto-reset) - or, if none sleeps yet, lets the next one straight
// through.

SfStatus LockInit(SdkLock* Lock)
{
    Lock->Count = 0;
    return SfCall(SFCALL_EVENT_CREATE, SF_EVENT_AUTO_RESET, (uint64_t)&Lock->Event);
}

void LockAcquire(SdkLock* Lock)
{
    if (__atomic_add_fetch(&Lock->Count, 1, __ATOMIC_ACQ_REL) == 1)
        return;
    // Only a signal cuts the wait short, and the program stops or ends on
    // its way back: wait again after a resume.
    while (SF_ERROR(SfCall(SFCALL_WAIT, Lock->Event, SF_WAIT_FOREVER)))
        ;
}

void LockRelease(SdkLock* Lock)
{
    if (__atomic_sub_fetch(&Lock->Count, 1, __ATOMIC_ACQ_REL) > 0)
        SfCall(SFCALL_EVENT_SET, Lock->Event);
}

// --- SfMutex -----------------------------------------------------------------

struct MutexObject
{
    SfMutex Public;
    SdkLock Lock;
};

static SfStatus MutexLock(SfMutex* This)
{
    LockAcquire(&((MutexObject*)This)->Lock);
    return SF_SUCCESS;
}

static SfStatus MutexUnlock(SfMutex* This)
{
    LockRelease(&((MutexObject*)This)->Lock);
    return SF_SUCCESS;
}

static SfStatus MutexClose(SfMutex* This)
{
    SfStatus Status = SfCall(SFCALL_CLOSE, ((MutexObject*)This)->Lock.Event);
    MemoryFree((SfMemory*)&SdkMemory, This);
    return Status;
}

static const SfMutex MutexTable =
{
    { SF_MUTEX_SIGNATURE, SF_MUTEX_REVISION, sizeof(SfMutex) },
    MutexLock,
    MutexUnlock,
    MutexClose,
};

SfStatus SyncCreateMutex(SfSync*, SfMutex** Out)
{
    if (!Out)
        return SF_INVALID_PARAMETER;
    MutexObject* Mutex = nullptr;
    SfStatus Status = MemoryAllocate((SfMemory*)&SdkMemory, sizeof(MutexObject), (void**)&Mutex);
    if (SF_ERROR(Status))
        return Status;
    Status = LockInit(&Mutex->Lock);
    if (SF_ERROR(Status))
    {
        MemoryFree((SfMemory*)&SdkMemory, Mutex);
        return Status;
    }
    Mutex->Public = MutexTable;
    *Out = &Mutex->Public;
    return SF_SUCCESS;
}

// --- SfEvent -----------------------------------------------------------------

struct EventObject
{
    SfEvent  Public;
    uint64_t Handle;
};

static uint64_t Handle(SfEvent* This)
{
    return ((EventObject*)This)->Handle;
}

static SfStatus EventSet(SfEvent* This)
{
    return SfCall(SFCALL_EVENT_SET, Handle(This));
}

static SfStatus EventReset(SfEvent* This)
{
    return SfCall(SFCALL_EVENT_RESET, Handle(This));
}

static SfStatus EventWait(SfEvent* This, uint64_t TimeoutMs)
{
    return SfCall(SFCALL_WAIT, Handle(This), TimeoutMs);
}

static SfStatus EventClose(SfEvent* This)
{
    SfStatus Status = SfCall(SFCALL_CLOSE, Handle(This));
    MemoryFree((SfMemory*)&SdkMemory, This);
    return Status;
}

static const SfEvent EventTable =
{
    { SF_EVENT_SIGNATURE, SF_EVENT_REVISION, sizeof(SfEvent) },
    EventSet,
    EventReset,
    EventWait,
    EventClose,
};

SfStatus SyncCreateEvent(SfSync*, uint64_t Flags, SfEvent** Out)
{
    if (!Out)
        return SF_INVALID_PARAMETER;
    EventObject* Event = nullptr;
    SfStatus Status = MemoryAllocate((SfMemory*)&SdkMemory, sizeof(EventObject), (void**)&Event);
    if (SF_ERROR(Status))
        return Status;
    Status = SfCall(SFCALL_EVENT_CREATE, Flags, (uint64_t)&Event->Handle);
    if (SF_ERROR(Status))
    {
        MemoryFree((SfMemory*)&SdkMemory, Event);
        return Status;
    }
    Event->Public = EventTable;
    *Out = &Event->Public;
    return SF_SUCCESS;
}
