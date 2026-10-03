#ifndef SFOS_SYNC_H
#define SFOS_SYNC_H

#include "table.h"

/// A mutex: lets one thread at a time through.
typedef struct SfMutex SfMutex;

struct SfMutex
{
    SfTableHeader Hdr;

    /// Waits until no other thread holds the mutex, then holds it.
    SfStatus (*Lock)(SfMutex* This);

    /// Lets go of the mutex; the thread that locked it unlocks it.
    SfStatus (*Unlock)(SfMutex* This);

    /// The mutex is not used any more; the pointer is invalid
    /// afterwards.
    SfStatus (*Close)(SfMutex* This);
};

/// An event: a flag threads can wait for.
typedef struct SfEvent SfEvent;

struct SfEvent
{
    SfTableHeader Hdr;

    /// Raises the event: every waiter goes on (auto-reset: one).
    SfStatus (*Set)(SfEvent* This);

    /// Lowers the event.
    SfStatus (*Reset)(SfEvent* This);

    /// Waits until the event is set, at most TimeoutMs milliseconds
    /// (SF_WAIT_FOREVER: no limit, 0: just look).
    ///
    /// SF_TIMEOUT when the time ran out.
    SfStatus (*Wait)(SfEvent* This, uint64_t TimeoutMs);

    /// The event is not used any more; the pointer is invalid
    /// afterwards.
    SfStatus (*Close)(SfEvent* This);
};

/// One thing for WaitAny to wait for.
typedef struct SfWaitItem
{
    uint64_t Kind;              ///< SF_WAIT_*
    uint64_t Handle;            ///< SF_WAIT_PROCESS, SF_WAIT_THREAD
    SfEvent* Event;             ///< SF_WAIT_EVENT
} SfWaitItem;

/// Synchronisation between the threads of a program, and waiting for
/// whichever of several things comes first.
typedef struct SfSync SfSync;

struct SfSync
{
    SfTableHeader Hdr;

    /// *Out gets a new mutex, unlocked.
    SfStatus (*CreateMutex)(SfSync* This, SfMutex** Out);

    /// *Out gets a new event, not set.
    ///
    /// With SF_EVENT_AUTO_RESET it resets itself each time a Wait on it
    /// succeeds, so one Set lets exactly one waiter through; without, it stays
    /// set until Reset.
    SfStatus (*CreateEvent)(SfSync* This, uint64_t Flags, SfEvent** Out);

    /// Waits until one of Count Items (at most SF_WAIT_MAX_ITEMS) is
    /// ready, at most TimeoutMs milliseconds (as SfEvent Wait).
    ///
    /// *Index (when Index is not null) gets the first ready one; an auto-reset
    /// event among them is used up only when it is that one. SF_TIMEOUT when
    /// the time ran out - with no items, WaitAny just sleeps. SF_BAD_HANDLE
    /// for an item that is not what its Kind says.
    SfStatus (*WaitAny)(SfSync* This, uint64_t Count, const SfWaitItem* Items,
                        uint64_t TimeoutMs, uint64_t* Index);
};

/// CreateEvent: the event resets itself when a Wait on it succeeds.
#define SF_EVENT_AUTO_RESET 0x01ULL
/// A timeout of no limit.
#define SF_WAIT_FOREVER     (~0ULL)

/// SfWaitItem Kind: Event is set.
#define SF_WAIT_EVENT       1
/// SfWaitItem Kind: the program behind Handle (sfos/process.h, Start)
/// has ended.
///
/// Handle stays for Wait.
#define SF_WAIT_PROCESS     2
/// SfWaitItem Kind: thread Handle (sfos/thread.h, Create) has ended.
///
/// Handle stays for Join.
#define SF_WAIT_THREAD      3
/// SfWaitItem Kind: a key is there for this program; ReadKey
/// (sfos/console.h) gets it without waiting.
#define SF_WAIT_KEY         4
/// The most items WaitAny takes.
#define SF_WAIT_MAX_ITEMS   16

#define SF_SYNC_SIGNATURE   SF_SIGNATURE('S', 'F', 'S', 'Y', 'N', 'C', 0, 0)
#define SF_MUTEX_SIGNATURE  SF_SIGNATURE('S', 'F', 'M', 'U', 'T', 'E', 'X', 0)
#define SF_EVENT_SIGNATURE  SF_SIGNATURE('S', 'F', 'E', 'V', 'E', 'N', 'T', 0)

SF_STATIC_ASSERT(sizeof(SfMutex) == 40, "SfMutex layout");
SF_STATIC_ASSERT(sizeof(SfEvent) == 48, "SfEvent layout");
SF_STATIC_ASSERT(sizeof(SfWaitItem) == 24, "SfWaitItem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSync, WaitAny) == 32, "SfSync layout");
SF_STATIC_ASSERT(sizeof(SfSync) == 40, "SfSync layout");

#endif // SFOS_SYNC_H
