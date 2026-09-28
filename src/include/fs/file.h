#ifndef FILE_H
#define FILE_H

#include "../cpu/types.h"
#include "vfs.h"
#include "../obj/object.h"

// Open files and their handles.
//
//   handle table (per process)  ->  struct file (an open file, a kernel
//                                    object)  ->  vnode
//
// An open file of a program (SfFile) is a handle: a slot of the process's
// handle table (obj/object.h) holding a File object, with its own offset.
// A process has at most HANDLE_TABLE_SIZE (64) handles.

struct file
{
    kobject  hdr;           // type File; the reference count lives here
    vnode*   vn;            // referenced
    uint64_t offset;
    uint32_t flags;         // O_ACCMODE | O_APPEND (fs/openflags.h)
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

    // --- handles holding a File ---
    // Put f into the lowest free slot. The caller's reference moves into the
    // table - on failure (-EMFILE) it is dropped.
    sint64_t fd_alloc(handle_table* t, file* f, sint32_t* out_fd);
    // fget: borrow the file in slot fd (nullptr + -EBADF if the slot is free
    // or holds something else). The table keeps its reference.
    file* fd_get(handle_table* t, sint32_t fd, sint64_t* rc);
    sint64_t fd_close(handle_table* t, sint32_t fd);
}

#endif // FILE_H
