// Synchronisation through the SurfaceOS SDK. See sfsync.h.

#include "../../include/cpu/sfsync.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/sfconsole.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/wait.h"
#include "../../include/obj/event.h"
#include "../../include/drivers/pit.h"
#include "../../include/errno.h"
#include "../../sdk/include/sfos.h"

namespace
{
    // The object behind handle h of the calling process, of type `want`
    // (None: any), or nullptr.
    kobject* object(uint64_t h, obj_type want)
    {
        if (h >= HANDLE_TABLE_SIZE)
            return nullptr;
        sint64_t rc;
        return handles::get(process::cur_handles(), (sint32_t)h, want, &rc);
    }

    // (Flags, *Handle)
    void event_create(user_regs* regs, iret_frame*)
    {
        if (regs->rdi & ~SF_EVENT_AUTO_RESET)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        kobject* o = event::create((regs->rdi & SF_EVENT_AUTO_RESET) != 0);
        if (!o)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        sint32_t h = -1;
        sint64_t rc = handles::install(process::cur_handles(), o, &h);
        kobj::put(o);                       // the handle holds it now (or nobody)
        if (rc != 0)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        uint64_t handle = (uint64_t)h;
        if (!uaccess::copy_to_user(regs->rsi, &handle, sizeof(handle)))
        {
            handles::close(process::cur_handles(), h);
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        regs->rax = SF_SUCCESS;
    }

    // (Handle)
    void event_set(user_regs* regs, iret_frame*)
    {
        kobject* o = object(regs->rdi, obj_type::Event);
        if (o)
            event::set(o);
        regs->rax = o ? SF_SUCCESS : SF_BAD_HANDLE;
    }

    // (Handle)
    void event_reset(user_regs* regs, iret_frame*)
    {
        kobject* o = object(regs->rdi, obj_type::Event);
        if (o)
            event::reset(o);
        regs->rax = o ? SF_SUCCESS : SF_BAD_HANDLE;
    }

    // (Handle, TimeoutMs): any waitable object - an event, a thread.
    void wait_one(user_regs* regs, iret_frame*)
    {
        kobject* o = object(regs->rdi, obj_type::None);
        if (!o)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        uint64_t ms   = regs->rsi;
        uint64_t tick = ms == SF_WAIT_FOREVER ? 0 : pit::deadline_ms(ms);

        // An own reference: another thread may close the handle meanwhile.
        kobj::get(o);
        sint64_t rc = objects::wait(o, tick);
        kobj::put(o);
        switch (rc)
        {
            case 0:           regs->rax = SF_SUCCESS;           break;
            case -ETIMEDOUT:  regs->rax = SF_TIMEOUT;           break;
            case -EINTR:      regs->rax = SF_ABORTED;           break;
            default:          regs->rax = SF_INVALID_PARAMETER; break;   // not waitable
        }
    }

    // WaitAny's items: the object behind each handle (referenced), or none
    // for a key.
    struct AnyWait
    {
        uint64_t count;
        kobject* obj[SF_WAIT_MAX_ITEMS];
        uint64_t ready;                 // the first ready one, once found
    };

    bool any_ready(void* arg)
    {
        AnyWait* w = (AnyWait*)arg;
        for (uint64_t i = 0; i < w->count; i++)
        {
            kobject* o = w->obj[i];
            if (o ? o->ops->signaled(o) : sfconsole::key_ready())
            {
                w->ready = i;
                return true;
            }
        }
        return false;
    }

    // (const uint64_t* Items, Count, TimeoutMs, uint64_t* Index): Items is
    // Count pairs of a kind and a handle.
    void wait_any(user_regs* regs, iret_frame*)
    {
        uint64_t count = regs->rsi;
        uint64_t items[SF_WAIT_MAX_ITEMS * 2];
        if (count > SF_WAIT_MAX_ITEMS ||
            (count && !uaccess::copy_from_user(items, regs->rdi, count * 16)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        AnyWait w = {};
        regs->rax = SF_SUCCESS;
        for (uint64_t i = 0; i < count; i++)
        {
            uint64_t kind = items[i * 2];
            obj_type want = kind == SF_WAIT_EVENT   ? obj_type::Event
                          : kind == SF_WAIT_PROCESS ? obj_type::Process
                          : kind == SF_WAIT_THREAD  ? obj_type::Thread : obj_type::None;
            kobject* o = want != obj_type::None ? object(items[i * 2 + 1], want) : nullptr;
            if (kind != SF_WAIT_KEY && !o)
            {
                regs->rax = kind <= SF_WAIT_KEY && kind ? SF_BAD_HANDLE : SF_INVALID_PARAMETER;
                break;
            }
            // An own reference: another thread may close the handle meanwhile.
            if (o)
                kobj::get(o);
            w.obj[w.count++] = o;
        }

        if (regs->rax == SF_SUCCESS)
        {
            uint64_t ms   = regs->rdx;
            uint64_t tick = ms == SF_WAIT_FOREVER ? 0 : pit::deadline_ms(ms);
            if (!wait::wait_any(any_ready, &w, tick))
                regs->rax = SF_ABORTED;
            else if (!any_ready(&w))
                regs->rax = SF_TIMEOUT;
            else
            {
                // As in objects::wait: nothing takes the signal between the
                // look and consume().
                kobject* o = w.obj[w.ready];
                if (o && o->ops->consume)
                    o->ops->consume(o);
                if (regs->r10 && !uaccess::copy_to_user(regs->r10, &w.ready, sizeof(w.ready)))
                    regs->rax = SF_INVALID_PARAMETER;
            }
        }
        for (uint64_t i = 0; i < w.count; i++)
            if (w.obj[i])
                kobj::put(w.obj[i]);
    }

    // (Handle): any handle.
    void close(user_regs* regs, iret_frame*)
    {
        if (regs->rdi >= HANDLE_TABLE_SIZE ||
            handles::close(process::cur_handles(), (sint32_t)regs->rdi) != 0)
            regs->rax = SF_BAD_HANDLE;
        else
            regs->rax = SF_SUCCESS;
    }
}

namespace sfsync
{
    void init()
    {
        sfcall::set_handler(SFCALL_EVENT_CREATE, event_create);
        sfcall::set_handler(SFCALL_EVENT_SET, event_set);
        sfcall::set_handler(SFCALL_EVENT_RESET, event_reset);
        sfcall::set_handler(SFCALL_WAIT, wait_one);
        sfcall::set_handler(SFCALL_WAIT_ANY, wait_any);
        sfcall::set_handler(SFCALL_CLOSE, close);
    }
}
