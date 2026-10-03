#include "../../include/drivers/screen.h"
#include "../../include/drivers/term.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../include/cpu/paging.h"
#include "../../include/mm/pmm.h"

static struct
{
    uint8_t* buffer;
    uint8_t* vram;
    uint64_t buffer_size;
    uint32_t pixels_per_scanline;

    uint32_t width;
    uint32_t height;

    char* font;
    uint16_t sym_w;
    uint16_t sym_h;
    uint32_t sym_count;

    uint32_t text_color;
    uint32_t pixel_format;  // 0 = RGBX, 1 = BGRX (assumed for BitMask/BltOnly)

    uint32_t vp_x;      // viewport pixel offset from left
    uint32_t vp_y;      // viewport pixel offset from top
    uint32_t vp_width;  // viewport width in pixels
    uint32_t vp_height; // viewport height in pixels

    uint32_t bar_y;     // the title bar: the top of the loader's viewport
    uint32_t bar_h;
} scr;

static const uint32_t BBP = 4;

static inline uint32_t max_cols()
{
    return scr.vp_width / scr.sym_w;
}

static inline uint32_t max_rows()
{
    return scr.vp_height / scr.sym_h;
}

static inline uint32_t* pixel_at(uint32_t x, uint32_t y)
{
    return (uint32_t*)(scr.buffer + (scr.vp_x + x + (scr.vp_y + y) * scr.pixels_per_scanline) * BBP);
}

// Convert an internal 0x00RRGGBB color to the native framebuffer format.
//
// Two things matter on real hardware (QEMU is forgiving, a Surface panel
// is not):
//  1. The high byte is the reserved/X channel. Some GOP implementations
//     blend it as alpha, so pixels written with X=0x00 (black bg, GRAY
//     text 0x9E...) become (semi)transparent: white title bar without
//     text, invisible console characters, only the cursor visible
//     (invert_cell already forced X=0xFF). Always force X=0xFF.
//  2. Internal colors are BGRX byte order. On an RGBX panel, swap R and B.
static inline uint32_t native_color(uint32_t c)
{
    if (scr.pixel_format == 0) // RGBX
        c = (c & 0x0000FF00) | ((c & 0x000000FF) << 16) | ((c & 0x00FF0000) >> 16);
    return c | 0xFF000000;
}

static inline void put_px(uint32_t* px, uint32_t c)
{
    *px = native_color(c);
}

// The 16 colours, in the same 0x00RRGGBB form as enum Colors, so both
// go through native_color() on the way to the panel. Index 7 keeps the grey
// the console has always used for normal text.
static const uint32_t palette[16] = {
    0x00000000, 0x00AA0000, 0x0000AA00, 0x00AA5500,
    0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x009E9E9E,
    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,
    0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};

// Rasterise one cell: glyph in `ink` on a solid `paper` background.
// Index the font unsigned: `char` is signed, so every byte >= 0x80 used to
// index *before* the font. It is a 256-glyph CP437 page, so that silently
// lost the whole upper half - box drawing included.
static void draw_glyph(uint8_t chr, uint32_t col, uint32_t row,
                       uint32_t ink, uint32_t paper)
{
    uint32_t ox = col * scr.sym_w;
    uint32_t oy = row * scr.sym_h;
    const unsigned char* glyph = (const unsigned char*)scr.font + chr * scr.sym_h;

    for (uint32_t y = 0; y < scr.sym_h; y++)
        for (uint32_t x = 0; x < scr.sym_w; x++)
            put_px(pixel_at(ox + x, oy + y),
                   (glyph[y] & (0x80 >> x)) ? ink : paper);
}

static void utoa(uint64_t v, char* b, uint32_t base)
{
    char* p = b;
    do
    {
        uint8_t d = v % base;
        *p++ = d < 10 ? '0' + d : 'A' + d - 10;
        v /= base;
    } while (v);
    *p = 0;

    for (char *l = b, *r = p - 1; l < r; l++, r--)
    {
        char t = *l;
        *l = *r;
        *r = t;
    }
}

// Everything printed goes through the terminal, which owns the cursor and
// the control characters - unless it is being captured (screen::capture): then into that buffer.
static char*    capture_buf  = nullptr;
static uint64_t capture_size = 0;
static uint64_t capture_len  = 0;

