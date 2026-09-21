// opendir/readdir/closedir over open(O_RDONLY|O_DIRECTORY) + getdents64.
// One 4 KiB record buffer per DIR, refilled on demand. Single-threaded.

#include "../include/dirent.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/stdlib.h"
#include "../include/syscall.h"
#include "../include/abi/syscall.h"
#include "../include/abi/dirent.h"

struct DIR
{
    int      fd;
    uint8_t  buf[4096];
    uint32_t used;      // valid bytes in buf
    uint32_t pos;       // next record offset
    struct dirent cur;  // the record readdir returns
};

DIR* opendir(const char* path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return (DIR*)NULL;

    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (!d)
    {
        close(fd);
        return (DIR*)NULL;
    }
    d->fd = fd;
    d->used = 0;
    d->pos = 0;
    return d;
}

struct dirent* readdir(DIR* d)
{
    if (!d)
        return (struct dirent*)NULL;

    for (;;)
    {
        if (d->pos >= d->used)
        {
            sint64_t n = syscall(SYS_GETDENTS64, (uint64_t)d->fd,
                                 (uint64_t)d->buf, sizeof(d->buf));
            if (n <= 0)
                return (struct dirent*)NULL;    // end (0) or error (-errno)
            d->used = (uint32_t)n;
            d->pos = 0;
        }

        linux_dirent64* de = (linux_dirent64*)(d->buf + d->pos);
        d->pos += de->d_reclen;

        d->cur.d_ino = de->d_ino;
        d->cur.d_off = de->d_off;
        d->cur.d_reclen = de->d_reclen;
        d->cur.d_type = de->d_type;

        uint32_t nlen = 0;
        while (nlen < sizeof(d->cur.d_name) - 1 && de->d_name[nlen])
        {
            d->cur.d_name[nlen] = de->d_name[nlen];
            nlen++;
        }
        d->cur.d_name[nlen] = '\0';
        return &d->cur;
    }
}

int closedir(DIR* d)
{
    if (!d)
        return -1;
    int rc = close(d->fd);
    free(d);
    return rc;
}
