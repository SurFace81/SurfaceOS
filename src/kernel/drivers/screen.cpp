#include "../../include/drivers/screen.h"
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

    uint32_t cursor_x;
    uint32_t cursor_y;

    char* font;
    uint16_t sym_w;
    uint16_t sym_h;
    uint32_t sym_count;

    uint32_t text_color;
    uint32_t pixel_format;  // 0 = RGBX, 1 = BGRX (assumed for BitMask/BltOnly)

    bool cursor_visible;
    bool cursor_drawn;
    uint32_t cursor_tick;

    uint32_t vp_x;      // viewport pixel offset from left
    uint32_t vp_y;      // viewport pixel offset from top
    uint32_t vp_width;  // viewport width in pixels
    uint32_t vp_height; // viewport height in pixels
} scr;

static const uint32_t BBP = 4;
static bool out_in_progress = false;

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

static void invert_cell(uint32_t col, uint32_t row)
{
    uint32_t ox = col * scr.sym_w;
    uint32_t oy = row * scr.sym_h;

    for (uint32_t y = 0; y < scr.sym_h; y++)
        for (uint32_t x = 0; x < scr.sym_w; x++)
        {
            uint32_t* px = pixel_at(ox + x, oy + y);
            *px = ~(*px) | 0xFF000000;
        }
}

static void cursor_draw()
{
    if (scr.cursor_drawn)
        return;
    if (scr.cursor_x < max_cols() && scr.cursor_y < max_rows())
    {
        invert_cell(scr.cursor_x, scr.cursor_y);
        scr.cursor_drawn = true;
    }
}

static void cursor_undraw()
{
    if (!scr.cursor_drawn)
        return;
    if (scr.cursor_x < max_cols() && scr.cursor_y < max_rows())
    {
        invert_cell(scr.cursor_x, scr.cursor_y);
        scr.cursor_drawn = false;
    }
}

static void draw_char(char chr, uint32_t col, uint32_t row)
{
    uint32_t ox = col * scr.sym_w;
    uint32_t oy = row * scr.sym_h;
    char* glyph = scr.font + chr * scr.sym_h;

    for (uint32_t y = 0; y < scr.sym_h; y++)
        for (uint32_t x = 0; x < scr.sym_w; x++)
        {
            uint32_t color = (glyph[y] & (0x80 >> x)) ? scr.text_color : 0x00000000;
            put_px(pixel_at(ox + x, oy + y), color);
        }
}

