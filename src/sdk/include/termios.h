#ifndef TERMIOS_H
#define TERMIOS_H

#include <abi/termios.h>

// tcsetattr's `actions` argument. There is no output queue to drain and no
// input queue distinct from the tty's own, so all three behave alike; they
// exist because portable code passes them.
#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

// tcflush's queue selector.
#define TCIFLUSH    0
#define TCOFLUSH    1
#define TCIOFLUSH   2

#ifdef __cplusplus
extern "C" {
#endif

int  tcgetattr(int fd, struct termios* t);
int  tcsetattr(int fd, int actions, const struct termios* t);
int  tcflush(int fd, int queue);

// Turn `t` into the usual raw configuration: no canonical line editing, no
// echo, no signal keys, one byte satisfies a read.
void cfmakeraw(struct termios* t);

#ifdef __cplusplus
}
#endif

#endif // TERMIOS_H
