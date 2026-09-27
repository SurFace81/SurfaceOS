// Unit tests for the screens' text in src/kernel/drivers/term.cpp: text,
// control characters, wrapping, scrolling, cells and screens.
//
// Run with tools/termtest_host.sh. These take milliseconds instead of a
// rebuild-and-boot cycle.

#include "../../src/include/drivers/term.h"

extern "C" int printf(const char*, ...);

// From stubs.cpp
void    snapshot();
uint8_t cell_ch(uint32_t x, uint32_t y);
uint8_t cell_fg(uint32_t x, uint32_t y);
uint8_t cell_bg(uint32_t x, uint32_t y);
void    feed_str(const char* s);
void    row_text(uint32_t y, char* out, uint32_t n);

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
    term::select(0);
    term::resize(cols, rows);
    term::set_colors(TERM_DEFAULT_FG, TERM_DEFAULT_BG);
    term::clear();
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
    check("the cursor follows the text", term::cursor_x() == 5 && term::cursor_y() == 0);

    feed_str("\r\nworld");
    snapshot();
    check("CR LF starts a new row", row_is(1, "world"));

    feed_str("\b\b");
    snapshot();
    check("backspace erases", row_is(1, "wor"));

    feed_str("\n!");
    snapshot();
    check("LF alone starts the next row at its start", row_is(2, "!"));

    reset(20, 5);
    feed_str("abcdefghijklmnopqrstuvwxyz");
    snapshot();
    check("a long line wraps at the width",
          row_is(0, "abcdefghijklmnopqrst") && row_is(1, "uvwxyz"));

    reset(20, 3);
    feed_str("a\001\002\033b");
    snapshot();
    check("other control characters, ESC too, are left out", row_is(0, "ab"));

    reset(20, 3);
    const char box[] = { (char)0xDA, (char)0xC4, (char)0xBF, 0 };
    feed_str(box);
    snapshot();
    check("bytes >= 0x80 (code page 437) are stored as they are",
          cell_ch(0, 0) == 0xDA && cell_ch(1, 0) == 0xC4 && cell_ch(2, 0) == 0xBF);
}

static void t_scroll()
{
    section("scrolling");
    reset(10, 4);

    feed_str("aaa\r\nbbb\r\nccc\r\nddd\r\neee");
    snapshot();
    check("output past the last row scrolls the screen",
          row_is(0, "bbb") && row_is(3, "eee"));

    reset(10, 4);
    feed_str("aaa\r\nbbb\r\nccc\r\nddd");
    term::scroll_up();
    snapshot();
    check("scroll_up moves everything up a row",
          row_is(0, "bbb") && row_is(3, "") && term::cursor_y() == 3);
}

static void t_cells()
{
    section("cells, colours and the cursor");
    reset(20, 5);

    term::set_colors(TERM_RED, TERM_BLUE);
    feed_str("X");
    snapshot();
    check("text takes the colours set", cell_fg(0, 0) == TERM_RED && cell_bg(0, 0) == TERM_BLUE);

    term::put_cell(5, 2, 'Q', TERM_GREEN, TERM_BLACK);
    snapshot();
    check("put_cell writes one cell", cell_ch(5, 2) == 'Q' && cell_fg(5, 2) == TERM_GREEN);

    term::write_at(17, 3, "abcdef", 6);
    snapshot();
    check("write_at is cut at the edge", row_is(3, "                 abc"));

    term::set_cursor(99, 99);
    check("set_cursor clamps to the screen", term::cursor_x() == 19 && term::cursor_y() == 4);

    term::clear();
    snapshot();
    check("clear blanks the screen and homes the cursor",
          row_is(0, "") && term::cursor_x() == 0 && term::cursor_y() == 0);
}

static void t_screens()
{
    section("screens");
    reset(20, 3);
    feed_str("one");
    term::select(1);
    term::clear();
    feed_str("two");
    term::select(0);
    term::show(0);
    snapshot();
    check("each screen has its own text", row_is(0, "one"));
    term::show(1);
    snapshot();
    check("the shown screen is what is drawn", row_is(0, "two"));
    term::show(0);

    sint32_t h = term::open_hidden();
    check("a hidden screen opens", h >= (sint32_t)TERM_SCREENS);
    term::copy_screen(0, (uint32_t)h);
    term::select((uint32_t)h);
    check("copy_screen takes the cursor along", term::cursor_x() == 3);
    feed_str("!");
    term::copy_screen((uint32_t)h, 1);
    term::select(0);
    term::show(1);
    snapshot();
    check("and the cells", row_is(0, "one!"));
    term::show(0);
    term::close_hidden((uint32_t)h);
    check("a closed hidden screen is free again", term::open_hidden() == h);
    term::close_hidden((uint32_t)h);
}

int main()
{
    printf("term unit tests\n");

    if (!term::init(200, 60))
    {
        printf("term::init failed\n");
        return 1;
    }

    t_plain();
    t_scroll();
    t_cells();
    t_screens();

    printf("\nterm: %d passed, %d failed\n", passed, failed);
    if (failed)
    {
        printf("\n(rows of the last failing case)\n");
        for (uint32_t y = 0; y < 4; y++)
            show_row(y);
    }
    return failed ? 1 : 0;
}
