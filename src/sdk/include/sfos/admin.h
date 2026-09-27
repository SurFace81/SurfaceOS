#ifndef SFOS_ADMIN_H
#define SFOS_ADMIN_H

#include "table.h"
#include "time.h"

// What a program started with the admin right can do beyond everyone else
// (the console is one): Sys->Admin, nullptr for any other program. The
// kernel checks the right on every call, so calling past the table gets
// SF_ACCESS_DENIED all the same. Such a program also has two more roots:
// disk:/ - the whole boot volume - and mount:/, the other volumes.
//
//   ListProcesses  the running programs into Buffer: *Count in: how many
//                  fit; out: how many there are. SF_BUFFER_TOO_SMALL (with
//                  *Count set) when they do not fit.
//   EndProcess     end program Id at once, however busy it is (as
//                  Ctrl+Alt+C would). SF_NOT_FOUND for no such program.
//   Mount          put Device under mount:/ - every partition of a disk
//                  ("usb1": mount:/usb1p1, mount:/usb1p2, ...) or one
//                  partition ("usb1p2"). SF_NOT_FOUND for no such device,
//                  SF_ALREADY_EXISTS when it is all mounted already,
//                  SF_DEVICE_ERROR when a partition holds no volume it can
//                  read.
//   Unmount        write back and take away what Mount put there for Device
//                  (a disk or one partition). SF_IN_USE while a file on it
//                  is open, SF_NOT_FOUND when nothing of it is mounted.
//   Restart        write every volume back and restart the machine.
//   ShutDown       the same, and power it off. Both come back only when
//                  they fail (SF_DEVICE_ERROR).
//   Sync           (revision 1.1) write everything cached back to the disks.
//   SetTime        set the machine's clock.
//   Report         what the kernel has to say on Topic, as text into Buffer,
//                  NUL-terminated: *Size in: its size; out: the bytes the
//                  report takes (SF_BUFFER_TOO_SMALL when it does not fit).
//                  Topics, each with its arguments after a space: "cpuid",
//                  "lspci", "lsusb", "usbports", "usbinfo <index>", "lsblk",
//                  "mount" (what is mounted where), "acpi", "meminfo",
//                  "dmesg" (the kernel's log). SF_NOT_FOUND for others.
typedef struct SfAdmin SfAdmin;

typedef struct SfProcessInfo
{
    uint64_t Id;
    uint32_t Screen;            // 1..9, 0 in the background
    uint32_t Paused;            // 1 when paused (Ctrl+Alt+Z)
    char     Name[32];
} SfProcessInfo;

struct SfAdmin
{
    SfTableHeader Hdr;
    SfStatus (*ListProcesses)(SfAdmin* This, SfProcessInfo* Buffer, uint64_t* Count);
    SfStatus (*EndProcess)(SfAdmin* This, uint64_t Id);
    SfStatus (*Mount)(SfAdmin* This, const char* Device);
    SfStatus (*Unmount)(SfAdmin* This, const char* Device);
    SfStatus (*Restart)(SfAdmin* This);
    SfStatus (*ShutDown)(SfAdmin* This);
    // Revision 1.1
    SfStatus (*Sync)(SfAdmin* This);
    SfStatus (*SetTime)(SfAdmin* This, const SfDateTime* Time);
    SfStatus (*Report)(SfAdmin* This, const char* Topic, char* Buffer, uint64_t* Size);
};

#define SF_ADMIN_SIGNATURE      SF_SIGNATURE('S', 'F', 'A', 'D', 'M', 'I', 'N', 0)
#define SF_ADMIN_REVISION       SF_REVISION(1, 1)

SF_STATIC_ASSERT(sizeof(SfProcessInfo) == 48, "SfProcessInfo layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfAdmin, ListProcesses) == 16, "SfAdmin layout");
SF_STATIC_ASSERT(sizeof(SfAdmin) == 88, "SfAdmin layout");

#endif // SFOS_ADMIN_H
