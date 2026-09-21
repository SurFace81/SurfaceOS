// Host-side stubs for the four things term.cpp needs from the kernel, plus
// a recorder for what it rasterises. Built by tools/termtest_host.sh.
//
// This is the whole point of splitting term out of screen: the parser has
// no kernel dependencies worth the name, so it can be exercised on the host
// in milliseconds instead of through a build-and-boot cycle.

#include "../../src/include/drivers/screen.h"
#include "../../src/include/drivers/term.h"
#include "../../src/include/drivers/pit.h"
#include "../../src/include/drivers/uart.h"
#include "../../src/include/mm/heap.h"
#include "../../src/include/mm/memory.h"

extern "C" void* malloc(unsigned long);
extern "C" void  free(void*);
extern "C" int   printf(const char*, ...);

// --- what the terminal drew ------------------------------------------------

const uint32_t REC_MAX = 256;

struct rec_cell { uint8_t ch, fg, bg, attr; };
static rec_cell recorded[REC_MAX][REC_MAX];
static uint64_t fake_ms = 0;

namespace screen
{
    void draw_cell(uint32_t col, uint32_t row, uint8_t ch,
                   uint8_t fg, uint8_t bg, uint8_t attr)
    {
        if (col >= REC_MAX || row >= REC_MAX)
            return;
        recorded[row][col].ch = ch;
        recorded[row][col].fg = fg;
        recorded[row][col].bg = bg;
        recorded[row][col].attr = attr;
    }

    // The cursor is an XOR of the cell under it; for the recorder it is
    // enough that it is a no-op, the tests look at the grid contents.
    void invert_cell(uint32_t, uint32_t) {}
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
        // The terminal copies whole rows around; they never overlap, but
        // copy defensively anyway so a future caller cannot be bitten.
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

void* kmalloc(size_t size) { return malloc((unsigned long)size); }
void  kfree(void* p)       { free(p); }

// --- helpers the runner uses ----------------------------------------------

// Force a full repaint into the recorder, so the tests see the whole grid
// rather than only the rows the last write happened to dirty.
void snapshot()
{
    for (uint32_t y = 0; y < REC_MAX; y++)
        for (uint32_t x = 0; x < REC_MAX; x++)
            recorded[y][x] = { 0, 0, 0, 0 };
    term::invalidate();
    term::render();
}

uint8_t cell_ch(uint32_t x, uint32_t y)   { return recorded[y][x].ch; }
uint8_t cell_fg(uint32_t x, uint32_t y)   { return recorded[y][x].fg; }
uint8_t cell_bg(uint32_t x, uint32_t y)   { return recorded[y][x].bg; }
uint8_t cell_attr(uint32_t x, uint32_t y) { return recorded[y][x].attr; }

void feed_str(const char* s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    term::feed(s, n);
}

// Read a row back as text, for readable failure messages.
void row_text(uint32_t y, char* out, uint32_t n)
{
    uint32_t i = 0;
    for (; i < n - 1 && i < term::cols(); i++)
    {
        uint8_t c = recorded[y][i].ch;
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[i] = '\0';
}
