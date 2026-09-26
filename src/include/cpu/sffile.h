#ifndef SFFILE_H
#define SFFILE_H

#include "types.h"

// Files through the SurfaceOS SDK: the roots data:/ and tmp:/ (SfFiles)
// and open files (SfFile).
//
// The SDK runtime keeps an SfFile per open file on the program's heap;
// the kernel calls take the file's handle number.
//
// Roots are what the process holds (process::cur_root): data:/ is
// /files/<program name>, created on first start, tmp:/ is /tmp, and argN:
// is the file or folder argument N of the command line names (the console
// opens it). Every open is LOOKUP_BENEATH its root or its directory; a
// root that is a file opens as itself.

struct vnode;
struct user_regs;
struct iret_frame;

namespace sffile
{
    // The roots of a new process of program `name`: referenced
    // directories, nullptr for one that could not be had.
    void open_roots(const char* name, vnode** data, vnode** tmp);

    // Register the SFCALL_FILES_* and SFCALL_FILE_* handlers.
    void init();
}

#endif // SFFILE_H
