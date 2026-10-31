// Screenshots (shot.h): the key wakes the "shot" kernel process, which
// starts /sfos/SHOT.BIN on the screen shown - unless the last one still
// runs. The program takes the picture and writes the file.

#include "../../include/drivers/shot.h"
#include "../../include/drivers/term.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/wait.h"

namespace
{
    wait_queue shot_wq;
    volatile bool wanted = false;

    bool is_wanted(void*) { return wanted; }

    void shot_main(void*)
    {
        pid_t last = -1;
        for (;;)
        {
            wait::wait_event(&shot_wq, is_wanted, nullptr, 0);
            wanted = false;
            if (last >= 0 && process::running(last))
                continue;                   // one at a time
            last = process::start_system_program("/sfos/SHOT.BIN", term::shown_screen());
            if (last < 0)
                term::set_status("screenshot failed: no /sfos/SHOT.BIN", 4000);
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
