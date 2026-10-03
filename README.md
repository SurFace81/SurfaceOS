# SurfaceOS
 
A hobby x86_64 operating system written in C++ (freestanding, no OOP).
 
# Features
 
- UEFI bootloader (custom EFI loader via MinGW; passes the boot volume's
  partition start and disk signature to the kernel for automount)
- x86_64 kernel: GDT/IDT, local APIC + IOAPIC (xAPIC or x2APIC; the
  8259s without a MADT), the local APIC timer calibrated against the PIT
  for the tick, per-CPU data (GS base, swapgs), TSS and GDT per CPU,
  spin locks and one big kernel lock (one CPU in the kernel at a time),
  the other CPUs started (INIT-SIPI-SIPI), each scheduling its own
  threads (a new thread goes to the least busy CPU, 10 ms slices; the
  threads of one program run on several CPUs, with TLB shootdown),
  paging (4 KiB, per-process address spaces, W^X), ring-3 user mode
- ACPI tables without AML: reboot/shutdown (FADT, \_S5), the CPUs and
  interrupt controllers from the MADT (`acpi` command)
- Preemptive scheduler (switches only at ring-3 boundaries); a program
  is a process with threads, started, waited for, paused, moved between
  screens and ended through the SDK - its own design, no POSIX layer
- Block layer: blkdev registry (USB MSD today, AHCI/NVMe-shaped),
  GPT/MBR/superfloppy partition parsing (SurfaceOS itself lives on GPT;
  the others are for mounting ordinary sticks), LRU sector cache with
  dirty tracking and flush, 512 and 4096-byte sectors
- VFS: vnode cache, mount table (the FAT32 root and /mount/<device>),
  path resolution (`/`-separated paths, `.`, `..` across mount points,
  LFN)
- FAT32 with long file names (UTF-8, case preserved), FSInfo-based
  allocator, incremental read/write by byte offset, truncate, rename,
  unlink of open files, volume dirty bit
- Kernel objects behind per-process handle tables: open files (each
  with its offset), processes, threads, mutexes, events
