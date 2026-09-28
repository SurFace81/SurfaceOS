// Unit tests for the key queues of src/kernel/drivers/tty.cpp: one per
// screen, in order, full ones drop, and wait_key's stop condition.
//
// Run with tools/ttytest_host.sh.

#include "../../src/include/drivers/tty.h"
#include "../../src/include/drivers/term.h"

extern "C" int printf(const char*, ...);

// From stubs.cpp
const char* host_echo();
void        host_echo_clear();

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", name);
    if (ok) passed++; else failed++;
}

static keyboard_event_t key(uint8_t code, char ch)
{
    keyboard_event_t e = {};
    e.type    = KEY_PRESS;
    e.KeyCode = code;
    e.KeyChar = ch;
    return e;
}

static void drain(uint32_t screen)
{
    keyboard_event_t e;
    while (tty::pop_key(&e, screen))
        ;
}

static bool same(const char* a, const char* b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static bool stop_always(uint32_t)  { return true; }
static bool stop_never(uint32_t)   { return false; }

int main()
{
    printf("tty key queue unit tests\n\n");
    keyboard_event_t e;

    check("an empty queue has nothing", !tty::pop_key(&e, 0));

    tty::on_key(key(30, 'a'), 0);
    tty::on_key(key(48, 'b'), 0);
    check("keys come out in the order typed",
          tty::pop_key(&e, 0) && e.KeyChar == 'a' && tty::pop_key(&e, 0) && e.KeyChar == 'b' &&
          !tty::pop_key(&e, 0));

    tty::on_key(key(30, 'x'), 2);
    check("a key goes to its screen's queue only",
          !tty::pop_key(&e, 0) && tty::pop_key(&e, 2) && e.KeyChar == 'x');

    tty::on_key(key(30, 'y'), TERM_ALL_SCREENS);
    check("a key for no screen is dropped", !tty::pop_key(&e, TERM_ALL_SCREENS));

    for (int i = 0; i < 1000; i++)
        tty::on_key(key(30, (char)('0' + i % 10)), 1);
    int n = 0;
    while (tty::pop_key(&e, 1))
        n++;
    check("a full queue drops what does not fit", n > 0 && n < 1000);
    tty::on_key(key(30, 'z'), 1);
    check("and takes keys again once read", tty::pop_key(&e, 1) && e.KeyChar == 'z');

    drain(3);
    check("wait_key ends on its stop condition", tty::wait_key(3, stop_always));
    check("and not without a key or it", !tty::wait_key(3, stop_never));
    tty::on_key(key(30, 'k'), 3);
    check("a queued key ends it", tty::wait_key(3, stop_never));

    host_echo_clear();
    tty::write("hi\n", 3);
    check("write goes to the screen", same(host_echo(), "hi\n"));

    printf("\ntty: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
