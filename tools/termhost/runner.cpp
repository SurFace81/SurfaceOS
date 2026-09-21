// Unit tests for the escape parser in src/kernel/drivers/term.cpp.
//
// Run with tools/termtest_host.sh. These take milliseconds, which is the
// difference between debugging a 30-command state machine comfortably and
// doing it through a rebuild-and-boot cycle.

#include "../../src/include/drivers/term.h"

extern "C" int printf(const char*, ...);

// From stubs.cpp
void    snapshot();
uint8_t cell_ch(uint32_t x, uint32_t y);
uint8_t cell_fg(uint32_t x, uint32_t y);
uint8_t cell_bg(uint32_t x, uint32_t y);
uint8_t cell_attr(uint32_t x, uint32_t y);
void    feed_str(const char* s);
void    row_text(uint32_t y, char* out, uint32_t n);
bool    host_csi_u();

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", name);
    if (ok) passed++; else failed++;
}

// Compare a row against expected text, ignoring the blank tail.
static bool row_is(uint32_t y, const char* want)
{
    char buf[256];
    row_text(y, buf, sizeof(buf));
    uint32_t i = 0;
    for (; want[i]; i++)
        if (buf[i] != want[i])
            return false;
    for (; buf[i]; i++)
        if (buf[i] != ' ')
            return false;
    return true;
}

static void show_row(uint32_t y)
{
    char buf[256];
    row_text(y, buf, sizeof(buf));
    printf("        row %u: \"%s\"\n", y, buf);
}

static void reset(uint32_t cols, uint32_t rows)
{
    term::resize(cols, rows);
    feed_str("\033c");          // RIS: full reset
    term::resize(cols, rows);
}

static void section(const char* s) { printf("\n%s\n", s); }

// ---------------------------------------------------------------------------

static void t_plain()
{
    section("plain text and control characters");
    reset(20, 5);

    feed_str("hello");
    snapshot();
    check("text lands on row 0", row_is(0, "hello"));
    check("cursor follows the text", term::cursor_x() == 5 && term::cursor_y() == 0);

    feed_str("\r\nworld");
    snapshot();
    check("CR LF starts a new row", row_is(1, "world"));

    feed_str("\b\b");
    snapshot();
    check("backspace erases destructively", row_is(1, "wor"));

    reset(20, 5);
    feed_str("abcdefghijklmnopqrstuvwxyz");
    snapshot();
    check("a long line wraps at the width",
          row_is(0, "abcdefghijklmnopqrst") && row_is(1, "uvwxyz"));
}

static void t_cursor()
{
    section("cursor movement");
    reset(20, 5);

    feed_str("\033[3;5H");
    check("CUP is 1-based", term::cursor_x() == 4 && term::cursor_y() == 2);

    feed_str("\033[A\033[A");
    check("CUU moves up", term::cursor_y() == 0);

    feed_str("\033[2B");
    check("CUD takes a count", term::cursor_y() == 2);

    feed_str("\033[10C");
    check("CUF takes a count", term::cursor_x() == 14);

    feed_str("\033[100C");
    check("CUF clamps at the right edge", term::cursor_x() == 19);

    feed_str("\033[100D");
    check("CUB clamps at the left edge", term::cursor_x() == 0);

    feed_str("\033[H");
    check("CUP with no parameters homes", term::cursor_x() == 0 && term::cursor_y() == 0);

    feed_str("\033[7G");
    check("CHA sets the column", term::cursor_x() == 6);

    feed_str("\033[4d");
    check("VPA sets the row", term::cursor_y() == 3);

    // Out-of-range targets are clamped, not wrapped.
    feed_str("\033[99;99H");
    check("CUP clamps to the screen", term::cursor_x() == 19 && term::cursor_y() == 4);
}

static void t_erase()
{
    section("erase in line and display");
    reset(10, 3);

    feed_str("abcdefghij");
    feed_str("\033[1;4H\033[K");     // erase from column 4 to end of line
    snapshot();
    check("EL 0 clears to end of line", row_is(0, "abc"));

    reset(10, 3);
    feed_str("abcdefghij\033[1;4H\033[1K");
    snapshot();
    check("EL 1 clears to start of line", row_is(0, "    efghij"));

    reset(10, 3);
    feed_str("abcdefghij\033[1;4H\033[2K");
    snapshot();
    check("EL 2 clears the whole line", row_is(0, ""));

    reset(10, 3);
    feed_str("aaa\r\nbbb\r\nccc");
    feed_str("\033[2;2H\033[J");     // from the middle of row 1 to the end
    snapshot();
    check("ED 0 clears downwards",
          row_is(0, "aaa") && row_is(1, "b") && row_is(2, ""));

    reset(10, 3);
    feed_str("aaa\r\nbbb\r\nccc\033[2;2H\033[1J");
    snapshot();
    check("ED 1 clears upwards",
          row_is(0, "") && row_is(1, "  b") && row_is(2, "ccc"));

    reset(10, 3);
    feed_str("aaa\r\nbbb\r\nccc\033[2J");
    snapshot();
    check("ED 2 clears everything",
          row_is(0, "") && row_is(1, "") && row_is(2, ""));
}

