#include "../../include/drivers/screen.h"

// All internal state — static, invisible to other modules
static struct
{
    uint8_t* buffer;
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
} scr;

static const uint32_t BBP = 4;

static inline uint32_t max_cols()
{
    return scr.width / scr.sym_w;
}
static inline uint32_t max_rows()
{
    return scr.height / scr.sym_h;
}

// pixel helpers
static inline uint32_t* pixel_at(uint32_t x, uint32_t y)
{
    return (uint32_t*)(scr.buffer + (x + y * scr.pixels_per_scanline) * BBP);
}

static void draw_char(char chr, uint32_t col, uint32_t row)
{
    uint32_t ox = col * scr.sym_w;
    uint32_t oy = row * scr.sym_h;
    char* glyph = scr.font + chr * scr.sym_h;

    for (uint32_t y = 0; y < scr.sym_h; y++)
    {
        for (uint32_t x = 0; x < scr.sym_w; x++)
        {
            uint32_t color = (glyph[y] & (0x80 >> x)) ? scr.text_color : 0x00000000;
            *pixel_at(ox + x, oy + y) = color;
        }
    }
}

static void erase_rect(uint32_t px, uint32_t py, uint32_t w, uint32_t h)
{
    for (uint32_t y = py; y < py + h && y < scr.height; y++)
        for (uint32_t x = px; x < px + w && x < scr.width; x++)
            *pixel_at(x, y) = 0x00000000;
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

// API
namespace screen
{
    void init(BOOT_HEADER* header)
    {
        scr.buffer = (uint8_t*)0x600000;
        scr.buffer_size = header->FrameBufferSize;
        scr.pixels_per_scanline = header->ScreenPixelsPerScanLine;
        scr.width = header->ScreenWidth;
        scr.height = header->ScreenHeight;
        scr.font = (char*)header->StandartFontBuffer;
        scr.sym_w = header->FontSymbolSizeX;
        scr.sym_h = header->FontSymbolSizeY;
        scr.sym_count = header->FontNumberOfSymbols;
        scr.text_color = Colors::GRAY;
        scr.cursor_x = 0;
        scr.cursor_y = 0;

        clear();
    }

    uint32_t cols()
    {
        return max_cols();
    }
    uint32_t rows()
    {
        return max_rows();
    }
    uint32_t width()
    {
        return scr.width;
    }
    uint32_t height()
    {
        return scr.height;
    }

    uint32_t cursor_x()
    {
        return scr.cursor_x;
    }
    uint32_t cursor_y()
    {
        return scr.cursor_y;
    }

    void set_cursor(uint32_t x, uint32_t y)
    {
        scr.cursor_x = x;
        scr.cursor_y = y;
    }

    void clear()
    {
        uint64_t total = (uint64_t)scr.pixels_per_scanline * scr.height;
        uint32_t* buf = (uint32_t*)scr.buffer;
        for (uint64_t i = 0; i < total; i++)
            buf[i] = 0x00000000;

        set_cursor(0, 0);
    }

    void scroll_up()
    {
        uint32_t line_h  = scr.sym_h;
        uint32_t stride  = scr.pixels_per_scanline * BBP;

        uint64_t* src = (uint64_t*)(scr.buffer + line_h * stride);
        uint64_t* dst = (uint64_t*)scr.buffer;

        uint32_t copy_qwords = ((max_rows() - 1) * line_h * stride) / 8;
        for (uint32_t i = 0; i < copy_qwords; i++)
            dst[i] = src[i];

        // Erase last line
        uint64_t* last = (uint64_t*)(scr.buffer + (max_rows() - 1) * line_h * stride);
        uint32_t last_qwords = (line_h * stride) / 8;
        for (uint32_t i = 0; i < last_qwords; i++)
            last[i] = 0;

        set_cursor(0, max_rows() - 1);
    }

    void erase_at(uint32_t x, uint32_t y)
    {
        erase_rect(x * scr.sym_w, y * scr.sym_h, scr.sym_w, scr.sym_h);
    }

    void set_color(Colors color)
    {
        // Repaint all non-black pixels with new color
        for (uint32_t y = 0; y < scr.height; y++)
        {
            for (uint32_t x = 0; x < scr.width; x++)
            {
                uint32_t* px = pixel_at(x, y);
                if (*px != 0x00000000)
                    *px = (uint32_t)color;
            }
        }
        scr.text_color = color;
    }

    void putc(char c)
    {
        switch (c)
        {
            case '\n':
                if (scr.cursor_y + 1 >= max_rows())
                    scroll_up();
                else
                    scr.cursor_y++;
                break;

            case '\b':
                if (scr.cursor_x > 0)
                    scr.cursor_x--;
                erase_at(scr.cursor_x, scr.cursor_y);
                break;

            case '\t':
                scr.cursor_x += 4;
                if (scr.cursor_x >= max_cols())
                {
                    scr.cursor_x = 0;
                    if (scr.cursor_y + 1 >= max_rows())
                        scroll_up();
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
                        scroll_up();
                    else
                        scr.cursor_y++;
                }
                break;
        }
    }

    void write(const char* s)
    {
        while (*s)
            putc(*s++);
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
                putc(*fmt++);
                continue;
            }
            fmt++;

            bool ll = (*fmt == 'l' && fmt[1] == 'l');
            if (ll)
                fmt += 2;

            switch (*fmt)
            {
                case 's':
                    write(__builtin_va_arg(a, char*));
                    break;
                case 'c':
                    putc((char)__builtin_va_arg(a, int));
                    break;
                case 'u':
                case 'i':
                    utoa(ll ? __builtin_va_arg(a, uint64_t) : __builtin_va_arg(a, uint32_t), buf, 10);
                    write(buf);
                    break;
                case 'x':
                    write("0x");
                    utoa(ll ? __builtin_va_arg(a, uint64_t) : __builtin_va_arg(a, uint32_t), buf, 16);
                    write(buf);
                    break;
                case '%':
                    putc('%');
                    break;
            }
            fmt++;
        }
        __builtin_va_end(a);
    }

} // namespace screen