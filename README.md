# SurfaceOS
 
A hobby x86_64 operating system written in C++ (freestanding, no OOP).
Its programs are written in C or C++.
 
# Features

### Boot

- UEFI bootloader: a custom EFI loader built with MinGW
- Passes the boot volume's partition start and disk signature to the
  kernel, which mounts it by itself

### Kernel

- x86_64, ring-3 user mode
- GDT/IDT; TSS and GDT per CPU; per-CPU data (GS base, swapgs)
- Interrupts: local APIC + IOAPIC (xAPIC or x2APIC); the 8259s when there
  is no MADT
- Tick: the local APIC timer, calibrated against the PIT
- SMP: the other CPUs started with INIT-SIPI-SIPI, each scheduling its own
  threads
- Locking: spin locks and one big kernel lock (one CPU in the kernel at a
  time)
- Paging: 4 KiB pages, an address space per process, W^X
- ACPI tables without AML:
  - reboot and shutdown (FADT, `\_S5`)
  - the CPUs and interrupt controllers from the MADT (`acpi` command)

### Processes and scheduling

- Preemptive scheduler; switches only at ring-3 boundaries
- 10 ms slices; a new thread goes to the least busy CPU
- The threads of one program run on several CPUs, with TLB shootdown
- A program on the shown screen gets twice the CPU time of one elsewhere
- A program is a process with threads: started, waited for, paused, moved
  between screens and ended through the SDK - its own design, no POSIX
  layer
- Kernel objects behind per-process handle tables: open files (each with
  its offset), processes, threads, mutexes, events
- A program that crashes (a CPU exception) ends with a line saying why and
  where, on its screen and in its log

### Storage

- Block layer:
  - blkdev registry (USB MSD today, AHCI/NVMe-shaped)
  - partitions: GPT (SurfaceOS itself lives on it), MBR and superfloppy
    (for ordinary sticks)
  - LRU sector cache with dirty tracking and flush
  - 512 and 4096-byte sectors
- VFS:
  - vnode cache
  - mount table: the FAT32 root and /mount/<device>
  - paths: `/`-separated, `.` and `..` across mount points, long names
- FAT32:
  - long file names (UTF-8, case preserved)
  - FSInfo-based allocator
  - read and write by byte offset, truncate, rename
  - unlink of open files, volume dirty bit

### SDK

How to write a program, and every table in detail:
[src/sdk/README.md](src/sdk/README.md).

- One header, `<sfos.h>`; programs in C or C++ alike implement
  `SfMain(SfApp*, SfSystem*)`
- The system is reached through tables of the SDK runtime
  (src/sdk/runtime): the kernel maps it into every program, and it enters
  the kernel with the `syscall` instruction
- The tables:
  - `Sys->Console`: text, cells and colours, keys, line or raw mode, the
    title, the clipboard
  - `Sys->Files`: open, list, make, remove and rename files and folders
  - `Sys->Memory`: pages and a heap
  - `Sys->Time`: the clock, the uptime, sleeping
  - `Sys->Process`: start other programs, wait for them, command lines
  - `Sys->Thread`: Create, Exit, Join
  - `Sys->Sync`: mutexes, events, and WaitAny (the first of events,
    programs or threads ending and a key, with a timeout)
  - `Sys->Admin`: only with the admin right
- `App->Args` is the command line; a path in it is opened by the console
  and reaches the program as `argN:`
- A small libc of the plain C functions (strings, numbers, `snprintf`,
  `qsort`), linked into every program; no system calls in it
- Programs in C can be built in SurfaceOS itself, with tcc (Ports)
- Files go through roots; no path leads above its root:
  - `data:/`: the program's own /files/<name>, created on first start
  - `tmp:/`: /tmp, unique names from CreateUnique

### Screens

- Nine screens, Alt+F1..F9, each with a system title bar: screen,
  program, subtitle, clock
- Keys go to the screen's input owner
- Programs draw through the console: cells, colours, cursor, keys, line or
  raw mode
- Keys of the system:
  - Ctrl+C is a key like any other
  - Ctrl+Alt+C ends every program on the shown screen
  - Ctrl+Alt+Z pauses them and hands the keys to the console (again: they
    go on)
  - Print Screen saves the screen as a BMP in /files/screenshots

### The console (CMD.BIN)

Every screen runs /sfos/CMD.BIN: a program in ring 3 with the admin right,
started again whenever it ends. `help` lists its commands.

- Files: ls, cat, xxd, write, cp, mv, rm, mkdir, rmdir, cd
- Volumes: mount <dev> (partitions go to /mount/<dev>pN), umount
  <dev|dir>, sync
