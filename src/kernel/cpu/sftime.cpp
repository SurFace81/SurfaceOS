// Time through the SurfaceOS SDK. See sftime.h.

#include "../../include/cpu/sftime.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/wait.h"
#include "../../include/drivers/rtc.h"
#include "../../include/drivers/pit.h"
#include "../../sdk/include/sfos.h"

namespace
{
    // (SfDateTime* Time)
    void get_time(user_regs* regs, iret_frame*)
    {
        rtc_time t;
        rtc::read(&t);

        SfDateTime out;
        out.Year     = t.year;
        out.Month    = t.month;
        out.Day      = t.day;
        out.Hour     = t.hours;
        out.Minute   = t.minutes;
        out.Second   = t.seconds;
        out.Reserved = 0;
        regs->rax = uaccess::copy_to_user(regs->rdi, &out, sizeof(out))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }

    // (uint64_t* Milliseconds)
    void get_uptime(user_regs* regs, iret_frame*)
    {
        uint64_t ms = pit::uptime_ms();
        regs->rax = uaccess::copy_to_user(regs->rdi, &ms, sizeof(ms))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }

    // (uint64_t Milliseconds)
    void sleep(user_regs* regs, iret_frame*)
    {
        uint64_t ms = regs->rdi;
        if (ms == 0)
        {
            regs->rax = SF_SUCCESS;
            return;
        }

        // Cut short only when the program is ended. A pause (Ctrl+Alt+Z)
        // does not stop the clock: past the deadline by then, it returns
        // right after the program goes on.
        regs->rax = wait::sleep_until(pit::deadline_ms(ms)) ? SF_SUCCESS : SF_ABORTED;
    }
}

namespace sftime
{
    void init()
    {
        sfcall::set_handler(SFCALL_TIME_GET, get_time);
        sfcall::set_handler(SFCALL_TIME_GET_UPTIME, get_uptime);
        sfcall::set_handler(SFCALL_TIME_SLEEP, sleep);
    }
}
