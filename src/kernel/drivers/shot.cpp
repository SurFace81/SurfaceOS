// Screenshots (shot.h). The picture is copied out of the back buffer at
// once, as a whole BMP file in contiguous frames, so what is written is
// one moment; then the file goes to the disk in one write.

#include "../../include/drivers/shot.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/term.h"
#include "../../include/drivers/rtc.h"
#include "../../include/drivers/uart.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/wait.h"
#include "../../include/cpu/paging.h"
#include "../../include/mm/pmm.h"
#include "../../include/fs/vfs.h"
#include "../../include/errno.h"

namespace
{
    wait_queue shot_wq;
    volatile bool wanted = false;

    bool is_wanted(void*) { return wanted; }

    void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

    char* put_num(char* p, uint32_t v, uint32_t digits)
    {
        for (uint32_t i = digits; i-- > 0; v /= 10)
            p[i] = (char)('0' + v % 10);
        return p + digits;
    }

    // /files/screenshots, made when missing; referenced, or null.
    vnode* folder()
    {
        vnode* files = nullptr;
        if (vfs::lookup("/files", nullptr, &files, true) != 0)
            return nullptr;
        vnode* dir = nullptr;
        sint64_t rc = files->ops->lookup(files, "screenshots", &dir);
        if (rc == -ENOENT && files->ops->mkdir &&
            files->ops->mkdir(files, "screenshots", 0755) == 0)
            rc = files->ops->lookup(files, "screenshots", &dir);
        vfs::unref(files);
        return rc == 0 ? dir : nullptr;
    }

    // The picture as a BMP file: 24 bits a pixel, rows bottom up, each
    // padded to 4 bytes. Into `name` (40 bytes) goes the file's name.
    bool save(char* name)
    {
        uint32_t w = screen::shot_width(), h = screen::shot_height();
        uint32_t row = (w * 3 + 3) & ~3u;
        uint64_t size = 54 + (uint64_t)row * h;
        uint64_t frames = (size + 4095) / 4096;
        uint64_t phys = pmm::alloc_frames(frames);
        if (!phys)
        {
            uart::printf("shot: no %llu KB for the picture\n", size / 1024);
            return false;
        }
        uint8_t* bmp = (uint8_t*)phys_to_virt(phys);

        for (uint32_t y = 0; y < h; y++)
        {
            uint8_t* r = bmp + 54 + (uint64_t)(h - 1 - y) * row;
            screen::shot_row(y, r);
            for (uint32_t i = w * 3; i < row; i++)
                r[i] = 0;
        }
        for (uint32_t i = 0; i < 54; i++)
            bmp[i] = 0;
        bmp[0] = 'B';
        bmp[1] = 'M';
        put32(bmp + 2, (uint32_t)size);
        put32(bmp + 10, 54);            // where the pixels start
        put32(bmp + 14, 40);            // BITMAPINFOHEADER
        put32(bmp + 18, w);
        put32(bmp + 22, h);
        put16(bmp + 26, 1);             // planes
        put16(bmp + 28, 24);            // bits a pixel
        put32(bmp + 34, row * h);
        put32(bmp + 38, 2835);          // 72 dpi
        put32(bmp + 42, 2835);

        rtc_time t;
        rtc::read(&t);
        char* q = name;
        for (const char* c = "shot_"; *c; c++)
            *q++ = *c;
        q = put_num(q, t.year, 4);    *q++ = '-';
        q = put_num(q, t.month, 2);   *q++ = '-';
        q = put_num(q, t.day, 2);     *q++ = '_';
        q = put_num(q, t.hours, 2);   *q++ = '-';
        q = put_num(q, t.minutes, 2); *q++ = '-';
        q = put_num(q, t.seconds, 2);
        for (const char* c = ".bmp"; ; c++)
            if (!(*q++ = *c))
                break;

        bool ok = false;
        vnode* dir = folder();
        vnode* v = nullptr;
        if (dir && dir->ops->create && dir->ops->create(dir, name, 0644, &v) == 0)
        {
            uint64_t done = 0;
            ok = v->ops->write(v, 0, bmp, size, &done) == 0 && done == size;
            if (v->ops->fsync)
                v->ops->fsync(v);
            vfs::unref(v);
        }
        if (dir)
            vfs::unref(dir);
        pmm::free_frames(phys, frames);
        uart::printf("shot: %s %s, %ux%u\n", name, ok ? "saved" : "NOT saved", w, h);
        return ok;
    }

    void shot_main(void*)
    {
        for (;;)
        {
            wait::wait_event(&shot_wq, is_wanted, nullptr, 0);
            wanted = false;
            char name[40];
            bool ok = save(name);

            // Say so in the title bar of the screen shown.
            char text[64] = "";
            char* e = text;
            for (const char* c = ok ? "saved /files/screenshots/" : "screenshot failed"; *c; c++)
                *e++ = *c;
            for (const char* c = name; ok && *c && e < text + sizeof(text) - 1; c++)
                *e++ = *c;
            *e = '\0';
            uint32_t prev = term::selected();
            term::select(term::shown_screen());
            term::set_subtitle(text);
            term::select(prev);
        }
    }
}

namespace shot
{
    void start()
    {
        process::start_kernel_process("shot", shot_main);
    }

    void request()
    {
        wanted = true;
        wait::wake_up(&shot_wq);
    }
}
