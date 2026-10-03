#ifndef SFOS_THREAD_H
#define SFOS_THREAD_H

#include "table.h"

/// A thread's code: SfThread Create runs Entry(Arg), and what it
/// returns ends the thread.
typedef SfStatus (*SfThreadEntry)(void* Arg);

/// Threads: more than one flow of the program running at once, sharing
/// its memory and files.
///
/// SfMain runs on the first thread.
typedef struct SfThread SfThread;

struct SfThread
{
    SfTableHeader Hdr;

    /// Starts Entry(Arg) on a new thread with a stack of its own (256
    /// KiB); *Id refers to it for Join.
    ///
    /// When Entry returns, the thread ends with its SfStatus.
    SfStatus (*Create)(SfThread* This, SfThreadEntry Entry, void* Arg, uint64_t* Id);

    /// Ends the calling thread with Status.
    ///
    /// The last thread to end ends the program with its Status; SfMain
    /// returning ends the program and every thread in it.
    SfStatus (*Exit)(SfThread* This, SfStatus Status);

    /// Waits until thread Id has ended; *Status (when Status is not
    /// null) gets its SfStatus.
    ///
    /// Id is used up: each thread is joined once. SF_BAD_HANDLE for an Id that
    /// is no thread.
    SfStatus (*Join)(SfThread* This, uint64_t Id, SfStatus* Status);
};

#define SF_THREAD_SIGNATURE SF_SIGNATURE('S', 'F', 'T', 'H', 'R', 'E', 'A', 'D')

SF_STATIC_ASSERT(SF_OFFSET_OF(SfThread, Create) == 16, "SfThread layout");
SF_STATIC_ASSERT(sizeof(SfThread) == 40, "SfThread layout");

#endif // SFOS_THREAD_H
