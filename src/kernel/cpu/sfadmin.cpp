// The admin right through the SurfaceOS SDK. See sfadmin.h and
// sfos/admin.h.
//
// Every call checks the right itself: Sys->Admin is only a convenience,
// a program without it can still make the call and must be refused.

#include "../../include/cpu/sfadmin.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/fs/mounts.h"
#include "../../include/acpi/acpi.h"
#include "../../include/mm/heap.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/rtc.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/reports.h"
#include "../../include/cpu/cpuid.h"
#include "../../include/cpu/smp.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/memory.h"
#include "../../include/stdlib/string.h"
#include "../../include/errno.h"
#include "../../sdk/include/sfos.h"

namespace
{
    bool allowed(user_regs* regs)
    {
        if (process::current_admin())
            return true;
        regs->rax = SF_ACCESS_DENIED;
        return false;
    }

    // A device name from the program: the block device, or nullptr (and
    // rax set) when there is none.
    blkdev* device_arg(user_regs* regs)
    {
        char name[16];
        sint64_t len = uaccess::strncpy_from_user(name, regs->rdi, sizeof(name));
        if (len < 0)
        {
            regs->rax = len == -1 ? SF_INVALID_PARAMETER : SF_NOT_FOUND;
            return nullptr;
        }
        blkdev* d = block::find(name);
        if (!d)
            regs->rax = SF_NOT_FOUND;
        return d;
    }

    // Is `d` the device itself, or one of the partitions of disk `dev`?
    inline bool part_of(blkdev* d, blkdev* dev)
    {
        return d == dev || d->parent == dev;
    }

