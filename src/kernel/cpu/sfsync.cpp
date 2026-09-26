// Synchronisation through the SurfaceOS SDK. See sfsync.h.

#include "../../include/cpu/sfsync.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/obj/event.h"
#include "../../include/drivers/pit.h"
#include "../../sdk/include/abi/errno.h"
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
        sint64_t rc = handles::install(process::cur_handles(), o, 0, 0, &h);
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
    void wait(user_regs* regs, iret_frame*)
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
        sfcall::set_handler(SFCALL_WAIT, wait);
        sfcall::set_handler(SFCALL_CLOSE, close);
    }
}