static void erase_rect(uint32_t px, uint32_t py, uint32_t w, uint32_t h)
{
    for (uint32_t y = py; y < py + h && y < scr.vp_height; y++)
        for (uint32_t x = px; x < px + w && x < scr.vp_width; x++)
            put_px(pixel_at(x, y), 0x00000000);
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

// Raw char output: no cursor management, just draw and advance.
// All public output functions (putc, write, printf) use this internally.
static void emit_char(char c)
{
    switch (c)
    {
        case '\n':
            scr.cursor_x = 0;
            if (scr.cursor_y + 1 >= max_rows())
                screen::scroll_up();
            else
                scr.cursor_y++;
            break;

        case '\b':
            if (scr.cursor_x > 0)
                scr.cursor_x--;
            erase_rect(scr.cursor_x * scr.sym_w, scr.cursor_y * scr.sym_h,
                       scr.sym_w, scr.sym_h);
            break;

        case '\t':
            scr.cursor_x += 4;
            if (scr.cursor_x >= max_cols())
            {
                scr.cursor_x = 0;
                if (scr.cursor_y + 1 >= max_rows())
                    screen::scroll_up();
                else
                    scr.cursor_y++;
            }
            break;

        case '\r':
            scr.cursor_x = 0;
            break;

        default:
            draw_char(c, scr.cursor_x, scr.cursor_y);
            scr.cursor_x++;

            if (scr.cursor_x >= max_cols())
            {
                scr.cursor_x = 0;
                if (scr.cursor_y + 1 >= max_rows())
                    screen::scroll_up();
                else
                    scr.cursor_y++;
            }
            break;
    }
}

// Emit a C-string without cursor management
static void emit_str(const char* s)
{
    while (*s)
        emit_char(*s++);
}

static struct
{
    uint32_t x, y, w, h;
} saved_vp;

// API
namespace screen
{
    void init(BOOT_HEADER* header)
    {
        // Back buffer: normal RAM from the PMM, reachable through the
        // identity map. Sized to the panel, so a 4K display no longer
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
        // to be mapped by overwriting the identity map at virt 0x8000000,
        // which silently aliased the RAM at physical 0x8000000 - invisible
        // under `qemu -m 128M`, memory corruption on anything bigger.
        scr.buffer = (uint8_t*)back;
        scr.vram   = (uint8_t*)paging::map_framebuffer(
                         (uint64_t)header->FrameBufferAddress, fb_size);
        scr.buffer_size = fb_size;
        scr.pixels_per_scanline = header->ScreenPixelsPerScanLine;
        scr.width = header->ScreenWidth;
        scr.height = header->ScreenHeight;
        scr.font = (char*)header->StandartFontBuffer;
        scr.sym_w = header->FontSymbolSizeX;
        scr.sym_h = header->FontSymbolSizeY;
        scr.sym_count = header->FontNumberOfSymbols;
        scr.text_color = Colors::GRAY;
        scr.pixel_format = header->ScreenPixelFormat;
        scr.cursor_x = 0;
        scr.cursor_y = 0;
        scr.cursor_visible = false;
        scr.cursor_drawn = false;
        scr.cursor_tick = 0;
        scr.vp_x      = header->ViewportX;
        scr.vp_y      = header->ViewportY;
        scr.vp_width  = header->ViewportWidth;
        scr.vp_height = header->ViewportHeight;

        clear();
    }

    uint64_t vram_base() { return (uint64_t)scr.vram; }

    uint32_t cols()  { return max_cols(); }
    uint32_t rows()  { return max_rows(); }
    uint32_t width() { return scr.width; }
    uint32_t height(){ return scr.height; }

    uint32_t cursor_x() { return scr.cursor_x; }
    uint32_t cursor_y() { return scr.cursor_y; }

    void set_cursor(uint32_t x, uint32_t y)
    {
        cursor_undraw();
        scr.cursor_x = x;
        scr.cursor_y = y;
        scr.cursor_tick = 0;
        if (scr.cursor_visible)
            cursor_draw();
    }

    void push_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        saved_vp.x = scr.vp_x;
        saved_vp.y = scr.vp_y;
        saved_vp.w = scr.vp_width;
        saved_vp.h = scr.vp_height;

        scr.vp_x = x;
        scr.vp_y = y;
        scr.vp_width = w;
        scr.vp_height = h;
        scr.cursor_x = 0;
        scr.cursor_y = 0;
    }

    void pop_viewport()
    {
        scr.vp_x = saved_vp.x;
        scr.vp_y = saved_vp.y;
        scr.vp_width = saved_vp.w;
        scr.vp_height = saved_vp.h;
        scr.cursor_x = 0;
        scr.cursor_y = 0;
    }

    uint32_t title_bar_height()
    {
        // ~2.5% of the screen height, but never smaller than the glyph
        // height + 4px padding. On a 2K panel this gives a bar that is
        // actually visible instead of a thin 20px strip.
        uint32_t min_h = scr.sym_h + 4;
        uint32_t pct_h = scr.height * 25 / 1000;   // 2.5%
        return pct_h > min_h ? pct_h : min_h;
    }

    void draw_title_bar(const char* title)
    {
        uint32_t bar_height = title_bar_height();

        // Fill bar with gray
        for (uint32_t y = scr.vp_y; y < scr.vp_y + bar_height; y++)
            for (uint32_t x = scr.vp_x; x < scr.vp_x + scr.vp_width; x++)
            {
                uint32_t* px = (uint32_t*)(scr.buffer +
                    (x + y * scr.pixels_per_scanline) * BBP);
                put_px(px, Colors::GRAY);
            }

        // Measure title length
        uint32_t len = 0;
        while (title[len])
            len++;

        // Center text horizontally and vertically inside the bar
        uint32_t text_px_w = len * scr.sym_w;
        uint32_t text_x = scr.vp_x + (scr.vp_width - text_px_w) / 2;
        uint32_t text_y = scr.vp_y + (bar_height - scr.sym_h) / 2;

        // Draw each character in black on the gray background
        char* glyph;
        for (uint32_t i = 0; i < len; i++)
        {
            glyph = scr.font + title[i] * scr.sym_h;
            for (uint32_t gy = 0; gy < scr.sym_h; gy++)
                for (uint32_t gx = 0; gx < scr.sym_w; gx++)
                {
                    uint32_t* px = (uint32_t*)(scr.buffer +
                        (text_x + i * scr.sym_w + gx +
                        (text_y + gy) * scr.pixels_per_scanline) * BBP);
                    if (glyph[gy] & (0x80 >> gx))
                        put_px(px, 0x00000000); // black text
                    // else leave gray
                }
        }
    }

    uint32_t vp_x() { return scr.vp_x; }
    uint32_t vp_y() { return scr.vp_y; }
    uint32_t vp_w() { return scr.vp_width; }
    uint32_t vp_h() { return scr.vp_height; }

    void show_cursor()
    {
        scr.cursor_visible = true;
        scr.cursor_tick = 0;
        cursor_draw();
    }

    void hide_cursor()
    {
        cursor_undraw();
        scr.cursor_visible = false;
    }

    void update_cursor()
    {
        if (!scr.cursor_visible || out_in_progress)
            return;

        uint32_t now = (uint32_t)(pit::uptime_ms());
        bool should_be_drawn = ((now / 500) % 2) == 0;

        if (should_be_drawn && !scr.cursor_drawn)
            cursor_draw();
        else if (!should_be_drawn && scr.cursor_drawn)
            cursor_undraw();
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
        cursor_undraw();

        for (uint32_t y = 0; y < scr.vp_height; y++)
            for (uint32_t x = 0; x < scr.vp_width; x++)
                put_px(pixel_at(x, y), 0x00000000);

        scr.cursor_x = 0;
        scr.cursor_y = 0;
        scr.cursor_tick = 0;

        if (scr.cursor_visible)
            cursor_draw();
    }

    void scroll_up()
    {
        cursor_undraw();

        uint32_t line_h = scr.sym_h;
        uint32_t row_count = max_rows();

        // Shift rows up within viewport
        for (uint32_t row = 1; row < row_count; row++)
            for (uint32_t y = 0; y < line_h; y++)
            {
                uint32_t* dst = pixel_at(0, (row - 1) * line_h + y);
                uint32_t* src = pixel_at(0, row * line_h + y);
                for (uint32_t x = 0; x < scr.vp_width; x++)
                    dst[x] = src[x];
            }

        // Clear last row
        for (uint32_t y = 0; y < line_h; y++)
            for (uint32_t x = 0; x < scr.vp_width; x++)
                put_px(pixel_at(x, (row_count - 1) * line_h + y), 0);

        scr.cursor_x = 0;
        scr.cursor_y = row_count - 1;
        scr.cursor_tick = 0;
    }

    void erase_at(uint32_t x, uint32_t y)
    {
        cursor_undraw();
        erase_rect(x * scr.sym_w, y * scr.sym_h, scr.sym_w, scr.sym_h);
        if (scr.cursor_visible)
            cursor_draw();
    }

    void set_color(Colors color)
    {
        cursor_undraw();

        for (uint32_t y = 0; y < scr.vp_height; y++)
            for (uint32_t x = 0; x < scr.vp_width; x++)
            {
                uint32_t* px = pixel_at(x, y);
                if (*px != native_color(0x00000000))
                    put_px(px, (uint32_t)color);
            }
        scr.text_color = color;

        if (scr.cursor_visible)
            cursor_draw();
    }

    void putc(char c)
    {
        out_in_progress = true;
        cursor_undraw();
        emit_char(c);
        scr.cursor_tick = 0;
        if (scr.cursor_visible)
            cursor_draw();
        out_in_progress = false;
    }

    void write(const char* s)
    {
        out_in_progress = true;
        cursor_undraw();
        emit_str(s);
        scr.cursor_tick = 0;
        if (scr.cursor_visible)
            cursor_draw();
        out_in_progress = false;
    }

    void write(const char* s, uint64_t len)
    {
        out_in_progress = true;
        cursor_undraw();
        for (uint64_t i = 0; i < len; i++)
            emit_char(s[i]);
        scr.cursor_tick = 0;
        if (scr.cursor_visible)
            cursor_draw();
        out_in_progress = false;
    }

    void printf(const char* fmt, ...)
    {
        out_in_progress = true;
        cursor_undraw();

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

            // Parse flags
            char pad_char = ' ';
            if (*fmt == '0')
            {
                pad_char = '0';
                fmt++;
            }

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
                    emit_str(__builtin_va_arg(a, char*));
                    break;
                case 'c':
                    emit_char((char)__builtin_va_arg(a, int));
                    break;
                case 'u':
                case 'i':
                {
                    utoa(ll ? __builtin_va_arg(a, uint64_t)
                            : __builtin_va_arg(a, uint32_t), buf, 10);
                    // Pad if needed
                    uint32_t len = 0;
                    while (buf[len]) len++;
                    for (uint32_t p = len; p < width; p++)
                        emit_char(pad_char);
                    emit_str(buf);
                    break;
                }
                case 'x':
                {
                    emit_str("0x");
                    utoa(ll ? __builtin_va_arg(a, uint64_t)
                            : __builtin_va_arg(a, uint32_t), buf, 16);
                    uint32_t len = 0;
                    while (buf[len]) len++;
                    for (uint32_t p = len; p < width; p++)
                        emit_char(pad_char);
                    emit_str(buf);
                    break;
                }
                case '%':
                    emit_char('%');
                    break;
            }
            fmt++;
        }
        __builtin_va_end(a);

        scr.cursor_tick = 0;
        if (scr.cursor_visible)
            cursor_draw();

        out_in_progress = false;
    }

} // namespace screen