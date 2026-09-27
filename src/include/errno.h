#ifndef ERRNO_H
#define ERRNO_H

// The kernel's own error codes: the file system and the layers below it
// return them negated (-ENOENT); the calls of the SDK turn them into an
// SfStatus before a program sees them (sffile.cpp).

#define EPERM            1   // Operation not permitted
#define ENOENT           2   // No such file or directory
#define ESRCH            3   // No such process
#define EINTR            4   // Interrupted system call
#define EIO              5   // I/O error
#define ENXIO            6   // No such device or address
#define E2BIG            7   // Argument list too long
#define ENOEXEC          8   // Exec format error
#define EBADF            9   // Bad file descriptor
#define EAGAIN          11   // Try again
#define ENOMEM          12   // Out of memory
#define EACCES          13   // Permission denied
#define EFAULT          14   // Bad address
#define EBUSY           16   // Device or resource busy
#define EEXIST          17   // File exists
#define EXDEV           18   // Cross-device link
#define ENOTDIR         20   // Not a directory
#define EISDIR          21   // Is a directory
#define EINVAL          22   // Invalid argument
#define ENFILE          23   // File table overflow
#define EMFILE          24   // Too many open files
#define EFBIG           27   // File too large
#define ENOSPC          28   // No space left on device
#define EROFS           30   // Read-only file system
#define ERANGE          34   // Math result not representable
#define ENAMETOOLONG    36   // File name too long
#define ENOTEMPTY       39   // Directory not empty
#define ETIMEDOUT      110

#endif // ERRNO_H
