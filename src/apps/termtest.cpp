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

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

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