static inline void emit_char(char c)
{
    if (!capture_buf)
        term::putc(c);
    else if (capture_len + 1 < capture_size)
        capture_buf[capture_len++] = c;
    else
        capture_len++;                  // counted: the size it would need
}
static inline void emit_str(const char* s) { while (*s) emit_char(*s++); }

// A black glyph on the title bar: `cell` columns in from the left of the
// panel's viewport, top at pixel row `y`.
static void bar_glyph(uint8_t chr, uint32_t cell, uint32_t y)
{
    const unsigned char* glyph = (const unsigned char*)scr.font + chr * scr.sym_h;
    uint32_t x0 = scr.vp_x + cell * scr.sym_w;
    for (uint32_t gy = 0; gy < scr.sym_h; gy++)
        for (uint32_t gx = 0; gx < scr.sym_w; gx++)
            if (glyph[gy] & (0x80 >> gx))
                put_px((uint32_t*)(scr.buffer + (x0 + gx + (y + gy) * scr.pixels_per_scanline) * BBP),
                       0x00000000);
}

// API
namespace screen
{
    void init(BOOT_HEADER* header)
    {
        // Back buffer: normal RAM from the PMM, reachable through the
        // direct map. Sized to the panel, so a 4K display no longer
        // overruns whatever happened to follow a hardcoded address.
        uint64_t fb_size = header->FrameBufferSize;
        uint64_t frames  = (fb_size + FRAME_SIZE - 1) / FRAME_SIZE;
        uint64_t back    = pmm::alloc_frames(frames);

        if (!back)
        {
            uart::printf("screen: cannot allocate a %llu KB back buffer\n",
                         fb_size / 1024);
            while (1) asm volatile("cli; hlt");
        }

        // VRAM: mapped write-combining in the kernel device window. It used
        // to be mapped by overwriting the old identity map at virt 0x8000000,
        // which silently aliased the RAM at physical 0x8000000 - invisible
        // under `qemu -m 128M`, memory corruption on anything bigger.
        scr.buffer = (uint8_t*)phys_to_virt(back);
        scr.vram   = (uint8_t*)paging::map_framebuffer(
                         (uint64_t)header->FrameBufferAddress, fb_size);
        scr.buffer_size = fb_size;
        scr.pixels_per_scanline = header->ScreenPixelsPerScanLine;
        scr.width = header->ScreenWidth;
        scr.height = header->ScreenHeight;
        scr.font = (char*)phys_to_virt((uint64_t)header->StandartFontBuffer);
        scr.sym_w = header->FontSymbolSizeX;
        scr.sym_h = header->FontSymbolSizeY;
        scr.sym_count = header->FontNumberOfSymbols;
        scr.text_color = Colors::GRAY;
        scr.pixel_format = header->ScreenPixelFormat;
        scr.vp_x      = header->ViewportX;
        scr.vp_y      = header->ViewportY;
        scr.vp_width  = header->ViewportWidth;
        scr.vp_height = header->ViewportHeight;

        // The title bar takes the top of the viewport; the text area is
        // what is left below it. ~2.5% of the height, but never less than
        // a glyph plus 4px of padding (a 20px strip on a 2K panel is lost).
        uint32_t min_h = scr.sym_h + 4;
        uint32_t pct_h = scr.height * 25 / 1000;
        scr.bar_y      = scr.vp_y;
        scr.bar_h      = pct_h > min_h ? pct_h : min_h;
        scr.vp_y      += scr.bar_h;
        scr.vp_height -= scr.bar_h;

        // The cell grid is sized to the whole panel once; a viewport change
        // later just selects a smaller rectangle of it (term::resize).
        if (!term::init(scr.width / scr.sym_w, scr.height / scr.sym_h))
            while (1) asm volatile("cli; hlt");
        term::resize(max_cols(), max_rows());

        clear();
    }

    uint32_t cell_w() { return scr.sym_w; }
    uint32_t cell_h() { return scr.sym_h; }

    void draw_cell(uint32_t col, uint32_t row, uint8_t ch,
                   uint8_t fg, uint8_t bg, uint8_t attr)
    {
        if (col >= max_cols() || row >= max_rows())
            return;

        uint32_t ink   = palette[fg & 0x0F];
        uint32_t paper = palette[bg & 0x0F];

        if (attr & TERM_BOLD)
            ink = palette[(fg & 0x07) | 0x08];      // bold brightens the ink
        if (attr & TERM_REVERSE)
        {
            uint32_t t = ink;
            ink = paper;
            paper = t;
        }

        draw_glyph(ch, col, row, ink, paper);
    }

