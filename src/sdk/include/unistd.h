#ifndef SFOS_UNISTD_H
#define SFOS_UNISTD_H

#include "abi/types.h"
#include "abi/process.h"
#include "abi/fcntl.h"      // SEEK_*

// --- POSIX file I/O (Linux syscall semantics, errno on -1) ----------------

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
ssize_t pread(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);

int     dup(int oldfd);
int     dup2(int oldfd, int newfd);

int     unlink(const char* path);
int     rmdir(const char* path);
int     rename(const char* oldpath, const char* newpath);
int     access(const char* path, int mode);    // F_OK/R_OK/W_OK/X_OK
int     chdir(const char* path);
int     fchdir(int fd);
char*   getcwd(char* buf, size_t size);
int     fsync(int fd);
void    sync();
int     truncate(const char* path, off_t length);
int     ftruncate(int fd, off_t length);
int     isatty(int fd);
int     dup3(int oldfd, int newfd, int flags);
int     ioctl(int fd, unsigned long request, ...);

// access() modes
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// --- processes ------------------------------------------------------------

pid_t    getpid();
pid_t    getppid();

// Duplicate the calling process. Returns the child's pid in the parent, 0 in
// the child, -1 on failure. Memory is copied, not shared.
pid_t    fork();

// Replace the current program. argv/envp are NULL-terminated; they may be
// NULL. Returns only on failure (-1, errno set); the old program keeps
// running.
int      execv(const char* path, char* const argv[]);
int      execve(const char* path, char* const argv[], char* const envp[]);

// Wait for a child (pid > 0) or any child (pid == -1). *status receives the
// Linux wait(2) status: decode with WIFEXITED/WEXITSTATUS and
// WIFSIGNALED/WTERMSIG (abi/process.h). With WNOHANG returns 0 if no child
// has finished yet. -1 if there is nothing to wait for.
pid_t    waitpid(pid_t pid, int* status, int options);

// Send a signal, with the POSIX pid conventions:
//   pid > 0   that process
//   pid == 0  every process in the caller's group
//   pid == -1 every process the caller may signal
//   pid < -1  every process in group -pid
// sig 0 delivers nothing and only reports whether the target exists.
int      kill(pid_t pid, int sig);

// Process groups (job control).
int      setpgid(pid_t pid, pid_t pgid);
pid_t    getpgid(pid_t pid);
pid_t    getpgrp();
pid_t    setsid();

// Which group owns the terminal on `fd`. A shell moves it to the job it
// puts in the foreground, so ^C and keyboard input follow.
pid_t    tcgetpgrp(int fd);
int      tcsetpgrp(int fd, pid_t pgid);

// No users yet: these are honest constants, not failures, because a
// program that gets -1 from getuid() tends to give up entirely.
uid_t    getuid();
uid_t    geteuid();
gid_t    getgid();
gid_t    getegid();

void     yield();
void     sleep_ms(uint32_t ms);

#endif