- SurfaceOS SDK (<sfos.h>, how to write a program: src/sdk/README.md): a program implements SfMain(SfApp*, SfSystem*)
  and reaches the system through tables of the SDK runtime
  (src/sdk/runtime), which the kernel maps into every program and which
  enters the kernel with the syscall instruction. Sys->Files opens,
  lists, makes, removes and renames files and folders, Sys->Memory gives pages
  and a heap, Sys->Time the clock, the uptime and sleeping,
  Sys->Process starts other programs and waits for them, and gives the
  command line of a process, Sys->Thread threads
  (Create/Exit/Join), Sys->Sync mutexes and events. App->Args is the command
  line; a path in it is opened by the console and reaches the program as
  argN:. Files go through roots: data:/
  (the program's own /files/<name>, created on first start) and tmp:/
  (/tmp, unique names from CreateUnique); no path leads above its root.
- Nine screens, Alt+F1..F9, each with a system title bar (screen,
  program, subtitle, clock); keys go to the screen's input owner.
  Every screen runs CMD.BIN (/sfos), the console: a program in ring 3
  with the admin right, started again whenever it ends.
  Programs draw through the console protocol (cells, colours, cursor,
  keys, line or raw mode); Ctrl+C is a key like any other, Ctrl+Alt+C
  ends every program on the shown screen, Ctrl+Alt+Z pauses them and
  hands the keys to the console (again: they go on); Print Screen
  saves the panel as a BMP in /files/screenshots. A program that
  crashes (a CPU exception) ends with a line saying why and where, on
  its screen and in its log. A program on the shown screen gets twice the CPU time of
  one elsewhere
- The console (CMD.BIN, `help`): ls, cat, xxd, write, cp, mv, rm, mkdir,
  rmdir, cd, mount <dev> (partitions go to /mount/<dev>pN), umount
  <dev|dir>, sync, time, settime, uptime, reboot, shutdown and hardware
  info (lsblk, meminfo, cpuid, lspci, lsusb, usbports, usbinfo, acpi,
  dmesg); a program runs by its name (looked up in /apps) or by its path,
  on a cleared screen, or with `&` before it in the background (a hidden
  screen, its output logged to /files/<name>/console_<date>_<time>.log);
  ps lists the programs, kill <id> ends one, bg sends the paused one to
  the background and fg brings it (or any by its id) back to the screen;
  output longer than the screen stops at a ";" line: PageUp/PageDown move
  a page, the arrows a line, q leaves;
  a name or path with spaces goes in quotes ("my file.txt"); Up and
  Down bring back the last lines typed; `a > file` (`>>` adds at the
  end) puts what a prints into the file - a program's too - and
  `a | b` hands it to the filters grep, head, tail and wc; while a word
  is typed, the rest of a command, program or name that starts with it
  shows dimmed, and Tab takes it;
  `sudo <program>` runs it with the admin right (Sys->Admin: processes,
  mount/unmount, restart, power off; roots disk:/ and mount:/)
- explorer (`sudo explorer`): a file manager of two panels - copy, move,
  rename and delete files and whole folders, also between volumes; an
  editor that shows a file as text (lines numbered) or as hex (rows under
  their offsets), with undo, a selection and a clipboard, and finding;
  volumes mounted and unmounted from a menu, files found by name and
  content, quick view, bookmarks, the folders compared; F1 lists the keys
- Programs: taskmgr (`sudo taskmgr`: memory, the load of every CPU and
  the running programs with their CPU share, CPU time, memory and threads,
  live; Del ends the chosen one), hello (Console Print and ReadLine), sdkcheck (the SDK
  tables, memory, time, arguments, threads, mutexes and events, starting programs;
  `sdkcheck input` hands its keys to a child, `sdkcheck keys` shows
  what ReadKey reports, `sdkcheck box` draws the box characters, `sdkcheck ticks` ticks to be paused, `sdkcheck
  spin` shows its share of a CPU, `sudo sdkcheck admin` checks the admin
  right), sfstest (files through the SDK and
  the roots' sandbox; `sfstest verify` after a restart), threadtest
  (how a program with several threads ends: fault, Ctrl+Alt+C, last
  exit; Ctrl+Alt+C ends the programs it started too; `threadtest stress` runs many
  threads over every CPU)
 
# Project Structure
 
```
src/
  boot/       — UEFI bootloader (C, MinGW)
  kernel/     — kernel source (C++, freestanding)
    cpu/      — GDT, IDT, IRQ, paging, PCI, the SDK calls (sfcall,
                sffile, sfconsole, sfadmin, ...), process, ELF loader,
                uaccess
    dev/      — blkdev registry, partition parsing, block cache
    drivers/  — screen, term (the screens' text), keyboard, tty (key
                queues), reports, UART, USB/xHCI, PIT, RTC
    fs/       — VFS core (vnode/mount/path lookup), open files and
                fileio, mounts, fat32/ (fat.cpp, dir.cpp, vnode.cpp)
    mm/       — physical memory, heap
    stdlib/   — stdio, string
  include/    — kernel-side headers (cpu/, dev/, fs/, drivers/, mm/)
  sdk/        — include/sfos.h + sfos/ (the SDK), abi/ (what the
                runtime and the kernel share), runtime/ (the code behind
                the SDK tables), sfos.ld (program link script)
  sfos/       — cmd.cpp: the console, /sfos/CMD.BIN
  apps/       — programs: <name>.cpp is one program, <name>/ is one
                program of all the .cpp files in it
tools/
  mkimg.py            — image builder: GPT, one FAT32 EFI System Partition
  qemu_exec_test.sh   — full QEMU regression suite
  qemu_verify.sh      — reboot + persistence + host fsck.fat
  *test_host.sh       — host unit tests: objects, term, tty key queues
```

# Build & Run

1. Install Linux (Ubuntu)
2. Enable i386 (if you have a 64-bit system):

    `sudo dpkg --add-architecture i386`

    `sudo apt update`

3. Install dependencies:

    `sudo apt install qemu-system-x86 nasm gparted okteta make git libc6:i386 libncurses6:i386 libstdc++6:i386 gcc-mingw-w64-x86-64 dosfstools sfdisk gdisk python3-pip ovmf`

    `pip3 install pyfatfs`

4. Install Cross Compiler (https://wiki.osdev.org/GCC_Cross-Compiler)
    - Extract opt.tar.xz in $HOME
    - Add to PATH permanently: `echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc`
    - Reload: `source ~/.bashrc`

5. Open SurfaceOS folder: 
    - Create folder `./tmp`
    - Run in terminal: `make run`

# Tests

- `bash tools/qemu_exec_test.sh` — boots QEMU, runs sdkcheck, sfstest,
  threadtest (stress run included), mount/umount on a second disk and a leak check, asserts on the serial
  log.
- `bash tools/qemu_verify.sh` — after the suite: reboots its image, runs
  `sfstest verify`, host `fsck.fat -n`.
- `bash tools/objtest_host.sh`, `termtest_host.sh`, `ttytest_host.sh` —
  unit tests of kernel parts on the host, in milliseconds.

# Writing to a real stick

`make usb DEV=/dev/sdX` (GPT; the whole device is overwritten)
