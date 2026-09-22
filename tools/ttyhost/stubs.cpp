// Host-side stubs for what tty.cpp needs from the kernel, plus a recorder
// for everything it echoes. Built by tools/ttytest_host.sh.
//
// Same trick as tools/termhost: the line discipline and the raw-mode
// encoder are pure logic over a keyboard-event ring, so they can be
// exercised on the host instead of through a build-and-boot cycle.

#include "../../src/include/drivers/screen.h"
#include "../../src/include/drivers/pit.h"
#include "../../src/include/drivers/uart.h"
#include "../../src/include/mm/memory.h"

extern "C" int printf(const char*, ...);

// --- what the tty echoed ---------------------------------------------------

const uint32_t ECHO_MAX = 4096;

static char     echoed[ECHO_MAX];
static uint32_t echoed_len = 0;
static uint64_t fake_ms = 0;

namespace screen
{
    void write(const char* s, uint64_t len)
    {
        for (uint64_t i = 0; i < len && echoed_len < ECHO_MAX - 1; i++)
            echoed[echoed_len++] = s[i];
        echoed[echoed_len] = '\0';
    }

    void write(const char* s)
    {
        uint64_t n = 0;
        while (s[n]) n++;
        write(s, n);
    }
}

namespace pit
{
    uint64_t uptime_ms() { return fake_ms; }
}

namespace uart
{
    void printf(const char*, ...) {}
    void write(const char*, uint64_t) {}
}

namespace memory
{
    void memcpy(uint8_t* dst, const uint8_t* src, uint64_t size)
    {
        if (dst < src)
            for (uint64_t i = 0; i < size; i++) dst[i] = src[i];
        else
            for (uint64_t i = size; i-- > 0; ) dst[i] = src[i];
    }

    void memcpy(uint8_t* dst, uint8_t* src, uint64_t size)
    {
        memcpy(dst, (const uint8_t*)src, size);
    }

    void memset(uint8_t* addr, char value, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++) addr[i] = (uint8_t)value;
    }
}

// --- helpers the runner uses ----------------------------------------------

const char* host_echo()      { return echoed; }
void        host_echo_clear() { echoed_len = 0; echoed[0] = '\0'; }
void        host_set_ms(uint64_t ms) { fake_ms = ms; }
uint64_t    host_ms()        { return fake_ms; }
