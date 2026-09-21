// termtest: terminal checks (stage 4).
//
// Grows with the stage. Right now it covers what stage 4's step 0 unlocked:
// the CP437 upper half, which draw_char used to index with a signed char and
// therefore could never reach. ESC-sequence, colour and termios checks land
// here as the later steps go in.
//
// Screen output is what matters, so this one is judged by a screendump, not
// by uart.log. The bytes still reach the serial log (tty::write mirrors), so
// the harness can at least confirm the app ran to the end.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

// CP437 box drawing. These are exactly the glyphs verified to exist in
// bin/kernel/data/stdfont.fnt.
#define BOX_TL "\xDA"
#define BOX_TR "\xBF"
#define BOX_BL "\xC0"
#define BOX_BR "\xD9"
#define BOX_H  "\xC4"
#define BOX_V  "\xB3"

// `inner` is the number of columns between the two verticals, so every line
// of the frame is inner + 2 wide.
static void hline(const char* left, const char* right, int inner)
{
    print(left);
    for (int i = 0; i < inner; i++)
        print(BOX_H);
    print(right);
    print("\n");
}

static void boxed(const char* text, int inner)
{
    print(BOX_V);
    print(" ");
    print(text);
    int used = 1 + (int)strlen(text);       // the leading space plus the text
    for (int i = used; i < inner; i++)
        print(" ");
    print(BOX_V);
    print("\n");
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    print("termtest: CP437 upper half\n\n");

    // A frame. Before the step-0 fix every one of these bytes indexed the
    // font negatively, so nothing recognisable could appear here.
    const int W = 44;                       // inner width
    hline(BOX_TL, BOX_TR, W);
    boxed("SurfaceOS terminal", W);
    boxed("CP437 box drawing works", W);
    hline(BOX_BL, BOX_BR, W);

    // The whole upper half, 16 per row, so a wrong index shows up as garbage
    // or blanks rather than the expected glyph table.
    print("\n0x80-0xFF:\n");
    char row[17];
    for (int hi = 0x80; hi < 0x100; hi += 16)
    {
        for (int lo = 0; lo < 16; lo++)
            row[lo] = (char)(hi + lo);
        row[16] = '\0';
        print("  ");
        print(row);
        print("\n");
    }

    // Shading and block glyphs: a quick visual gradient.
    print("\nblocks: \xB0\xB1\xB2\xDB  half: \xDC\xDF  dot: \xFE\n");

    print("\ntermtest: done\n");

    // Hold the screen: the session clears it on teardown, and this app is
    // judged by a screendump. Same idiom as memtest/proctest.
    print("press any key...");
    read_key();
    return 0;
}