- Time and power: time, settime, uptime, reboot, shutdown
- Hardware: lsblk, meminfo, cpuid, lspci, lsusb, usbports, usbinfo, acpi,
  dmesg
- Programs:
  - run by name (looked up in /apps) or by path, on a cleared screen
  - `& <program>`: in the background, on a hidden screen, its output
    logged to /files/<name>/console_<date>_<time>.log
  - `sudo <program>`: with the admin right (Sys->Admin: processes,
    mount/unmount, restart, power off; the roots disk:/ and mount:/)
  - ps lists them, kill <id> ends one
  - bg sends the paused one to the background, fg brings it (or any by
    its id) back
- Pipes and redirection:
  - `a > file` puts what a prints into the file (`>>` adds at the end) -
    a program's output too
  - `a | b` hands it to the filters grep, head, tail and wc, or to a
    program, whose ReadLine reads it a line at a time
- Typing:
  - a name or path with spaces goes in quotes ("my file.txt")
  - Up and Down bring back the last lines typed
  - while a word is typed, the rest of a command, program or name that
    starts with it shows dimmed; Tab takes it
  - Ctrl+arrows select text on the screen, Ctrl+C copies it to the
    clipboard (one for every screen and program), Esc drops it, Ctrl+V
    types it
- Output longer than the screen stops at a ";" line: PageUp/PageDown move
  a page, the arrows a line, q leaves

### Programs

- explorer (`sudo explorer`), a file manager of two panels:
  - copy, move, rename and delete files and whole folders, also between
    volumes
  - an editor: a file as text (lines numbered) or as hex (rows under their
    offsets), with undo, a selection, a clipboard and finding
  - volumes mounted and unmounted from a menu
  - files found by name and content, quick view, bookmarks, folders
    compared
  - F1 lists the keys
- taskmgr (`sudo taskmgr`): memory, the load of every CPU and the running
  programs with their CPU share, CPU time, memory and threads, live; Del
  ends the chosen one
- hello: Console Print and ReadLine; hello_c: the same SDK from C
- tcc: the C compiler, built in (Ports)
- libctest: the SDK's libc
- sdkcheck: the SDK tables, memory, time, arguments, threads, mutexes and
  events, starting programs
  - `sdkcheck input` hands its keys to a child
  - `sdkcheck keys` shows what ReadKey reports
  - `sdkcheck box` draws the box characters
  - `sdkcheck ticks` ticks to be paused
  - `sdkcheck spin` shows its share of a CPU
  - `sudo sdkcheck admin` checks the admin right
- sfstest: files through the SDK and the roots' sandbox; `sfstest verify`
  after a restart
- threadtest: how a program with several threads ends (fault,
  Ctrl+Alt+C, last exit; Ctrl+Alt+C ends the programs it started too);
  `threadtest stress` runs many threads over every CPU

### Ports

Programs from elsewhere, changed to run on the SDK: `ports/<name>/`, each
with a README of what it is, where it comes from and what was changed.

- tcc ([ports/tcc](ports/tcc/README.md)): TinyCC 0.9.27, a C compiler that
  runs in SurfaceOS and builds SurfaceOS programs:
  - `tcc hello.c -o hello.bin`: one file
  - `tcc /demo`: a project - every `.c` in the folder and the folders in
    it, into `/demo/demo.bin`
  - its headers (tcc's, the SDK's, libc's) and `libc.a`, `libtcc1.a` are
    in its data folder /files/tcc
  - `/demo`: a project of three files to try it on
 
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
                the SDK tables), libc/ (the C functions), sfos.ld
                (program link script)
  sfos/       — cmd.cpp: the console, /sfos/CMD.BIN
  apps/       — programs: <name>.cpp or <name>.c is one program,
                <name>/ is one program of all the .cpp and .c files in it
ports/
  tcc/        — TinyCC: tinycc/ (its source, changed for SurfaceOS),
                sfport.c (its start and system calls), demo/ (a project)
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
  threadtest (stress run included), mount/umount on a second disk, a leak
  check and `tcc /demo`, asserts on the serial log.
- `bash tools/qemu_verify.sh` — after the suite: reboots its image, runs
  `sfstest verify`, host `fsck.fat -n`.
- `bash tools/objtest_host.sh`, `termtest_host.sh`, `ttytest_host.sh` —
  unit tests of kernel parts on the host, in milliseconds.

# Writing to a real stick

`make usb DEV=/dev/sdX` (GPT; the whole device is overwritten)
