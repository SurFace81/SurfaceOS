#ifndef ABI_TERMIOS_H
#define ABI_TERMIOS_H

#include "types.h"

// Linux x86_64 struct termios. Stage 3.5 returns a fixed canonical-mode
// instance from ioctl(TCGETS) so musl's isatty/tcgetattr work; real mode
// switching arrives with termios support in stage 4.

struct termios
{
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

// c_lflag
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define IEXTEN  0100000

// c_iflag
#define IGNBRK  0000001
#define BRKINT  0000002
#define ICRNL   0000400
#define IXON    0002000

// c_oflag
#define OPOST   0000001
#define ONLCR   0000004

// c_cflag
#define CSIZE   0000060
#define CS8     0000040
#define CREAD   0000200

// c_cc indices
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT  15
#define VEOL2   16

// ioctl requests (Linux values)
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

struct winsize
{
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#endif // ABI_TERMIOS_H
