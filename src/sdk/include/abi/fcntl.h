#ifndef ABI_FCNTL_H
#define ABI_FCNTL_H

// Linux x86_64 open(2)/fcntl(2)/faccessat(2) constants. Values are the
// kernel ABI; do not renumber.

// Access mode (mask O_ACCMODE over flags)
#define O_RDONLY        00000000
#define O_WRONLY        00000001
#define O_RDWR          00000002
#define O_ACCMODE       00000003

// Creation / behaviour
#define O_CREAT         00000100
#define O_EXCL          00000200
#define O_NOCTTY        00000400    // accepted, ignored (no ctty yet)
#define O_TRUNC         00001000
#define O_APPEND        00002000
#define O_NONBLOCK      00004000
#define O_DIRECTORY     00200000    // fail if not a directory
#define O_NOFOLLOW      00400000    // accepted, ignored (no symlinks)
#define O_CLOEXEC       02000000

// lseek whence (Linux puts these in unistd.h; the SDK re-exports them)
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

// *at() dirfd
#define AT_FDCWD        (-100)
#define AT_SYMLINK_NOFOLLOW 0x100   // accepted, ignored
#define AT_REMOVEDIR    0x200
#define AT_EMPTY_PATH   0x1000      // accepted by fstatat with an fd

// fcntl commands
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

#define FD_CLOEXEC      1

// renameat2 flags
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2

#endif // ABI_FCNTL_H
