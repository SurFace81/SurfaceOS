#ifndef FILE_H
#define FILE_H

#include "../cpu/types.h"
#include "vfs.h"
#include "../obj/object.h"

// Open files and descriptors (stage 3.4; roadmap stage 3.2).
//
//   handle table (per process)  ->  struct file (open file description,
//                                    a kernel object)  ->  vnode
//
// A file descriptor is a handle: slot fd of the process's handle table
// (obj/object.h) holding a File object. POSIX semantics stay as they were:
// fork and dup share the *file* (and therefore its offset); each process
// owns its *table*. FD_CLOEXEC is the slot's HANDLE_CLOEXEC flag and is
// acted on by execve. RLIMIT_NOFILE is HANDLE_TABLE_SIZE (64).

struct file
{
    kobject  hdr;           // type File; the reference count lives here
    vnode*   vn;            // referenced
    uint64_t offset;
    uint32_t flags;         // O_ACCMODE | O_APPEND | O_NONBLOCK
    uint32_t id;            // small unique id (diagnostics)
    bool     used;          // pool slot taken
};

namespace filesys
{
    void init();            // one-time: clear the file pool

    // --- open file descriptions ---
    // Takes a reference on `vn`; releases it when the last reference to the
    // file goes. The caller holds the file's first reference.
    file* file_open(vnode* vn, uint32_t flags);
    // Any open file description still pointing into this mount? umount asks
    // through the VFS busy hook.
    bool  any_open_on(const mount* m);
    void  file_get(file* f);
    void  file_put(file* f);

    // --- descriptors (handles holding a File) ---
    // Put f into the lowest free slot. The caller's reference moves into the
    // table - on failure (-EMFILE) it is dropped.
    sint64_t fd_alloc(handle_table* t, file* f, bool cloexec, sint32_t* out_fd);
    // fget: borrow the file in slot fd (nullptr + -EBADF if the slot is free
    // or holds something else). The table keeps its reference.
    file* fd_get(handle_table* t, sint32_t fd, sint64_t* rc);
    sint64_t fd_close(handle_table* t, sint32_t fd);
    // Lowest free slot >= minfd, or -1 when the table is full. F_DUPFD needs
    // this: fd_dup with an explicit newfd *takes* an occupied slot, so the
    // free one has to be found before asking for the dup.
    sint32_t fd_lowest_free(const handle_table* t, sint32_t minfd);
    // dup: the new slot shares the object. dup2 closes newfd first
    // (silently, POSIX), dup3 adds flags (O_CLOEXEC) and rejects old==new.
    // On success *out_fd is always set, including dup2's old==new no-op.
    sint64_t fd_dup(handle_table* t, sint32_t oldfd, sint32_t newfd,
                    bool cloexec, bool explicit_new, sint32_t* out_fd);
    // F_GETFD / F_SETFD: FD_CLOEXEC <-> HANDLE_CLOEXEC.
    sint64_t fd_getfd(handle_table* t, sint32_t fd);
    sint64_t fd_setfd(handle_table* t, sint32_t fd, sint64_t arg);
}

#endif // FILE_H