    void invert_cell(uint32_t col, uint32_t row)
    {
        if (col >= max_cols() || row >= max_rows())
            return;

        uint32_t ox = col * scr.sym_w;
        uint32_t oy = row * scr.sym_h;
        for (uint32_t y = 0; y < scr.sym_h; y++)
            for (uint32_t x = 0; x < scr.sym_w; x++)
            {
                uint32_t* px = pixel_at(ox + x, oy + y);
                *px = ~(*px) | 0xFF000000;
            }
    }

    uint64_t vram_base() { return (uint64_t)scr.vram; }

    uint32_t cols()  { return max_cols(); }
    uint32_t rows()  { return max_rows(); }
    uint32_t width() { return scr.width; }
    uint32_t height(){ return scr.height; }

    uint32_t cursor_x() { return term::cursor_x(); }
    uint32_t cursor_y() { return term::cursor_y(); }

    void set_cursor(uint32_t x, uint32_t y)
    {
        term::set_cursor(x, y);
    }

    void draw_title_bar(const char* left, const char* right)
    {
        for (uint32_t y = scr.bar_y; y < scr.bar_y + scr.bar_h; y++)
            for (uint32_t x = scr.vp_x; x < scr.vp_x + scr.vp_width; x++)
                put_px((uint32_t*)(scr.buffer + (x + y * scr.pixels_per_scanline) * BBP),
                       Colors::GRAY);

        // One cell of padding at each end; the left text stops short of
        // the right one.
        uint32_t cells = scr.vp_width / scr.sym_w;
        uint32_t rlen = 0;
        while (right[rlen])
            rlen++;
        uint32_t rstart = cells > rlen + 1 ? cells - rlen - 1 : 0;
        uint32_t text_y = scr.bar_y + (scr.bar_h - scr.sym_h) / 2;

        for (uint32_t i = 0; left[i] && 1 + i + 1 < rstart; i++)
            bar_glyph((uint8_t)left[i], 1 + i, text_y);
        for (uint32_t i = 0; i < rlen && rstart + i < cells; i++)
            bar_glyph((uint8_t)right[i], rstart + i, text_y);
    }

    void show_cursor() { term::show_cursor(); }
    void hide_cursor() { term::hide_cursor(); }

    // Retained so the timer path keeps compiling; blinking is part of
    // term::render() now, which runs on the same tick as the flush.
    void update_cursor() {}

    uint32_t shot_width()  { return scr.vp_width; }
    uint32_t shot_height() { return scr.vp_y + scr.vp_height - scr.bar_y; }

    void shot_row(uint32_t y, uint8_t* bgr)
    {
        const uint8_t* px = scr.buffer +
                            (scr.vp_x + (scr.bar_y + y) * scr.pixels_per_scanline) * BBP;
        bool rgb = scr.pixel_format == 0;       // RGBX in memory, else BGRX
        for (uint32_t x = 0; x < scr.vp_width; x++, px += BBP, bgr += 3)
        {
            bgr[0] = rgb ? px[2] : px[0];
            bgr[1] = px[1];
            bgr[2] = rgb ? px[0] : px[2];
        }
    }

    void flush()
    {
        // rep movsq: ~15 MB per flush at 2K resolution, a naive 64-bit
        // loop is far too slow (this runs from the PIT IRQ ~45 times a
        // second and starved everything else on real hardware).
        // NOTE: use local pointers - rep movsq advances RSI/RDI and the
        // "+S"/"+D" constraints would write the shifted values back into
        // scr.buffer/scr.vram.
        uint8_t* src = scr.buffer;
        uint8_t* dst = scr.vram;
        uint64_t count = scr.buffer_size / 8;
        asm volatile("rep movsq"
                     : "+S"(src), "+D"(dst), "+c"(count)
                     :
                     : "memory");
    }

    void clear()
    {
        // Blank the pixels here as well as the grid: the back buffer comes
        // straight from the PMM at boot, so without this the panel shows
        // whatever was in those frames until the first render.
        for (uint32_t y = 0; y < scr.vp_height; y++)
            for (uint32_t x = 0; x < scr.vp_width; x++)
                put_px(pixel_at(x, y), 0x00000000);

        term::clear();
    }

    void scroll_up()
    {
        term::scroll_up();
    }

