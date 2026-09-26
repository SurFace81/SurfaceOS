#ifndef SFFILE_H
#define SFFILE_H

#include "types.h"

// Files through the SurfaceOS SDK: the roots data:/ and tmp:/ (SfFiles)
// and open files (SfFile).
//
// Every process has a read-only page of SfFile tables at USER_SDK_FILES,
// one per handle slot, all alike: the SfFile a program gets for handle h
// is the table in slot h, so the kernel tells the handle from This alone.
// A table stays in place when its handle closes; calls through it then
// fail with SF_BAD_HANDLE (or reach the file that took the slot next).
//
// Roots are directories the process holds (process::cur_root): data:/ is
// /files/<program name>, created on first start, and tmp:/ is /tmp. Every
// open is LOOKUP_BENEATH its root or its directory.

struct vnode;
struct user_regs;
struct iret_frame;

namespace sffile
{
    // The roots of a new process of program `name`: referenced
    // directories, nullptr for one that could not be had.
    void open_roots(const char* name, vnode** data, vnode** tmp);

    // The SfFile of handle h, as the program sees it.
    uint64_t table_address(sint32_t h);

    // Register the SFCALL_FILES_* and SFCALL_FILE_* handlers.
    void init();
}

#endif // SFFILE_H
