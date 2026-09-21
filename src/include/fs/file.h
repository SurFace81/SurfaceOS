#ifndef FILE_H
#define FILE_H

#include "../cpu/types.h"
#include "vfs.h"

// Open files and descriptor tables (stage 3.4).
//
//   fd_table (per process)  ->  struct file (open file description)
//                                     -> vnode
//
// POSIX semantics: fork and dup share the *file* (and therefore its offset);
// each process owns its *table*. FD_CLOEXEC lives on the table slot and is
// acted on by execve. RLIMIT_NOFILE is FD_TABLE_SIZE (64) for now.

#define FD_TABLE_SIZE   64

struct file
{
    vnode*  vn;             // referenced
    uint64_t offset;
    uint32_t flags;         // O_ACCMODE | O_APPEND | O_NONBLOCK
    uint32_t refcnt;        // number of fd slots pointing here
    uint32_t id;            // small unique id (diagnostics)
    bool    used;
};

struct fd_slot
{
    file*   f;              // null: free
    uint8_t cloexec;
};

struct fd_table
{
    fd_slot slots[FD_TABLE_SIZE];
};

namespace filesys
{
    void init();            // one-time: clear the file pool

    // --- open file descriptions ---
    // Takes a reference on `vn`; releases it at the last close.
    file* file_open(vnode* vn, uint32_t flags);
    void  file_get(file* f);
    void  file_put(file* f);

    // --- fd tables ---
    void  fdtable_init(fd_table* t);
    // Allocate the lowest free slot; -EMFILE when full.
    sint64_t fdtable_alloc(fd_table* t, file* f, bool cloexec, sint32_t* out_fd);
    // fget: borrow the slot's file (nullptr + -EBADF if closed). The caller
    // must NOT unref it through the table - the table owns the reference.
    file* fdtable_get(fd_table* t, sint32_t fd, sint64_t* rc);
    sint64_t fdtable_close(fd_table* t, sint32_t fd);
    // dup: the new slot shares the file (refcnt++). dup2 closes newfd first
    // (silently, POSIX), dup3 adds flags (O_CLOEXEC) and rejects old==new.
    sint64_t fdtable_dup(fd_table* t, sint32_t oldfd, sint32_t newfd,
                         bool cloexec, bool explicit_new, sint32_t* out_fd);
    sint64_t fdtable_getfd(fd_table* t, sint32_t fd);
    sint64_t fdtable_setfd(fd_table* t, sint32_t fd, sint64_t arg);
    // fork: copy every slot, bumping each file's refcnt.
    void  fdtable_fork(fd_table* dst, const fd_table* src);
    // execve: close every CLOEXEC slot.
    void  fdtable_cloexec(fd_table* t);
    // exit: close everything.
    void  fdtable_close_all(fd_table* t);
}

#endif // FILE_H