    // (SfProcessInfo* Buffer, uint64_t* Count)
    void list_processes(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        uint64_t max = 0;
        if (!uaccess::copy_from_user(&max, regs->rsi, sizeof(max)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }

        // Taken all at once so the list is one moment's, however big.
        uint64_t total = process::list_programs(nullptr, 0);
        SfProcessInfo* list = total ? (SfProcessInfo*)kmalloc(total * sizeof(SfProcessInfo))
                                    : nullptr;
        if (total && !list)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        total = process::list_programs(list, total);

        uint64_t n = total < max ? total : max;
        bool ok = (!n || uaccess::copy_to_user(regs->rdi, list, n * sizeof(SfProcessInfo))) &&
                  uaccess::copy_to_user(regs->rsi, &total, sizeof(total));
        if (list)
            kfree(list);
        regs->rax = !ok ? SF_INVALID_PARAMETER
                  : total > max ? SF_BUFFER_TOO_SMALL : SF_SUCCESS;
    }

    // (uint64_t Id)
    void end_process(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        regs->rax = regs->rdi <= 0x7FFFFFFF && process::end_program((pid_t)regs->rdi)
                  ? SF_SUCCESS : SF_NOT_FOUND;
    }

    // (const char* Device): every partition of a disk that is not mounted
    // yet, or the one partition (or a disk that is one volume).
    void mount_volume(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        blkdev* dev = device_arg(regs);
        if (!dev)
            return;

        bool whole = !dev->parent && mounts::has_partitions(dev);
        uint32_t tried = 0, failed = 0;
        for (uint32_t i = 0; block::get(i); i++)
        {
            blkdev* d = block::get(i);
            if ((whole ? d->parent != dev : d != dev) || mounts::of_device(d->name))
                continue;
            tried++;
            sint64_t rc = mounts::mount_device(d);
            if (rc == 0)
                uart::printf("mount: %s on /mount/%s\n", d->name, d->name);
            else
                uart::printf("mount: %s failed rc=%d\n", d->name, (int)rc);
            if (rc != 0)
                failed++;
        }
        regs->rax = !tried ? SF_ALREADY_EXISTS : failed ? SF_DEVICE_ERROR : SF_SUCCESS;
    }

    // (const char* Device): whatever of it is mounted.
    void unmount_volume(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        blkdev* dev = device_arg(regs);
        if (!dev)
            return;

        // What is mounted from it, gathered first: unmounting the last
        // volume of an unplugged disk takes the disk and its partitions out
        // of the block registry, `dev` included.
        mount* targets[8];
        uint32_t found = 0;
        for (uint32_t i = 0; block::get(i) && found < 8; i++)
        {
            blkdev* d = block::get(i);
            mount* m = part_of(d, dev) ? mounts::of_device(d->name) : nullptr;
            if (m)
                targets[found++] = m;
        }

        SfStatus st = SF_SUCCESS;
        for (uint32_t i = 0; i < found; i++)
        {
            char name[sizeof(targets[i]->devname)];
            strncpy(name, targets[i]->devname, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';

            sint64_t rc = mounts::unmount(targets[i]);
            uart::printf("umount: %s %s\n", rc == 0 ? "ok" : rc == -EBUSY ? "busy" : "failed",
                         name);
            if (rc == -EBUSY)
                st = SF_IN_USE;
            else if (rc == -EPERM)
                st = SF_ACCESS_DENIED;      // the boot volume stays
            else if (rc != 0 && st == SF_SUCCESS)
                st = SF_DEVICE_ERROR;
        }
        regs->rax = found ? st : SF_NOT_FOUND;
    }

    // () Both only come back when the machine could not be restarted or
    // powered off; the volumes are written back and unmounted by then.
    void restart(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        mounts::prepare_power_off();
        acpi::reboot();
    }

    void shut_down(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        mounts::prepare_power_off();
        acpi::shutdown();
        regs->rax = SF_DEVICE_ERROR;
    }

    // (uint64_t Id)
    void foreground(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        regs->rax = regs->rdi <= 0x7FFFFFFF ? process::move_to_foreground((pid_t)regs->rdi)
                                            : SF_NOT_FOUND;
    }

    void background(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        regs->rax = regs->rdi <= 0x7FFFFFFF ? process::move_to_background((pid_t)regs->rdi)
                                            : SF_NOT_FOUND;
    }

    // (SfSystemInfo* Info)
    void get_system_info(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        SfSystemInfo* info = (SfSystemInfo*)kmalloc(sizeof(SfSystemInfo));
        if (!info)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        memory::memset((uint8_t*)info, 0, sizeof(*info));

        pmm::Stats mem;
        pmm::get_stats(&mem);
        info->MemoryTotal = mem.total_frames * PAGE_SIZE_4K;
        info->MemoryFree  = mem.free_frames * PAGE_SIZE_4K;
        info->CpuCount    = smp::running();
        if (info->CpuCount > SF_MAX_CPUS)
            info->CpuCount = SF_MAX_CPUS;
        char name[51] = {};
        cpuid::get_cpu_name(name);
        const char* n = name;
        while (*n == ' ')
            n++;                        // some CPUs pad the name in front
        for (uint32_t i = 0; n[i] && i < sizeof(info->CpuName) - 1; i++)
            info->CpuName[i] = n[i];
        for (uint32_t i = 0; i < info->CpuCount; i++)
            process::cpu_times(i, &info->CpuBusy[i], &info->CpuTotal[i]);

        regs->rax = uaccess::copy_to_user(regs->rdi, info, sizeof(*info))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
        kfree(info);
    }

    // (uint64_t Id, SfProcessStats* Info)
    void get_process_info(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        SfProcessStats stats;
        if (regs->rdi > 0x7FFFFFFF || !process::program_stats((pid_t)regs->rdi, &stats))
        {
            regs->rax = SF_NOT_FOUND;
            return;
        }
        regs->rax = uaccess::copy_to_user(regs->rsi, &stats, sizeof(stats))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }

    // ()
    void sync(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        sint64_t rc = vfs::sync_all();
        uart::printf(rc == 0 ? "sync: ok\n" : "sync: failed %d\n", (int)rc);
        regs->rax = rc == 0 ? SF_SUCCESS : SF_DEVICE_ERROR;
    }

    // (const SfDateTime* Time)
    void set_time(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        SfDateTime t;
        if (!uaccess::copy_from_user(&t, regs->rdi, sizeof(t)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        if (t.Year < 2000 || t.Year > 2099 || t.Month < 1 || t.Month > 12 || t.Day < 1 ||
            t.Day > 31 || t.Hour > 23 || t.Minute > 59 || t.Second > 59)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        rtc_time r = {};
        r.year = t.Year;  r.month = t.Month;   r.day = t.Day;
        r.hours = t.Hour; r.minutes = t.Minute; r.seconds = t.Second;
        rtc::write(&r);
        regs->rax = SF_SUCCESS;
    }

    // (const char* Topic, char* Buffer, uint64_t* Size): what the kernel's
    // info command prints, captured.
    void report(user_regs* regs, iret_frame*)
    {
        if (!allowed(regs))
            return;
        char topic[64];
        uint64_t size = 0;
        if (uaccess::strncpy_from_user(topic, regs->rdi, sizeof(topic)) < 0 ||
            !uaccess::copy_from_user(&size, regs->rdx, sizeof(size)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        const uint64_t MAX = 256 * 1024;
        uint64_t cap = size < MAX ? size : MAX;
        char* buf = (char*)kmalloc(cap ? cap : 1);
        if (!buf)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }

        screen::capture(buf, cap);
        bool known = reports::report(topic);
        uint64_t need = screen::end_capture();
        if (!known)
            regs->rax = SF_NOT_FOUND;
        else if (!uaccess::copy_to_user(regs->rdx, &need, sizeof(need)) ||
                 (cap && !uaccess::copy_to_user(regs->rsi, buf, need < cap ? need : cap)))
            regs->rax = SF_INVALID_PARAMETER;
        else
            regs->rax = need > size ? SF_BUFFER_TOO_SMALL : SF_SUCCESS;
        kfree(buf);
    }
}

namespace sfadmin
{
    void init()
    {
        sfcall::set_handler(SFCALL_ADMIN_LIST_PROCESSES, list_processes);
        sfcall::set_handler(SFCALL_ADMIN_END_PROCESS, end_process);
        sfcall::set_handler(SFCALL_ADMIN_MOUNT, mount_volume);
        sfcall::set_handler(SFCALL_ADMIN_UNMOUNT, unmount_volume);
        sfcall::set_handler(SFCALL_ADMIN_RESTART, restart);
        sfcall::set_handler(SFCALL_ADMIN_SHUT_DOWN, shut_down);
        sfcall::set_handler(SFCALL_ADMIN_SYNC, sync);
        sfcall::set_handler(SFCALL_ADMIN_SET_TIME, set_time);
        sfcall::set_handler(SFCALL_ADMIN_REPORT, report);
        sfcall::set_handler(SFCALL_ADMIN_FOREGROUND, foreground);
        sfcall::set_handler(SFCALL_ADMIN_BACKGROUND, background);
        sfcall::set_handler(SFCALL_ADMIN_GET_SYSTEM_INFO, get_system_info);
        sfcall::set_handler(SFCALL_ADMIN_GET_PROCESS_INFO, get_process_info);
    }
}