    void erase_at(uint32_t x, uint32_t y)
    {
        term::erase_at(x, y);
    }

    void set_color(Colors color)
    {
        // Legacy entry point: pick the closest palette entry and make it the
        // default ink. It used to repaint every non-black pixel in the
        // viewport; with a cell grid the colour is simply an attribute.
        uint32_t c = (uint32_t)color;
        uint32_t best = TERM_WHITE;
        uint32_t best_d = 0xFFFFFFFF;
        for (uint32_t i = 0; i < 16; i++)
        {
            int dr = (int)((c >> 16) & 0xFF) - (int)((palette[i] >> 16) & 0xFF);
            int dg = (int)((c >> 8)  & 0xFF) - (int)((palette[i] >> 8)  & 0xFF);
            int db = (int)( c        & 0xFF) - (int)( palette[i]        & 0xFF);
            uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (d < best_d)
            {
                best_d = d;
                best = i;
            }
        }
        scr.text_color = c;
        term::set_fg((uint8_t)best);
    }

    void putc(char c) { emit_char(c); }

    void write(const char* s) { emit_str(s); }

    void write(const char* s, uint64_t len)
    {
        if (!capture_buf)
            term::feed(s, len);
        else
            for (uint64_t i = 0; i < len; i++)
                emit_char(s[i]);
    }

    void capture(char* buf, uint64_t size)
    {
        capture_buf  = buf;
        capture_size = size;
        capture_len  = 0;
    }

    uint64_t end_capture()
    {
        if (capture_buf && capture_size)
            capture_buf[capture_len < capture_size ? capture_len : capture_size - 1] = '\0';
        capture_buf = nullptr;
        return capture_len + 1;
    }

    void printf(const char* fmt, ...)
    {
        __builtin_va_list a;
        __builtin_va_start(a, fmt);

        char buf[32];

        while (*fmt)
        {
            if (*fmt != '%')
            {
                emit_char(*fmt++);
                continue;
            }
            fmt++;

            // Parse flags: '-' pads on the right, '0' with zeroes.
            char pad_char = ' ';
            bool left = false;
            for (;; fmt++)
            {
                if (*fmt == '-')
                    left = true;
                else if (*fmt == '0')
                    pad_char = '0';
                else
                    break;
            }
            if (left)
                pad_char = ' ';

            // Parse width
            uint32_t width = 0;
            while (*fmt >= '0' && *fmt <= '9')
            {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            // Parse 'll' length modifier
            bool ll = (*fmt == 'l' && fmt[1] == 'l');
            if (ll)
                fmt += 2;

            switch (*fmt)
            {
                case 's':
                {
                    const char* s = __builtin_va_arg(a, char*);
                    uint32_t len = 0;
                    while (s[len]) len++;
                    if (!left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(' ');
                    emit_str(s);
                    if (left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(' ');
                    break;
                }
                case 'c':
                    emit_char((char)__builtin_va_arg(a, int));
                    break;
                case 'd':
                case 'i':
                {
                    sint64_t v = ll ? __builtin_va_arg(a, sint64_t)
                                    : (sint64_t)__builtin_va_arg(a, int);
                    uint32_t start = 0;
                    if (v < 0)
                        buf[start++] = '-';
                    utoa(v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, buf + start, 10);
                    uint32_t len = 0;
                    while (buf[len]) len++;
                    if (!left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(pad_char);
                    emit_str(buf);
                    if (left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(' ');
                    break;
                }
                case 'u':
                {
                    utoa(ll ? __builtin_va_arg(a, uint64_t)
                            : __builtin_va_arg(a, uint32_t), buf, 10);
                    // Pad if needed
                    uint32_t len = 0;
                    while (buf[len]) len++;
                    if (!left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(pad_char);
                    emit_str(buf);
                    if (left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(' ');
                    break;
                }
                case 'x':
                {
                    emit_str("0x");
                    utoa(ll ? __builtin_va_arg(a, uint64_t)
                            : __builtin_va_arg(a, uint32_t), buf, 16);
                    uint32_t len = 0;
                    while (buf[len]) len++;
                    if (!left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(pad_char);
                    emit_str(buf);
                    if (left)
                        for (uint32_t p = len; p < width; p++)
                            emit_char(' ');
                    break;
                }
                case '%':
                    emit_char('%');
                    break;
            }
            fmt++;
        }
        __builtin_va_end(a);
    }

} // namespace screen