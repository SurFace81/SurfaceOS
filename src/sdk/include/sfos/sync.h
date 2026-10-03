#ifndef SFOS_SYNC_H
#define SFOS_SYNC_H

#include "table.h"

// Synchronisation between the threads of a program.
//
//   CreateMutex  *Out gets a new mutex, unlocked.
//   CreateEvent  *Out gets a new event, not set. With SF_EVENT_AUTO_RESET
//                it resets itself each time a Wait on it succeeds, so one
//                Set lets exactly one waiter through; without, it stays set
//                until Reset.
//
// A mutex lets one thread at a time through:
//
//   Lock    wait until no other thread holds the mutex, then hold it.
//   Unlock  let go of it; the thread that locked it unlocks it.
//   Close   the mutex is not used any more; the pointer is invalid.
//
// An event is a flag threads can wait for:
//
//   Set     raise it: every waiter goes on (auto-reset: one).
//   Reset   lower it.
//   Wait    wait until it is set, at most TimeoutMs milliseconds
//           (SF_WAIT_FOREVER: no limit, 0: just look). SF_TIMEOUT when
//           the time ran out.
//   Close   the event is not used any more; the pointer is invalid.
//
// Waiting for whichever comes first (revision 1.1):
//
//   WaitAny  wait until one of Count items (at most SF_WAIT_MAX_ITEMS) is
//            ready, at most TimeoutMs milliseconds (as Wait). *Index (when
//            Index is not null) gets the first ready one; an auto-reset event among them is used
//            up only when it is that one. SF_TIMEOUT when the time ran out
//            - with no items, WaitAny just sleeps. SF_BAD_HANDLE for an
//            item that is not what its Kind says.
//
//            SF_WAIT_EVENT    Event is set.
//            SF_WAIT_PROCESS  the program behind Handle (sfos/process.h,
//                             Start) has ended. Handle stays for Wait.
//            SF_WAIT_THREAD   thread Handle (sfos/thread.h, Create) has
//                             ended. Handle stays for Join.
//            SF_WAIT_KEY      a key is there for this program: ReadKey
//                             (sfos/console.h) gets it without waiting.
typedef struct SfMutex SfMutex;
typedef struct SfEvent SfEvent;
typedef struct SfSync  SfSync;

typedef struct SfWaitItem
{
    uint64_t Kind;              // SF_WAIT_*
    uint64_t Handle;            // SF_WAIT_PROCESS, SF_WAIT_THREAD
    SfEvent* Event;             // SF_WAIT_EVENT
} SfWaitItem;

struct SfMutex
{
    SfTableHeader Hdr;
    SfStatus (*Lock)(SfMutex* This);
    SfStatus (*Unlock)(SfMutex* This);
    SfStatus (*Close)(SfMutex* This);
};

struct SfEvent
{
    SfTableHeader Hdr;
    SfStatus (*Set)(SfEvent* This);
    SfStatus (*Reset)(SfEvent* This);
    SfStatus (*Wait)(SfEvent* This, uint64_t TimeoutMs);
    SfStatus (*Close)(SfEvent* This);
};

struct SfSync
{
    SfTableHeader Hdr;
    SfStatus (*CreateMutex)(SfSync* This, SfMutex** Out);
    SfStatus (*CreateEvent)(SfSync* This, uint64_t Flags, SfEvent** Out);
    SfStatus (*WaitAny)(SfSync* This, uint64_t Count, const SfWaitItem* Items,
                        uint64_t TimeoutMs, uint64_t* Index);
};

#define SF_EVENT_AUTO_RESET 0x01ULL
#define SF_WAIT_FOREVER     (~0ULL)

#define SF_WAIT_EVENT       1
#define SF_WAIT_PROCESS     2
#define SF_WAIT_THREAD      3
#define SF_WAIT_KEY         4
#define SF_WAIT_MAX_ITEMS   16

#define SF_SYNC_SIGNATURE   SF_SIGNATURE('S', 'F', 'S', 'Y', 'N', 'C', 0, 0)
#define SF_SYNC_REVISION    SF_REVISION(1, 1)
#define SF_MUTEX_SIGNATURE  SF_SIGNATURE('S', 'F', 'M', 'U', 'T', 'E', 'X', 0)
#define SF_MUTEX_REVISION   SF_REVISION(1, 0)
#define SF_EVENT_SIGNATURE  SF_SIGNATURE('S', 'F', 'E', 'V', 'E', 'N', 'T', 0)
#define SF_EVENT_REVISION   SF_REVISION(1, 0)

SF_STATIC_ASSERT(sizeof(SfMutex) == 40, "SfMutex layout");
SF_STATIC_ASSERT(sizeof(SfEvent) == 48, "SfEvent layout");
SF_STATIC_ASSERT(sizeof(SfWaitItem) == 24, "SfWaitItem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSync, WaitAny) == 32, "SfSync layout");
SF_STATIC_ASSERT(sizeof(SfSync) == 40, "SfSync layout");

#endif // SFOS_SYNC_H
