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
typedef struct SfMutex SfMutex;
typedef struct SfEvent SfEvent;
typedef struct SfSync  SfSync;

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
};

#define SF_EVENT_AUTO_RESET 0x01ULL
#define SF_WAIT_FOREVER     (~0ULL)

#define SF_SYNC_SIGNATURE   SF_SIGNATURE('S', 'F', 'S', 'Y', 'N', 'C', 0, 0)
#define SF_SYNC_REVISION    SF_REVISION(1, 0)
#define SF_MUTEX_SIGNATURE  SF_SIGNATURE('S', 'F', 'M', 'U', 'T', 'E', 'X', 0)
#define SF_MUTEX_REVISION   SF_REVISION(1, 0)
#define SF_EVENT_SIGNATURE  SF_SIGNATURE('S', 'F', 'E', 'V', 'E', 'N', 'T', 0)
#define SF_EVENT_REVISION   SF_REVISION(1, 0)

SF_STATIC_ASSERT(sizeof(SfMutex) == 40, "SfMutex layout");
SF_STATIC_ASSERT(sizeof(SfEvent) == 48, "SfEvent layout");
SF_STATIC_ASSERT(sizeof(SfSync) == 32, "SfSync layout");

#endif // SFOS_SYNC_H
