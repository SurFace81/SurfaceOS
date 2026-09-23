// Unit tests for the line discipline and raw-mode encoder in
// src/kernel/drivers/tty.cpp. Run with tools/ttytest_host.sh.

#include "../../src/include/drivers/tty.h"
#include "../../src/sdk/include/abi/errno.h"

extern "C" int printf(const char*, ...);

// From stubs.cpp
const char* host_echo();
void        host_echo_clear();
void        host_set_ms(uint64_t ms);

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", name);
    if (ok) passed++; else failed++;
}

static void section(const char* s) { printf("\n%s\n", s); }

static bool streq(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// --- input helpers ---------------------------------------------------------

static void press(uint8_t code, char ch, bool ctrl = false, bool alt = false,
                  bool shift = false)
{
    keyboard_event_t e = {};
    e.KeyCode = code;
    e.KeyChar = ch;
    e.type    = KEY_PRESS;
    e.Control = ctrl;
    e.Alt     = alt;
    e.Shift   = shift;
    tty::on_key(e);
}

static void type_str(const char* s)
{
    for (; *s; s++)
        press(KEY_A, *s);       // the code only matters for editing keys
}

static void release(uint8_t code)
{
    keyboard_event_t e = {};
    e.KeyCode = code;
    e.type    = KEY_RELEASE;
    tty::on_key(e);
}

// --- termios helpers -------------------------------------------------------

static void set_raw(uint8_t vmin, uint8_t vtime, bool echo = false)
{
    struct termios t;
    tty::get_termios(&t);
    t.c_lflag &= ~(uint32_t)(ICANON | ISIG);
    if (echo) t.c_lflag |= ECHO; else t.c_lflag &= ~(uint32_t)ECHO;
    t.c_cc[VMIN]  = vmin;
    t.c_cc[VTIME] = vtime;
    tty::set_termios(&t);
}

// Read into a NUL-terminated buffer so failures print readably.
static char rbuf[256];

static sint64_t do_read(uint64_t n)
{
    for (uint64_t i = 0; i < sizeof(rbuf); i++) rbuf[i] = 0;
    sint64_t r = tty::read(rbuf, n);
    if (r > 0) rbuf[r] = '\0';
    return r;
}

// Escapes the result so an ESC shows up in a failure message.
static bool read_is(sint64_t r, const char* want)
{
    if (r < 0) return false;
    uint64_t i = 0;
    for (; want[i]; i++)
        if (i >= (uint64_t)r || rbuf[i] != want[i])
            return false;
    return (uint64_t)r == i;
}

static void reset_tty()
{
    tty::reset();
    host_echo_clear();
    host_set_ms(0);
}

// --- canonical mode --------------------------------------------------------

static void t_canonical()
{
    section("canonical line discipline");

    reset_tty();
    type_str("hi");
    press(KEY_ENTER, '\n');
    check("a complete line is served with its newline",
          read_is(do_read(64), "hi\n"));

    reset_tty();
    type_str("hi");
    check("an unterminated line blocks the reader", do_read(64) == -EAGAIN);

    reset_tty();
    type_str("ab");
    press(KEY_BACKSPACE, 8);
    type_str("c");
    press(KEY_ENTER, '\n');
    check("backspace edits the line", read_is(do_read(64), "ac\n"));

    reset_tty();
    type_str("ac");
    press(KEY_ARROW_LEFT, 0);
    type_str("b");
    press(KEY_ENTER, '\n');
    check("the cursor moves and inserts mid-line",
          read_is(do_read(64), "abc\n"));

    reset_tty();
    type_str("junk");
    press(KEY_A, 21, true);             // ^U
    type_str("ok");
    press(KEY_ENTER, '\n');
    check("^U kills the line", read_is(do_read(64), "ok\n"));

    reset_tty();
    type_str("one two");
    press(KEY_A, 23, true);             // ^W
    press(KEY_ENTER, '\n');
    check("^W erases a word", read_is(do_read(64), "one \n"));

    reset_tty();
    press(KEY_A, 4, true);              // ^D on an empty line
    check("^D on an empty line is EOF", do_read(64) == 0);

    reset_tty();
    type_str("tail");
    press(KEY_A, 4, true);
    check("^D after text flushes without a newline",
          read_is(do_read(64), "tail"));

    reset_tty();
    type_str("abcdef");
    press(KEY_ENTER, '\n');
    check("a short read takes a prefix", read_is(do_read(3), "abc"));
    check("the rest stays for the next read", read_is(do_read(64), "def\n"));

    reset_tty();
    press(KEY_DELETE, 0);               // used to insert a literal 0x7F
    type_str("x");
    press(KEY_ENTER, '\n');
    check("non-printable keys stay out of the line",
          read_is(do_read(64), "x\n"));

    reset_tty();
    type_str("hi");
    release(KEY_A);
    press(KEY_ENTER, '\n');
    check("key releases are ignored", read_is(do_read(64), "hi\n"));
}

// --- raw mode --------------------------------------------------------------

static void t_raw()
{
    section("raw mode");

    reset_tty();
    set_raw(1, 0);
    press(KEY_A, 'a');
    check("a plain key is one byte", read_is(do_read(64), "a"));

    reset_tty();
    set_raw(1, 0);
    press(KEY_ARROW_UP, 0);
    check("an arrow key encodes to CSI", read_is(do_read(64), "\033[A"));

    reset_tty();
    set_raw(1, 0);
    press(KEY_F5, 0);
    check("F5 encodes to its tilde form", read_is(do_read(64), "\033[15~"));

    reset_tty();
    set_raw(1, 0);
    press(KEY_ARROW_UP, 0, false, false, true);     // Shift+Up
    check("a modifier adds the xterm parameter",
          read_is(do_read(64), "\033[1;2A"));

    reset_tty();
    set_raw(1, 0);
    press(KEY_ENTER, '\n');
    check("Enter is CR on the wire", read_is(do_read(64), "\r"));

    reset_tty();
    set_raw(1, 0);
    press(KEY_A, 'x', false, true);                 // Alt+x
    check("Alt is an ESC prefix", read_is(do_read(64), "\033x"));

    reset_tty();
    set_raw(1, 0);
    press(KEY_ARROW_UP, 0);
    check("a short buffer takes what fits", read_is(do_read(2), "\033["));
    check("the remainder survives for the next read",
          read_is(do_read(64), "A"));
}

// --- VMIN and VTIME --------------------------------------------------------

static void t_vmin_vtime()
{
    section("VMIN and VTIME");

    reset_tty();
    set_raw(0, 0);
    check("VMIN 0 VTIME 0 with no input is a poll returning 0",
          do_read(64) == 0);

    reset_tty();
    set_raw(0, 0);
    press(KEY_A, 'a');
    check("VMIN 0 VTIME 0 with input returns it", read_is(do_read(64), "a"));

    reset_tty();
    set_raw(2, 0);
    press(KEY_A, 'a');
    check("fewer bytes than VMIN blocks", do_read(64) == -EAGAIN);
    press(KEY_A, 'b');
    check("the held byte is not lost", read_is(do_read(64), "ab"));

    // The regression this whole rewrite exists for: with VMIN > 1 and more
    // than one key queued, the old code copied the bytes out and then
    // "pushed back" more of them than it had taken from the per-key buffer,
    // walking pend_pos below zero and writing outside the array.
    reset_tty();
    set_raw(5, 0);
    press(KEY_ARROW_UP, 0);             // 3 bytes
    press(KEY_A, 'a');                  // 1 byte
    check("a multi-key short read holds everything", do_read(64) == -EAGAIN);
    press(KEY_A, 'b');
    check("and serves it in order once VMIN is met",
          read_is(do_read(64), "\033[Aab"));

    reset_tty();
    set_raw(4, 0);
    press(KEY_A, 'a');
    press(KEY_A, 'b');
    press(KEY_A, 'c');
    press(KEY_A, 'd');
    check("asking for fewer bytes than VMIN still returns",
          read_is(do_read(2), "ab"));

    reset_tty();
    set_raw(0, 1);                      // VTIME 100 ms, no minimum
    check("VTIME arms a deadline instead of returning", do_read(64) == -EAGAIN);
    check("and is not ready before it expires", !tty::readable());
    host_set_ms(150);
    check("the deadline makes the tty readable", tty::readable());
    check("and the read returns empty", do_read(64) == 0);

    reset_tty();
    set_raw(4, 1);
    press(KEY_A, 'a');
    check("VMIN with VTIME waits first", do_read(64) == -EAGAIN);
    host_set_ms(150);
    check("then gives up and returns what it has",
          read_is(do_read(64), "a"));
}

// --- readiness, signals and echo -------------------------------------------

static void t_readable()
{
    section("readiness");

    reset_tty();
    check("an empty canonical tty is not readable", !tty::readable());
    type_str("x");
    check("queued keys make it readable", tty::readable());

    reset_tty();
    set_raw(1, 0);
    check("an empty raw tty is not readable", !tty::readable());
    press(KEY_A, 'a');
    check("a queued key makes it readable", tty::readable());

    // A read that held bytes back must not report itself ready: the reader
    // would wake, find the same too-short queue and spin.
    reset_tty();
    set_raw(4, 0);
    press(KEY_A, 'a');
    (void)do_read(64);
    check("a short read stops reporting ready", !tty::readable());
    press(KEY_A, 'b');
    check("new input makes it ready again", tty::readable());
}

static void t_signals()
{
    section("ISIG keys become signals");

    reset_tty();
    check("no signal to start with", tty::take_signal() == 0);

    press(KEY_A, 3, true);              // ^C
    check("ISIG turns ^C into SIGINT", tty::take_signal() == SIGINT);
    check("and it is taken only once", tty::take_signal() == 0);
    check("it is not queued as input", do_read(64) == -EAGAIN);

    reset_tty();
    press(KEY_A, 28, true);             // ^backslash
    check("^backslash is SIGQUIT", tty::take_signal() == SIGQUIT);

    reset_tty();
    press(KEY_A, 26, true);             // ^Z
    check("^Z is SIGTSTP", tty::take_signal() == SIGTSTP);

    // The flush belongs to the consumer: emptying the line buffer from the
    // keyboard IRQ could land in the middle of a read assembling it.
    reset_tty();
    type_str("typed");
    (void)do_read(64);                  // assembles "typed" into the line
    press(KEY_A, 3, true);
    check("the line survives until the signal is taken",
          tty::line_len() == 5);
    (void)tty::take_signal();
    check("taking the signal flushes what was typed", tty::line_len() == 0);

    reset_tty();
    {
        struct termios t;
        tty::get_termios(&t);
        t.c_lflag |= NOFLSH;
        tty::set_termios(&t);
    }
    type_str("kept");
    (void)do_read(64);
    press(KEY_A, 3, true);
    (void)tty::take_signal();
    check("NOFLSH keeps it", tty::line_len() == 4);

    reset_tty();
    set_raw(1, 0);                      // set_raw clears ISIG, as cfmakeraw does
    press(KEY_A, 3, true);
    check("without ISIG ^C is ordinary input", read_is(do_read(64), "\003"));
    check("and no signal is raised", tty::take_signal() == 0);
}

static void t_emergency_kill()
{
    section("the emergency kill");

    // It has to work through anything an application can do to the tty,
    // which is the whole reason it is not a termios key.
    reset_tty();
    check("nothing to start with", !tty::take_kill());

    keyboard_event_t e = {};
    e.KeyCode = KEY_BACKSPACE;
    e.KeyChar = 8;
    e.type    = KEY_PRESS;
    e.Control = true;
    e.Alt     = true;
    tty::on_key(e);
    check("Ctrl+Alt+Backspace is caught", tty::take_kill());
    check("and reported only once", !tty::take_kill());
    check("it never reaches the application", do_read(64) == -EAGAIN);

    reset_tty();
    set_raw(1, 0);                      // ISIG cleared, everything raw
    tty::on_key(e);
    check("raw mode does not disarm it", tty::take_kill());

    reset_tty();
    press(KEY_BACKSPACE, 8);
    check("a plain backspace is not it", !tty::take_kill());
}

static void t_foreground()
{
    section("the foreground group");

    reset_tty();
    check("a fresh tty has no foreground group", tty::fg_pgrp() == 0);
    tty::set_fg_pgrp(42);
    check("it can be claimed", tty::fg_pgrp() == 42);
    tty::reset();
    check("and a new session releases it", tty::fg_pgrp() == 0);
}

static void t_echo()
{
    section("echo");

    reset_tty();
    type_str("hi");
    press(KEY_ENTER, '\n');
    (void)do_read(64);
    check("canonical mode echoes what was typed",
          streq(host_echo(), "hi\n"));

    reset_tty();
    set_raw(1, 0, true);
    host_echo_clear();
    press(KEY_A, 'a');
    (void)do_read(64);
    check("raw mode echoes printable input", streq(host_echo(), "a"));

    // Echoing the bytes an arrow key encodes to verbatim feeds a real CSI
    // sequence back to the screen, so the cursor moves instead of anything
    // being shown. ECHOCTL is what a terminal does about that.
    reset_tty();
    set_raw(1, 0, true);
    host_echo_clear();
    press(KEY_ARROW_UP, 0);
    (void)do_read(64);
    check("ECHOCTL shows an escape rather than executing it",
          streq(host_echo(), "^[[A"));

    reset_tty();
    set_raw(1, 0, false);
    host_echo_clear();
    press(KEY_A, 'a');
    (void)do_read(64);
    check("ECHO off echoes nothing", streq(host_echo(), ""));
}

static void t_mode_switch()
{
    section("switching discipline");

    // The partial line lives in the tty, and only a read assembles it -
    // so the read that finds nothing is what puts "half" into the buffer.
    reset_tty();
    type_str("half");
    (void)do_read(64);
    set_raw(1, 0);
    check("a half-typed line is dropped, not served raw",
          do_read(64) == -EAGAIN);

    reset_tty();
    set_raw(1, 0);
    press(KEY_A, 'a');
    (void)tty::readable();
    struct termios t;
    tty::get_termios(&t);
    t.c_lflag |= ICANON;
    tty::set_termios(&t);
    check("queued raw bytes are dropped on the way back to canonical",
          do_read(64) == -EAGAIN);
}

int main()
{
    printf("tty unit tests\n");

    t_canonical();
    t_raw();
    t_vmin_vtime();
    t_readable();
    t_signals();
    t_emergency_kill();
    t_foreground();
    t_echo();
    t_mode_switch();

    printf("\ntty: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