static void t_sgr()
{
    section("SGR colours and attributes");
    reset(20, 3);

    feed_str("\033[31mR\033[32mG\033[0mN");
    snapshot();
    check("foreground colours apply per cell",
          cell_fg(0, 0) == TERM_RED && cell_fg(1, 0) == TERM_GREEN);
    check("SGR 0 restores the default", cell_fg(2, 0) == TERM_DEFAULT_FG);

    reset(20, 3);
    feed_str("\033[44mB");
    snapshot();
    check("background colours apply", cell_bg(0, 0) == TERM_BLUE);

    reset(20, 3);
    feed_str("\033[1mb\033[22mn");
    snapshot();
    check("bold sets and clears",
          (cell_attr(0, 0) & TERM_BOLD) && !(cell_attr(1, 0) & TERM_BOLD));

    reset(20, 3);
    feed_str("\033[7mr\033[27mn");
    snapshot();
    check("reverse sets and clears",
          (cell_attr(0, 0) & TERM_REVERSE) && !(cell_attr(1, 0) & TERM_REVERSE));

    reset(20, 3);
    feed_str("\033[91mX");
    snapshot();
    check("bright foreground (90-97)", cell_fg(0, 0) == TERM_RED + 8);

    reset(20, 3);
    feed_str("\033[33;44;1mX");
    snapshot();
    check("several parameters in one SGR",
          cell_fg(0, 0) == TERM_YELLOW && cell_bg(0, 0) == TERM_BLUE &&
          (cell_attr(0, 0) & TERM_BOLD));

    reset(20, 3);
    feed_str("\033[31m\033[mX");
    snapshot();
    check("bare ESC[m is a reset", cell_fg(0, 0) == TERM_DEFAULT_FG);
}

static void t_lines()
{
    section("insert and delete lines and characters");
    reset(10, 4);

    feed_str("aaa\r\nbbb\r\nccc\r\nddd");
    feed_str("\033[2;1H\033[L");
    snapshot();
    check("IL pushes rows down",
          row_is(0, "aaa") && row_is(1, "") && row_is(2, "bbb") && row_is(3, "ccc"));

    reset(10, 4);
    feed_str("aaa\r\nbbb\r\nccc\r\nddd\033[2;1H\033[M");
    snapshot();
    check("DL pulls rows up",
          row_is(0, "aaa") && row_is(1, "ccc") && row_is(2, "ddd") && row_is(3, ""));

    reset(10, 4);
    feed_str("abcdef\033[1;3H\033[P");
    snapshot();
    check("DCH deletes characters in place", row_is(0, "abdef"));

    reset(10, 4);
    feed_str("abcdef\033[1;3H\033[2@");
    snapshot();
    check("ICH inserts blanks in place", row_is(0, "ab  cdef"));

    reset(10, 4);
    feed_str("abcdef\033[1;3H\033[2X");
    snapshot();
    check("ECH blanks in place without shifting", row_is(0, "ab  ef"));
}

static void t_scroll()
{
    section("scrolling and the scroll region");
    reset(10, 4);

    feed_str("aaa\r\nbbb\r\nccc\r\nddd\r\neee");
    snapshot();
    check("output past the last row scrolls the screen",
          row_is(0, "bbb") && row_is(3, "eee"));

    // Confine scrolling to rows 2-3, then overflow inside it.
    reset(10, 4);
    feed_str("aaa\r\nbbb\r\nccc\r\nddd");
    feed_str("\033[2;3r");           // region = rows 2..3 (1-based)
    check("DECSTBM homes the cursor to the region",
          term::cursor_x() == 0 && term::cursor_y() == 1);
    feed_str("\033[3;1H\033[2KX\r\nY");
    snapshot();
    check("scrolling stays inside the region",
          row_is(1, "X") && row_is(2, "Y"));
    check("rows outside the region are untouched",
          row_is(0, "aaa") && row_is(3, "ddd"));

    reset(10, 4);
    feed_str("aaa\r\nbbb\r\nccc\r\nddd\033[S");
    snapshot();
    check("SU scrolls the whole screen up", row_is(0, "bbb") && row_is(3, ""));

    reset(10, 4);
    feed_str("aaa\r\nbbb\r\nccc\r\nddd\033[T");
    snapshot();
    check("SD scrolls the whole screen down", row_is(0, "") && row_is(1, "aaa"));

    // Reverse index at the top of the screen scrolls down.
    reset(10, 4);
    feed_str("aaa\r\nbbb\033[1;1H\033M");
    snapshot();
    check("RI at the top scrolls down", row_is(0, "") && row_is(1, "aaa"));
}

