#ifndef SFOS_ADMIN_H
#define SFOS_ADMIN_H

#include "table.h"
#include "time.h"

/// A running program, as ListProcesses lists it.
typedef struct SfProcessInfo
{
    uint64_t Id;                ///< the program's number
    uint32_t Screen;            ///< 1..9, 0 in the background
    uint32_t Paused;            ///< 1 when paused (Ctrl+Alt+Z)
    char     Name[32];          ///< the program's name, NUL-terminated
} SfProcessInfo;

/// The most CPUs SfSystemInfo describes.
#define SF_MAX_CPUS             64

/// The memory and the CPUs, as GetSystemInfo reports them.
typedef struct SfSystemInfo
{
    uint64_t MemoryTotal;           ///< bytes of RAM the system manages
    uint64_t MemoryFree;            ///< of it, bytes nobody uses
    uint32_t CpuCount;              ///< CPUs running
    uint32_t Reserved;
    char     CpuName[48];           ///< NUL-terminated
    uint64_t CpuBusy[SF_MAX_CPUS];  ///< ms each CPU has worked since it started
    uint64_t CpuTotal[SF_MAX_CPUS]; ///< ms each CPU has run, working or idle
} SfSystemInfo;

/// SfProcessStats Flags: the program is paused (Ctrl+Alt+Z).
#define SF_PROCESS_PAUSED       0x1
/// SfProcessStats Flags: the program has the admin right.
#define SF_PROCESS_ADMIN        0x2

/// What one program uses, as GetProcessInfo reports it.
typedef struct SfProcessStats
{
    uint64_t Id;                    ///< the program's number
    uint64_t ParentId;              ///< 0 when the one that started it is gone
    uint64_t CpuTime;               ///< ms it has run, all its threads together
    uint64_t Memory;                ///< bytes of memory of its own
    uint32_t Threads;               ///< threads it has now
    uint32_t Screen;                ///< as SfProcessInfo
    uint32_t Flags;                 ///< SF_PROCESS_*
    uint32_t Reserved;
    char     Name[32];              ///< the program's name, NUL-terminated
} SfProcessStats;

/// What a program started with the admin right can do beyond everyone
/// else (the console is one): Sys->Admin, null for any other program.
///
/// The kernel checks the right on every call, so calling past the table gets
/// SF_ACCESS_DENIED all the same. Such a program also has two more roots:
/// disk:/ - the whole boot volume - and mount:/, the other volumes.
typedef struct SfAdmin SfAdmin;

struct SfAdmin
{
    SfTableHeader Hdr;

    /// Copies the running programs into Buffer: *Count in: how many
    /// fit; out: how many there are.
    ///
    /// SF_BUFFER_TOO_SMALL (with *Count set) when they do not fit.
    SfStatus (*ListProcesses)(SfAdmin* This, SfProcessInfo* Buffer, uint64_t* Count);

    /// Ends program Id at once, however busy it is (as Ctrl+Alt+C
    /// would).
    ///
    /// SF_NOT_FOUND for no such program.
    SfStatus (*EndProcess)(SfAdmin* This, uint64_t Id);

    /// Puts Device under mount:/ - every partition of a disk ("usb1":
    /// mount:/usb1p1, mount:/usb1p2, ...) or one partition ("usb1p2").
    ///
    /// SF_NOT_FOUND for no such device, SF_ALREADY_EXISTS when it is all
    /// mounted already, SF_DEVICE_ERROR when a partition holds no volume it
    /// can read.
    SfStatus (*Mount)(SfAdmin* This, const char* Device);

    /// Writes back and takes away what Mount put there for Device (a
    /// disk or one partition).
    ///
    /// SF_IN_USE while a file on it is open, SF_NOT_FOUND when nothing of it
    /// is mounted.
    SfStatus (*Unmount)(SfAdmin* This, const char* Device);

    /// Writes every volume back and restarts the machine.
    ///
    /// Comes back only when it fails (SF_DEVICE_ERROR).
    SfStatus (*Restart)(SfAdmin* This);

    /// Writes every volume back and powers the machine off.
    ///
    /// Comes back only when it fails (SF_DEVICE_ERROR).
    SfStatus (*ShutDown)(SfAdmin* This);

    /// Writes everything cached back to the disks.
    SfStatus (*Sync)(SfAdmin* This);

    /// Sets the machine's clock.
    SfStatus (*SetTime)(SfAdmin* This, const SfDateTime* Time);

    /// What the kernel has to say on Topic, as text into Buffer, NUL-
    /// terminated: *Size in: its size; out: the bytes the report takes
    /// (SF_BUFFER_TOO_SMALL when it does not fit).
    ///
    /// Topics, each with its arguments after a space: "cpuid", "lspci",
    /// "lsusb", "usbports", "usbinfo <index>", "lsblk", "mount" (what is
    /// mounted where), "acpi", "meminfo", "dmesg" (the kernel's log).
    /// SF_NOT_FOUND for others.
    SfStatus (*Report)(SfAdmin* This, const char* Topic, char* Buffer, uint64_t* Size);

    /// Program Id - and everything else on its screen but the console
    /// there - moves to the caller's screen, with what that screen shows, and
    /// gets its keys (back to the caller when it ends); paused, it goes on.
    ///
    /// SF_IN_USE when other programs are on the caller's screen,
    /// SF_ACCESS_DENIED for a console or from the background.
    SfStatus (*Foreground)(SfAdmin* This, uint64_t Id);

    /// As Foreground, but onto a hidden screen of its own, what it
    /// prints logged as with SF_START_BACKGROUND (sfos/process.h); the screen
    /// it leaves goes back to its console.
    SfStatus (*Background)(SfAdmin* This, uint64_t Id);

    /// *Info gets the memory and the CPUs, as they are now.
    ///
    /// A CPU's load over a time is how much its Busy grew against its Total.
    SfStatus (*GetSystemInfo)(SfAdmin* This, SfSystemInfo* Info);

    /// *Info gets what program Id uses: its threads, CPU time and
    /// memory.
    ///
    /// SF_NOT_FOUND for no such program.
    SfStatus (*GetProcessInfo)(SfAdmin* This, uint64_t Id, SfProcessStats* Info);
};

#define SF_ADMIN_SIGNATURE      SF_SIGNATURE('S', 'F', 'A', 'D', 'M', 'I', 'N', 0)

SF_STATIC_ASSERT(sizeof(SfProcessInfo) == 48, "SfProcessInfo layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfAdmin, ListProcesses) == 16, "SfAdmin layout");
SF_STATIC_ASSERT(sizeof(SfSystemInfo) == 1096, "SfSystemInfo layout");
SF_STATIC_ASSERT(sizeof(SfProcessStats) == 80, "SfProcessStats layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfAdmin, GetSystemInfo) == 104, "SfAdmin layout");
SF_STATIC_ASSERT(sizeof(SfAdmin) == 120, "SfAdmin layout");

#endif // SFOS_ADMIN_H
