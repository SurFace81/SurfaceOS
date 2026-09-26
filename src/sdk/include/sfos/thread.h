#ifndef SFOS_THREAD_H
#define SFOS_THREAD_H

#include "table.h"

// Threads: more than one flow of the program running at once, sharing its
// memory and files. SfMain runs on the first thread.
//
//   Create  start Entry(Arg) on a new thread with a stack of its own
//           (256 KiB); *Id refers to it for Join. When Entry returns, the
//           thread ends with its SfStatus.
//   Exit    end the calling thread with Status. The last thread to end
//           ends the program with its Status; SfMain returning ends the
//           program and every thread in it.
//   Join    wait until thread Id has ended; *Status (when Status is not
//           null) gets its SfStatus. Id is used up: each thread is joined
//           once. SF_BAD_HANDLE for an Id that is no thread.
typedef SfStatus (*SfThreadEntry)(void* Arg);

typedef struct SfThread SfThread;

struct SfThread
{
    SfTableHeader Hdr;
    SfStatus (*Create)(SfThread* This, SfThreadEntry Entry, void* Arg, uint64_t* Id);
    SfStatus (*Exit)(SfThread* This, SfStatus Status);
    SfStatus (*Join)(SfThread* This, uint64_t Id, SfStatus* Status);
};

#define SF_THREAD_SIGNATURE SF_SIGNATURE('S', 'F', 'T', 'H', 'R', 'E', 'A', 'D')
#define SF_THREAD_REVISION  SF_REVISION(1, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfThread, Create) == 16, "SfThread layout");
SF_STATIC_ASSERT(sizeof(SfThread) == 40, "SfThread layout");

#endif // SFOS_THREAD_H
