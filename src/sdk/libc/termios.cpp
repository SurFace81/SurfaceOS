// termios wrappers. The kernel keeps the state; these only marshal it.

#include "../include/termios.h"
#include "../include/unistd.h"

extern "C" {

int tcgetattr(int fd, struct termios* t)
{
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int actions, const struct termios* t)
{
    // TCSADRAIN waits for pending output and TCSAFLUSH also discards
    // pending input. There is no output queue, and the tty drops a
    // half-typed line itself when the discipline changes, so the three
    // requests differ only in name here.
    unsigned long req = TCSETS;
    if (actions == TCSADRAIN) req = TCSETSW;
    else if (actions == TCSAFLUSH) req = TCSETSF;
    return ioctl(fd, req, (void*)t);
}

int tcflush(int fd, int queue)
{
    (void)fd; (void)queue;
    // Nothing is buffered on this side; the tty's input is discarded by a
    // discipline change, which is the only case that matters so far.
    return 0;
}

void cfmakeraw(struct termios* t)
{
    t->c_iflag &= ~(ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN);
    t->c_cflag &= ~CSIZE;
    t->c_cflag |= CS8;
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

}
