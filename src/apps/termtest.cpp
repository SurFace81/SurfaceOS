// termtest: what the terminal can do (stage 4).
//
// Judged by a screendump, not by uart.log - the bytes reach the serial log
// either way, but only the panel shows whether they meant anything. The
// parser itself is unit-tested on the host by tools/termtest_host.sh; this
// is the end-to-end check that the same sequences survive the write path,
// devfs and the cell grid.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <abi/termios.h>

// CP437 box drawing, verified to exist in bin/kernel/data/stdfont.fnt.
#define TL "\xC9"
#define TR "\xBB"
#define BL "\xC8"
#define BR "\xBC"
#define HZ "\xCD"
#define VT "\xBA"

static void esc(const char* s) { print(s); }

// ESC [ <row> ; <col> H, 1-based.
static void at(int row, int col)
{
    char buf[24];
    int n = 0;
    buf[n++] = '\033';
    buf[n++] = '[';
    if (row >= 10) buf[n++] = (char)('0' + row / 10);
    buf[n++] = (char)('0' + row % 10);
    buf[n++] = ';';
    if (col >= 10) buf[n++] = (char)('0' + col / 10);
    buf[n++] = (char)('0' + col % 10);
    buf[n++] = 'H';
    buf[n] = '\0';
    print(buf);
}

static void sgr(int code)
{
    char buf[12];
    int n = 0;
    buf[n++] = '\033';
    buf[n++] = '[';
    if (code >= 100)     { buf[n++] = (char)('0' + code / 100);
                           buf[n++] = (char)('0' + (code / 10) % 10); }
    else if (code >= 10) { buf[n++] = (char)('0' + code / 10); }
    buf[n++] = (char)('0' + code % 10);
    buf[n++] = 'm';
    buf[n] = '\0';
    print(buf);
}

// A framed box drawn by absolute cell addressing - no assumptions about
// where the cursor happened to be.
static void box(int row, int col, int w, int h, const char* title)
{
    at(row, col);
    print(TL);
    for (int i = 0; i < w - 2; i++) print(HZ);
    print(TR);

    for (int y = 1; y < h - 1; y++)
    {
        at(row + y, col);
        print(VT);
        for (int i = 0; i < w - 2; i++) print(" ");
        print(VT);
    }

    at(row + h - 1, col);
    print(BL);
    for (int i = 0; i < w - 2; i++) print(HZ);
    print(BR);

    if (title && *title)
    {
        at(row, col + 2);
        sgr(1);
        print(" ");
        print(title);
        print(" ");
        sgr(22);
    }
}

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    print(ok ? "  [ ok ] " : "  [FAIL] ");
    print(name);
    print("\n");
    if (ok) passed++; else failed++;
}

// The parts that can be asserted rather than looked at: termios round-trip,
// raw mode, and a window size that reflects the real viewport.
static void checks()
{
    print("termtest: termios and winsize\n");

    struct termios t0;
    check("tcgetattr on the tty", tcgetattr(0, &t0) == 0);
    check("the tty starts canonical with echo",
          (t0.c_lflag & ICANON) && (t0.c_lflag & ECHO));
    check("isatty agrees", isatty(0) == 1);

    struct winsize w;
    check("TIOCGWINSZ succeeds", ioctl(0, TIOCGWINSZ, &w) == 0);
    check("the window has a plausible size",
          w.ws_row >= 10 && w.ws_row < 300 && w.ws_col >= 40 && w.ws_col < 500);
    check("the pixel size matches the cell grid",
          w.ws_xpixel >= w.ws_col && w.ws_ypixel >= w.ws_row);
    // 25x80 was the hardcoded answer before the viewport was consulted; a
    // session runs under a title bar, so it cannot legitimately be exactly
    // that on this panel.
    check("the size is not the old hardcoded 25x80",
          !(w.ws_row == 25 && w.ws_col == 80));

    struct termios raw = t0;
    cfmakeraw(&raw);
    check("tcsetattr accepts raw mode", tcsetattr(0, TCSANOW, &raw) == 0);

    struct termios back;
    check("tcgetattr reads the new settings back", tcgetattr(0, &back) == 0);
    check("raw mode cleared ICANON, ECHO and ISIG",
          !(back.c_lflag & ICANON) && !(back.c_lflag & ECHO) &&
          !(back.c_lflag & ISIG));
    check("raw mode set VMIN=1 VTIME=0",
          back.c_cc[VMIN] == 1 && back.c_cc[VTIME] == 0);

    // A non-blocking raw read of an idle tty must come back empty rather
    // than hang: VMIN 0 with VTIME 0 is a pure poll.
    struct termios poll_mode = raw;
    poll_mode.c_cc[VMIN]  = 0;
    poll_mode.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &poll_mode);
    char c;
    check("VMIN=0 VTIME=0 polls instead of blocking", read(0, &c, 1) == 0);

    check("tcsetattr restores the original settings",
          tcsetattr(0, TCSANOW, &t0) == 0);
    check("... and it reads back canonical again",
          tcgetattr(0, &back) == 0 && (back.c_lflag & ICANON));

    print("\ntermtest: ");
    print_i64(passed);
    print(" passed, ");
    print_i64(failed);
    print(" failed\n");
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    checks();

    esc("\033[2J\033[H");

    // --- colours -----------------------------------------------------------
    at(1, 1);
    print("SGR foreground:");
    for (int c = 30; c <= 37; c++) { sgr(c); print(" \xDB\xDB"); }
    sgr(0);

    at(2, 1);
    print("SGR bright:    ");
    for (int c = 90; c <= 97; c++) { sgr(c); print(" \xDB\xDB"); }
    sgr(0);

    at(3, 1);
    print("SGR background:");
    for (int c = 40; c <= 47; c++) { sgr(c); print("   "); }
    sgr(0);

    at(4, 1);
    sgr(7); print(" reverse video "); sgr(0);
    print("  ");
    sgr(1); print("bold"); sgr(0);

    // --- absolute addressing and box drawing -------------------------------
    box(6, 2, 34, 7, "CP437 frame");
    at(7, 5);  sgr(36); print("cursor addressing works"); sgr(0);
    at(8, 5);  sgr(33); print("colour is per cell");      sgr(0);
    at(9, 5);  sgr(32); print("the grid remembers it");   sgr(0);

    box(6, 40, 28, 7, "nested");
    at(7, 43); print("drawn after the first");
    at(8, 43); print("box, at its own");
    at(9, 43); print("coordinates");

    // --- erase and delete --------------------------------------------------
    at(13, 1);
    print("erase/delete:  XXXXXXXXXXXXXXXXXXXXXXXX");
    at(13, 20);
    esc("\033[K");
    print("<- EL cleared from here");

    at(14, 1);
    print("abcdefghij");
    at(14, 4);
    esc("\033[3P");
    at(14, 20);
    print("<- DCH removed \"def\"");

    // --- the CP437 upper half ----------------------------------------------
    at(16, 1);
    print("CP437 0x80-0xFF:");
    char row[17];
    int line = 17;
    for (int hi = 0x80; hi < 0x100; hi += 16)
    {
        for (int lo = 0; lo < 16; lo++)
            row[lo] = (char)(hi + lo);
        row[16] = '\0';
        at(line, 3);
        print(row);
        line++;
    }

    at(26, 1);
    print("termtest: done");

    // Hold the screen: the session clears it on teardown, and this app is
    // judged by a screendump. Same idiom as memtest/proctest.
    print("  press any key...");
    read_key();

    esc("\033[0m");
    return 0;
}
