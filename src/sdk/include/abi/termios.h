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
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define FLUSHO  0010000
#define PENDIN  0040000
#define EXTPROC 0200000

// c_iflag
#define IGNBRK  0000001
#define BRKINT  0000002
#define ICRNL   0000400
#define IXON    0002000
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define IUCLC   0001000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

// c_oflag
#define OPOST   0000001
#define ONLCR   0000004
#define OLCUC   0000002
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040
#define OFILL   0000100
#define OFDEL   0000200

// c_cflag
#define CSIZE   0000060
#define CS8     0000040
#define CREAD   0000200
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CSTOPB  0000100
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000
#define CBAUD   0010017
#define B0      0000000
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017

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
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define FIONREAD    0x541B

struct winsize
{
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#endif // ABI_TERMIOS_H
