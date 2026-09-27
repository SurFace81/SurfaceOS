#ifndef FS_OPENFLAGS_H
#define FS_OPENFLAGS_H

// How a file is opened (struct file flags, fileio.cpp) and renamed
// (vnode_ops rename).

// Access mode (mask O_ACCMODE over the flags)
#define O_RDONLY        00000000
#define O_WRONLY        00000001
#define O_RDWR          00000002
#define O_ACCMODE       00000003

#define O_CREAT         00000100    // create it when it is missing
#define O_EXCL          00000200    // with O_CREAT: fail when it exists
#define O_TRUNC         00001000    // empty it
#define O_APPEND        00002000    // every write goes to the end
#define O_DIRECTORY     00200000    // fail if not a directory

// rename flags
#define RENAME_NOREPLACE 1          // fail when the new name exists
#define RENAME_EXCHANGE  2          // swap the two

#endif // FS_OPENFLAGS_H