static void t_save_restore()
{
    section("save and restore cursor");
    reset(20, 4);

    feed_str("\033[2;5H\033[31m\0337");   // DECSC
    feed_str("\033[1;1H\033[32mX");
    feed_str("\0338Y");                   // DECRC
    snapshot();
    check("DECRC restores the position", cell_ch(4, 1) == 'Y');
    check("DECRC restores the colour too", cell_fg(4, 1) == TERM_RED);
}

static void t_alt_screen()
{
    section("alternate screen");
    reset(10, 3);

    feed_str("main");
    feed_str("\033[?1049h");
    snapshot();
    check("the alternate screen starts empty", row_is(0, ""));

    feed_str("alt");
    snapshot();
    check("writes land on the alternate screen", row_is(0, "alt"));

    feed_str("\033[?1049l");
    snapshot();
    check("leaving restores the main screen", row_is(0, "main"));
}

static void t_robustness()
{
    section("split, malformed and hostile sequences");

    // A sequence cut in half between two writes: exactly what BOUNCE_SIZE
    // chunking does to a long app write.
    reset(20, 3);
    feed_str("\033[3");
    feed_str("1mX");
    snapshot();
    check("a sequence split across two writes still applies",
          cell_fg(0, 0) == TERM_RED);

    reset(20, 3);
    feed_str("\033");
    feed_str("[");
    feed_str("2");
    feed_str(";");
    feed_str("3");
    feed_str("H");
    feed_str("Z");
    snapshot();
    check("one byte at a time works too", cell_ch(2, 1) == 'Z');

    // Unknown final byte: consumed, not printed.
    reset(20, 3);
    feed_str("\033[5zX");
    snapshot();
    check("an unknown command is swallowed, not printed", row_is(0, "X"));

    // More parameters than the parser stores.
    reset(20, 3);
    feed_str("\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20mX");
    snapshot();
    check("a parameter overflow does not corrupt anything", row_is(0, "X"));

    // A very long numeric parameter must clamp rather than wrap around.
    reset(20, 3);
    // No trailing text here: writing a glyph at the last column would wrap
    // the cursor and hide what this is actually measuring.
    feed_str("\033[99999999999999999999C");
    check("a huge parameter clamps to the edge", term::cursor_x() == 19);

    // An aborted escape returns to ground without eating the next text.
    reset(20, 3);
    feed_str("\033[1\033[0mhi");
    snapshot();
    check("an escape inside an escape recovers", row_is(0, "hi"));

    // The CSI u toggle is an input-side setting the terminal only relays.
    reset(20, 3);
    feed_str("\033[>1u");
    check("ESC [ > 1 u enables full-fidelity keys", host_csi_u());
    feed_str("\033[<u");
    check("ESC [ < u disables them again", !host_csi_u());

    // Without a private marker the same final byte is restore-cursor.
    reset(20, 3);
    feed_str("\033[2;5H\0337\033[1;1H\033[uZ");
    snapshot();
    check("ESC [ u with no marker is still restore-cursor",
          cell_ch(4, 1) == 'Z' && !host_csi_u());

    // OSC strings are swallowed up to BEL.
    reset(20, 3);
    feed_str("\033]0;a window title\007ok");
    snapshot();
    check("OSC strings are swallowed", row_is(0, "ok"));

    // C0 controls that mean nothing must not become glyphs.
    reset(20, 3);
    feed_str("a\001\002b");
    snapshot();
    check("unhandled C0 controls are ignored", row_is(0, "ab"));

    // The upper half must survive as-is: it is CP437, not a control range.
    reset(20, 3);
    const char box[] = { (char)0xDA, (char)0xC4, (char)0xBF, 0 };
    feed_str(box);
    snapshot();
    check("bytes >= 0x80 are stored verbatim",
          cell_ch(0, 0) == 0xDA && cell_ch(1, 0) == 0xC4 && cell_ch(2, 0) == 0xBF);
}

int main()
{
    printf("term parser unit tests\n");

    if (!term::init(200, 60))
    {
        printf("term::init failed\n");
        return 1;
    }

    t_plain();
    t_cursor();
    t_erase();
    t_sgr();
    t_lines();
    t_scroll();
    t_save_restore();
    t_alt_screen();
    t_robustness();

    printf("\nterm: %d passed, %d failed\n", passed, failed);
    if (failed)
    {
        printf("\n(rows of the last failing case)\n");
        for (uint32_t y = 0; y < 4; y++)
            show_row(y);
    }
    return failed ? 1 : 0;
}
