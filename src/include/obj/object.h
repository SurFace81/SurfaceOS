#ifndef OBJECT_H
#define OBJECT_H

#include "../cpu/types.h"

// Kernel objects and handle tables (roadmap stage 3).
//
// A kernel object is anything a process can hold a handle to: an open file,
// a directory, a process, an event. Each one starts with a kobject header -
// its operations (type, name, destroy) and a reference count - and is freed
// by its own destroy() when the last reference goes.
//
// A handle table belongs to a process: slot i holds a referenced object,
// and i is the handle the process uses (an SfFile's, a Wait's, a mutex's).
//
// This file and object.cpp depend on nothing else in the kernel, so they
// are unit-tested on the host (tools/objtest_host.sh). Single CPU, no
// locking: tables are only touched from syscall context.

enum class obj_type : uint8_t
{
    None = 0,       // get(): any type
    File,           // open file description
    Process,
    Event,
    Thread,
};

struct kobject;
struct wait_queue;

struct kobject_ops
{
    obj_type    type;
    const char* name;                   // "file", ... for diagnostics
    void      (*destroy)(kobject* o);   // the last reference is gone

    // Waitable objects (obj/event.h: objects::wait). signaled() says whether
    // a wait on the object ends now, waitq() is where waiters sleep until it
    // does, and consume() - optional - runs once a wait has succeeded (an
    // auto-reset event clears itself there). All null: not waitable.
    bool        (*signaled)(kobject* o);
    wait_queue* (*waitq)(kobject* o);
    void        (*consume)(kobject* o);
};

struct kobject
{
    const kobject_ops* ops;
    uint32_t           refcnt;
};

namespace kobj
{
    // Set up the header; the creator holds the first reference.
    void     init(kobject* o, const kobject_ops* ops);
    void     get(kobject* o);
    // Drop a reference; destroy() runs when it was the last one.
    void     put(kobject* o);
    obj_type type(const kobject* o);
}

#define HANDLE_TABLE_SIZE   64

struct handle_slot
{
    kobject* obj;       // referenced; null: free
};

struct handle_table
{
    handle_slot slots[HANDLE_TABLE_SIZE];
};

namespace handles
{
    void init(handle_table* t);

    // Put `o` into the lowest free slot, taking a new reference. 0 and *out
    // set, or -EMFILE when no slot is free.
    sint64_t install(handle_table* t, kobject* o, sint32_t* out);

    // Borrow the object in slot h: no reference is taken, the table keeps
    // its own. `want` None accepts any type. nullptr with *rc = -EBADF when
    // the slot is free or out of range, or holds another type.
    kobject* get(const handle_table* t, sint32_t h, obj_type want, sint64_t* rc);

    // Release slot h. 0 or -EBADF.
    sint64_t close(handle_table* t, sint32_t h);

    // Close everything (exit).
    void close_all(handle_table* t);
}

#endif // OBJECT_H
